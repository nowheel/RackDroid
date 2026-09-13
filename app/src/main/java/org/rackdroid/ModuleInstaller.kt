package org.rackdroid

import android.app.Activity
import android.os.Build
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.OutputStream
import java.io.RandomAccessFile
import java.util.zip.ZipInputStream
import org.json.JSONObject

/** On-demand / side-loadable module packs.
 *
 * WHY this exists: the base APK ships a curated core; extra module packs can
 * be added without rebuilding the app. A pack is a ".rdmod" file (a plain
 * zip) built against this RackDroid engine, containing at its top level:
 *   plugin.json, res/… , libplugin_<name>.so
 *
 * WHERE the user drops them: the app's own external folder
 *   Android/data/org.rackdroid/files/Modules/
 * (user-visible in any file manager, no storage permission needed).
 *
 * HOW they load: a plugin's native library can't be dlopen'd straight from
 * shared storage on modern Android (linker namespace + W^X). So on startup
 * we extract each pack into app-PRIVATE storage (filesDir/user/plugins/<slug>),
 * call Java System.load() on its .so — the one path that satisfies the app
 * classloader's linker namespace — then hand the already-loaded library to
 * the native side (nativeLoadUserPlugin → dlopen RTLD_NOLOAD → register).
 *
 * NOTE for Play: distributing native code that executes from outside Google
 * Play violates Play policy, so this feature is for the sideload/GitHub
 * build. The Play build should deliver extra packs via Play asset packs. */
object ModuleInstaller {
	private const val MAX_PACK_BYTES = 256L * 1024 * 1024
	private const val MAX_MANIFEST_BYTES = 1024L * 1024
	private const val MAX_ENTRY_BYTES = 128L * 1024 * 1024
	private const val MAX_UNPACKED_BYTES = 512L * 1024 * 1024
	private const val MAX_ENTRIES = 10_000
	private const val MAX_ENTRY_NAME_CHARS = 512
	/** Packs one bundle may hold. The published all_rdmods.zip carries
	 * twenty-one; the cap only stops a hostile archive from making the import
	 * loop run for a very long time. */
	private const val MAX_BUNDLE_PACKS = 128
	private val VALID_SLUG = Regex("[A-Za-z0-9][A-Za-z0-9._-]{0,63}")
	private val VALID_SONAME = Regex("libplugin_[A-Za-z0-9._-]+\\.so")

	/** Public, user-visible drop folder. Created with a README on first run. */
	fun modulesDir(activity: Activity): File =
		File(activity.getExternalFilesDir(null), "Modules").apply { mkdirs() }

	private fun installedDir(activity: Activity): File =
		File(activity.filesDir, "user/plugins").apply { mkdirs() }

	/** Import any new packs from the drop folder, then load every installed
	 * pack. Call once at startup, after the native engine has initialised
	 * and before the module list JSON is built. */
	fun loadUserPlugins(activity: Activity): Int {
		// The README is a convenience for the user, never a prerequisite. The
		// drop folder can end up owned by another uid (created by adb, a file
		// manager, a backup tool), which makes this write throw -- that must
		// not abort the import/load below, which is the whole point of this
		// call and does not need to write there at all.
		// Phase timings: this call blocks native startup (loadUserPluginsBlocking
		// in jni_bridge.cpp waits for it before the patch can be restored), and
		// it measured over a second on an S22 with no packs installed at all --
		// so where the time goes is worth stating rather than assuming.
		val tReadme = android.os.SystemClock.uptimeMillis()
		runCatching { writeReadme(activity) }
			.onFailure { jlog("could not write Modules/README.txt: ${it.message}") }
		val tImport = android.os.SystemClock.uptimeMillis()
		importNewPacks(activity)
		val tLoad = android.os.SystemClock.uptimeMillis()
		var loaded = 0
		installedDir(activity).listFiles { f -> f.isDirectory }?.forEach { dir ->
			if (loadInstalled(activity, dir)) loaded++
		}
		jlog("loadUserPlugins: readme ${tImport - tReadme} ms, import ${tLoad - tImport} ms, " +
			"load ${android.os.SystemClock.uptimeMillis() - tLoad} ms ($loaded packs)")
		if (loaded > 0)
			activity.runOnUiThread {
				android.widget.Toast.makeText(activity,
					activity.getString(R.string.modules_loaded, loaded),
					android.widget.Toast.LENGTH_SHORT).show()
			}
		return loaded
	}

	/** An installed pack: its plugin slug (== the extracted dir name) and the
	 * on-disk size of its extracted copy, for the manager UI. */
	data class InstalledPack(val slug: String, val dir: File, val sizeBytes: Long)

	/** Every pack currently extracted into private storage (i.e. loaded at the
	 * last startup, or just installed live). Sorted by slug. */
	fun installedPacks(activity: Activity): List<InstalledPack> =
		installedDir(activity).listFiles { f -> f.isDirectory && isValidSlug(f.name) }
			?.map { InstalledPack(it.name, it, dirSize(it)) }
			?.sortedBy { it.slug.lowercase() }
			?: emptyList()

	/** Uninstall a pack: remove its extracted copy AND the source .rdmod in the
	 * drop folder (so it does not re-import next launch). The native library is
	 * already loaded into the running engine and cannot be safely unregistered
	 * mid-session, so the module only fully disappears after a restart -- the
	 * caller tells the user. Returns true if anything was removed. */
	fun uninstall(activity: Activity, slug: String): Boolean {
		if (!isValidSlug(slug)) {
			jlog("refusing to uninstall invalid module slug")
			return false
		}
		var removed = destinationForSlug(activity, slug).deleteRecursively()
		// Drop any source pack in Modules/ whose plugin.json slug matches.
		modulesDir(activity).listFiles { f ->
			f.isFile && (f.name.endsWith(".rdmod") || f.name.endsWith(".zip"))
		}?.forEach { pack ->
			if (runCatching { peekSlug(pack) }.getOrNull() == slug)
				if (pack.delete()) removed = true
		}
		jlog("uninstalled module pack $slug (removed=$removed)")
		return removed
	}

	private fun dirSize(dir: File): Long =
		dir.walkTopDown().filter { it.isFile }.map { it.length() }.sum()

	/** Unpack an archive that holds packs instead of being one.
	 *
	 * The release publishes all_rdmods.zip beside the individual files, and
	 * handing it to the app did nothing whatsoever: it has no plugin.json of
	 * its own, so peekSlug returned null and the import below skipped it
	 * without a word -- the file just sat in the drop folder. Expanding it here
	 * means the user installs the one file they downloaded instead of
	 * extracting twenty-one by hand and multi-selecting them in the picker.
	 *
	 * Only top-level .rdmod entries are taken, so a bundle cannot smuggle in a
	 * path; each is written under a temporary name so a half-copied pack never
	 * sits there looking importable; and the whole expansion is capped, because
	 * a zip that claims to hold module packs is exactly where a decompression
	 * bomb would arrive. The packs it produces are then imported by the normal
	 * path, with the same validation as any other pack -- this only unwraps.
	 *
	 * The bundle is deleted once it has given up its contents: it is a
	 * seventy-megabyte duplicate of files now sitting next to it, and keeping
	 * it means reading all of it again at every launch. */
	private fun expandBundles(activity: Activity) {
		val dir = modulesDir(activity)
		val root = dir.canonicalFile
		// Same file set as the import below, not just *.zip: the picker appends
		// .rdmod to anything whose name does not already end in one of the two
		// (safeIncomingName), so a bundle can perfectly well arrive called
		// .rdmod. What decides is what is inside, and a real pack is ruled out
		// by its manifest in one central-directory lookup.
		val bundles = dir.listFiles { f ->
			f.isFile && (f.name.endsWith(".rdmod") || f.name.endsWith(".zip"))
		} ?: return
		for (bundle in bundles) {
			try {
				var written = 0
				var packs = 0
				var totalBytes = 0L
				var isPack = false
				// ZipFile, not the ZipInputStream the rest of this file uses:
				// classifying a bundle by streaming means inflating all of it
				// just to learn there is no plugin.json, and then reading it a
				// second time to extract. The central directory answers both
				// questions without touching the compressed data.
				java.util.zip.ZipFile(bundle).use { zf ->
					// A .zip that is itself a pack belongs to the normal import.
					if (zf.getEntry("plugin.json") != null) {
						isPack = true
						return@use
					}
					for (e in zf.entries()) {
						val name = e.name
						if (e.isDirectory || !name.endsWith(".rdmod", ignoreCase = true) ||
							name.contains('/') || name.contains('\\'))
							continue
						validateEntryName(name)
						if (++packs > MAX_BUNDLE_PACKS)
							throw IllegalArgumentException("too many packs in bundle")
						val dest = File(root, name).canonicalFile
						if (dest.parentFile != root)
							throw IllegalArgumentException("bundle entry escapes the drop folder")
						// Never overwrite: a pack already sitting there is
						// either installed or something the user put there.
						if (dest.exists()) continue
						val remaining = MAX_UNPACKED_BYTES - totalBytes
						if (remaining <= 0) throw IllegalArgumentException("bundle is too large")
						val tmp = File(root, "$name.part")
						tmp.delete()
						zf.getInputStream(e).use { input ->
							tmp.outputStream().use {
								totalBytes += copyLimited(input, it, minOf(MAX_PACK_BYTES, remaining))
							}
						}
						if (tmp.renameTo(dest)) written++ else tmp.delete()
					}
				}
				if (written > 0) {
					jlog("expanded ${bundle.name} into $written module pack(s)")
					if (!bundle.delete())
						jlog("could not remove ${bundle.name} after expanding it")
				} else if (!isPack && packs == 0) {
					jlog("${bundle.name} is neither a pack nor a bundle of packs")
				}
			} catch (t: Throwable) {
				jlog("bundle ${bundle.name} could not be expanded: ${t.message}")
			}
		}
	}

	/** name|size|mtime -> slug, so a pack already seen need not be opened again
	just to ask what it is called. peekSlug() reads plugin.json out of the
	archive, which measured 887 ms across 22 packs (75 MB) sitting in the drop
	folder on an S22 -- paid on every launch, to conclude every time that they
	are all installed already. The key changes if the file does, so replacing a
	.rdmod re-reads it.

	Deliberately caches the SLUG and not the decision: whether to import is
	still answered by asking if the destination exists. Caching that instead
	would quietly change what uninstalling means -- today removing an installed
	pack while its .rdmod is still in the drop folder brings it back on the next
	launch, and that is not a behaviour to alter as a side effect of making
	startup faster. */
	private fun slugIndexFile(activity: Activity) = File(activity.filesDir, "user/.pack-slugs")

	private fun readSlugIndex(activity: Activity): MutableMap<String, String> {
		val map = mutableMapOf<String, String>()
		runCatching {
			val f = slugIndexFile(activity)
			if (!f.isFile) return@runCatching
			f.forEachLine { line ->
				val i = line.lastIndexOf('|')
				if (i > 0) map[line.substring(0, i)] = line.substring(i + 1)
			}
		}
		return map
	}

	private fun writeSlugIndex(activity: Activity, map: Map<String, String>) {
		runCatching {
			slugIndexFile(activity).parentFile?.mkdirs()
			slugIndexFile(activity).writeText(
				map.entries.joinToString("\n") { "${it.key}|${it.value}" })
		}
	}

	private fun packKey(f: File) = "${f.name}|${f.length()}|${f.lastModified()}"

	private fun importNewPacks(activity: Activity) {
		// Turn any bundle into loose packs first, so the scan below sees them.
		val tExpand = android.os.SystemClock.uptimeMillis()
		expandBundles(activity)
		val tScan = android.os.SystemClock.uptimeMillis()
		val dir = modulesDir(activity)
		// listFiles() returns null when the folder is missing or unreadable
		// (again: possible when another uid created it). Silently treating
		// that as "no packs to import" hides the one failure the user would
		// actually need to act on, so say it out loud.
		val packs = dir.listFiles { f ->
			f.isFile && (f.name.endsWith(".rdmod") || f.name.endsWith(".zip"))
		}
		if (packs == null) {
			jlog("cannot read module drop folder $dir - no packs imported")
			return
		}
		val slugIndex = readSlugIndex(activity)
		val seen = mutableMapOf<String, String>()
		for (pack in packs) {
			var tmp: File? = null
			try {
				val key = packKey(pack)
				val slug = slugIndex[key] ?: peekSlug(pack) ?: continue
				seen[key] = slug
				val dest = destinationForSlug(activity, slug)
				if (dest.exists()) continue // already installed; drop a new slug to add
				tmp = File(dest.parentFile, "$slug.tmp")
				tmp.deleteRecursively()
				unzip(pack, tmp, slug)
				if (!tmp.renameTo(dest))
					throw IllegalStateException("could not finalize extracted pack")
				jlog("installed module pack $slug from ${pack.name}")
			} catch (t: Throwable) {
				jlog("module pack ${pack.name} import failed: ${t.message}")
				tmp?.deleteRecursively()
			}
		}
		// Rewritten from what this pass actually saw, so packs the user has
		// since deleted drop out instead of accumulating forever.
		if (seen != slugIndex)
			writeSlugIndex(activity, seen)
		jlog("importNewPacks: expandBundles ${tScan - tExpand} ms, scan+import " +
			"${android.os.SystemClock.uptimeMillis() - tScan} ms (${packs.size} files, " +
			"${slugIndex.size} cached slugs)")
	}

	private fun loadInstalled(activity: Activity, dir: File): Boolean {
		if (!isValidSlug(dir.name)) return false
		val so = try {
			validateInstalledLayout(dir, dir.name)
		} catch (t: Throwable) {
			jlog("installed pack ${dir.name} is invalid: ${t.message}")
			return false
		}
		// Re-imports rescan every installed pack: skip the ones already
		// registered (dir name == plugin slug) instead of paying a redundant
		// System.load + dlopen that ends in a WARN on the native side.
		if ((activity as MainActivity).isPluginLoadedNative(dir.name)) return false
		// Android refuses to load a writable .so on newer versions ("Attempt
		// to load writable file … will throw"); make it read-only first.
		runCatching { so.setReadOnly() }
		return try {
			System.load(so.absolutePath) // must precede the native dlopen
			(activity as MainActivity).loadUserPluginNative(dir.absolutePath, so.name)
		} catch (t: Throwable) {
			jlog("load ${dir.name}/${so.name} failed: ${t.message}")
			false
		}
	}

	private fun peekSlug(pack: File): String? {
		if (pack.length() !in 1..MAX_PACK_BYTES)
			throw IllegalArgumentException("pack size is invalid or exceeds ${MAX_PACK_BYTES / 1048576} MB")
		ZipInputStream(pack.inputStream()).use { zin ->
			var e = zin.nextEntry
			var manifestSeen = false
			while (e != null) {
				if (e.name == "plugin.json") {
					if (manifestSeen) throw IllegalArgumentException("duplicate plugin.json")
					manifestSeen = true
					val json = readEntryBytes(zin, MAX_MANIFEST_BYTES).toString(Charsets.UTF_8)
					val slug = JSONObject(json).optString("slug")
					if (!isValidSlug(slug))
						throw IllegalArgumentException("invalid plugin slug")
					return slug
				}
				e = zin.nextEntry
			}
		}
		return null
	}

	private fun unzip(pack: File, dest: File, expectedSlug: String) {
		dest.mkdirs()
		val root = dest.canonicalFile
		val seenPaths = HashSet<String>()
		var entries = 0
		var totalBytes = 0L
		ZipInputStream(pack.inputStream()).use { zin ->
			var e = zin.nextEntry
			while (e != null) {
				entries++
				if (entries > MAX_ENTRIES) throw IllegalArgumentException("too many archive entries")
				validateEntryName(e.name)
				val out = File(root, e.name).canonicalFile
				// Zip-slip guard. Invalid entries reject the entire native-code pack.
				if (!out.path.startsWith(root.path + File.separator))
					throw IllegalArgumentException("archive entry escapes destination")
				if (!seenPaths.add(out.path))
					throw IllegalArgumentException("duplicate archive entry")
				if (e.isDirectory) out.mkdirs()
				else {
					out.parentFile?.mkdirs()
					val remaining = MAX_UNPACKED_BYTES - totalBytes
					if (remaining <= 0) throw IllegalArgumentException("archive is too large")
					val limit = minOf(MAX_ENTRY_BYTES, remaining)
					out.outputStream().use { totalBytes += copyLimited(zin, it, limit) }
				}
				e = zin.nextEntry
			}
		}
		validateInstalledLayout(root, expectedSlug)
	}

	private fun validateInstalledLayout(dir: File, expectedSlug: String): File {
		val root = dir.canonicalFile
		val manifest = File(root, "plugin.json")
		if (!manifest.isFile || manifest.length() !in 1..MAX_MANIFEST_BYTES)
			throw IllegalArgumentException("missing or oversized plugin.json")
		val slug = manifest.inputStream().use {
			JSONObject(readEntryBytes(it, MAX_MANIFEST_BYTES).toString(Charsets.UTF_8)).optString("slug")
		}
		if (slug != expectedSlug || !isValidSlug(slug))
			throw IllegalArgumentException("plugin slug does not match install directory")
		val libraries = root.walkTopDown().filter { it.isFile && it.name.endsWith(".so") }.toList()
		if (libraries.size != 1)
			throw IllegalArgumentException("pack must contain exactly one native library")
		val so = libraries.single().canonicalFile
		if (so.parentFile != root || !VALID_SONAME.matches(so.name))
			throw IllegalArgumentException("native library must be a root libplugin_*.so file")
		validateElfForDevice(so)
		return so
	}

	private fun validateElfForDevice(so: File) {
		val header = ByteArray(20)
		RandomAccessFile(so, "r").use { file ->
			if (file.length() < header.size) throw IllegalArgumentException("native library is truncated")
			file.readFully(header)
		}
		if (header[0] != 0x7f.toByte() || header[1] != 'E'.code.toByte() ||
			header[2] != 'L'.code.toByte() || header[3] != 'F'.code.toByte() ||
			header[4] != 2.toByte() || header[5] != 1.toByte())
			throw IllegalArgumentException("native library is not a 64-bit little-endian ELF")
		val machine = (header[18].toInt() and 0xff) or ((header[19].toInt() and 0xff) shl 8)
		val supportedMachines = Build.SUPPORTED_64_BIT_ABIS.mapNotNull {
			when (it) {
				"arm64-v8a" -> 183 // EM_AARCH64
				"x86_64" -> 62 // EM_X86_64
				else -> null
			}
		}.toSet()
		if (machine !in supportedMachines)
			throw IllegalArgumentException("native library ABI is not supported by this device")
	}

	private fun destinationForSlug(activity: Activity, slug: String): File {
		if (!isValidSlug(slug)) throw IllegalArgumentException("invalid plugin slug")
		val root = installedDir(activity).canonicalFile
		val dest = File(root, slug).canonicalFile
		if (dest.parentFile != root) throw IllegalArgumentException("plugin path escapes install directory")
		return dest
	}

	private fun isValidSlug(slug: String): Boolean =
		VALID_SLUG.matches(slug) && slug != "." && slug != ".."

	private fun validateEntryName(name: String) {
		if (name.isBlank() || name.length > MAX_ENTRY_NAME_CHARS || name.indexOf('\u0000') >= 0 ||
			name.startsWith('/') || name.startsWith('\\') || name.contains('\\'))
			throw IllegalArgumentException("invalid archive entry name")
		val path = name.trimEnd('/')
		if (path.isEmpty() || path.split('/').any { it.isEmpty() || it == "." || it == ".." })
			throw IllegalArgumentException("invalid archive entry path")
	}

	private fun readEntryBytes(input: java.io.InputStream, limit: Long): ByteArray {
		val output = ByteArrayOutputStream()
		copyLimited(input, output, limit)
		return output.toByteArray()
	}

	private fun copyLimited(input: java.io.InputStream, output: OutputStream, limit: Long): Long {
		val buffer = ByteArray(64 * 1024)
		var total = 0L
		while (true) {
			val count = input.read(buffer)
			if (count < 0) break
			total += count
			if (total > limit) throw IllegalArgumentException("archive entry exceeds size limit")
			output.write(buffer, 0, count)
		}
		return total
	}

	private fun writeReadme(activity: Activity) {
		val readme = File(modulesDir(activity), "README.txt")
		if (readme.exists()) return
		readme.writeText(
			"RackDroid — module packs\n\n" +
			"Drop .rdmod pack files here to add extra modules, then restart the app.\n" +
			"A .rdmod is a zip built for this RackDroid version containing plugin.json, " +
			"res/ and its libplugin_*.so.\n\n" +
			"A zip holding several .rdmod files -- all_rdmods.zip from the releases " +
			"page, for one -- can be dropped here as it is: the app unpacks it and " +
			"installs each pack, then removes the zip.\n")
	}

	private fun jlog(msg: String) = android.util.Log.i("rackdroid.modules", msg)
}
