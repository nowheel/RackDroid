package org.rackdroid

import android.app.Activity
import android.app.AlertDialog
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import androidx.core.content.IntentCompat
import android.content.pm.PackageInfo
import android.content.pm.PackageInstaller
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.util.Log
import android.widget.Toast
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/** Self-update for the GitHub build.
 *
 * The app is otherwise offline — no telemetry, no accounts, nothing phones home
 * — so this is the one place that opens a socket, and it stays shut until the
 * user says yes. Nothing is checked before the opt-in dialog is answered, and
 * answering "no" is remembered.
 *
 * What is downloaded is treated as untrusted until proven otherwise: it must
 * parse as an APK, carry this application id, a HIGHER version code, and above
 * all be signed by the same key as the running app. Android enforces that last
 * one too, but failing early gives an honest message instead of a bewildering
 * refusal from the installer. Only then does the system installer get it, and
 * that still asks the user itself.
 *
 * The Play flavor stubs this whole object out (src/play/) — see the note there
 * for why an updater cannot ship on Play at all. */
object AppUpdates {
	const val SUPPORTED = true

	private const val TAG = "rackdroid.updates"
	// The release list, not /latest: this repository publishes module packs as
	// releases too, so "latest" is regularly not the application.
	private const val RELEASES_API =
		"https://api.github.com/repos/nowheel/RackDroid/releases?per_page=15"
	private const val PREFS = "updates"
	private const val KEY_ASKED = "opt_in_asked"
	private const val KEY_ENABLED = "enabled"
	private const val KEY_LAST_CHECK = "last_check"
	/** One check a day is plenty for a hobby release cadence, and it keeps the
	 * app from talking to GitHub every time it is opened. */
	private const val CHECK_INTERVAL_MS = 24L * 60 * 60 * 1000
	private const val MAX_APK_BYTES = 250L * 1024 * 1024
	private const val CONNECT_TIMEOUT_MS = 15_000
	private const val READ_TIMEOUT_MS = 30_000
	/** Architecture words the release assets put in their filenames. Used to
	 * recognise an APK built for a machine this one is not, so it is never
	 * downloaded: the install would fail after the whole transfer. */
	private val ABI_TOKENS = listOf("arm64-v8a", "x86_64", "armeabi-v7a", "x86")

	private val ui = Handler(Looper.getMainLooper())

	private fun prefs(context: Context) =
		context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

	/** The running version, straight from the package manager -- the same
	 * source the credits dialog uses. BuildConfig would need an extra build
	 * feature switched on for this one class. */
	private fun versionName(context: Context): String =
		runCatching {
			context.packageManager.getPackageInfo(context.packageName, 0).versionName
		}.getOrNull() ?: "0"

	/** Once per launch. Asks for consent the first time, then — only if it was
	 * granted, and at most once a day — looks for a new release in the
	 * background. Silent when there is nothing to report. */
	fun onStart(activity: Activity) {
		val p = prefs(activity)
		if (!p.getBoolean(KEY_ASKED, false)) {
			askOptIn(activity)
			return
		}
		if (!p.getBoolean(KEY_ENABLED, false))
			return
		val now = System.currentTimeMillis()
		val last = p.getLong(KEY_LAST_CHECK, 0L)
		// A clock that jumped backwards must not park the next check in the
		// far future, so treat any negative interval as "due".
		if (now - last in 0 until CHECK_INTERVAL_MS)
			return
		p.edit().putLong(KEY_LAST_CHECK, now).apply()
		check(activity, explicit = false)
	}

	/** From Help ▸ Check for updates. Always looks, and says so even when the
	 * app is already current — a silent no-op reads as a broken button. */
	/** The installer session reports back through a PendingIntent aimed at
	 * MainActivity, so every arriving intent passes through here.
	 *
	 * STATUS_PENDING_USER_ACTION is the one that matters: Android will not
	 * install anything until the user has seen its own confirmation screen, and
	 * that screen only appears if the app launches the intent handed to it. The
	 * first version of this never did, so the session sat pending for ever --
	 * the download worked, the checks passed, and nothing was installed, with
	 * nothing on screen to say why. */
	fun onNewIntent(activity: Activity, intent: Intent?) {
		val status = intent?.getIntExtra(PackageInstaller.EXTRA_STATUS, Int.MIN_VALUE)
			?: return
		if (status == Int.MIN_VALUE)
			return // not from the installer
		when (status) {
			PackageInstaller.STATUS_PENDING_USER_ACTION -> {
				val confirm = IntentCompat.getParcelableExtra(
					intent, Intent.EXTRA_INTENT, Intent::class.java)
				if (confirm != null)
					runCatching { activity.startActivity(confirm) }
				else
					Log.w(TAG, "installer asked for user action without an intent")
			}
			PackageInstaller.STATUS_SUCCESS ->
				Log.i(TAG, "update installed")
			PackageInstaller.STATUS_FAILURE_ABORTED ->
				Log.i(TAG, "update cancelled by the user")
			else -> {
				val message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE)
				Log.w(TAG, "install failed (status ${'$'}status): ${'$'}message")
				Toast.makeText(activity, R.string.updates_install_failed, Toast.LENGTH_LONG).show()
			}
		}
	}

	fun checkNow(activity: Activity) {
		prefs(activity).edit()
			.putBoolean(KEY_ASKED, true)
			.putBoolean(KEY_ENABLED, true)
			.putLong(KEY_LAST_CHECK, System.currentTimeMillis())
			.apply()
		Toast.makeText(activity, R.string.updates_checking, Toast.LENGTH_SHORT).show()
		check(activity, explicit = true)
	}

	private fun askOptIn(activity: Activity) {
		AlertDialog.Builder(activity)
			.setTitle(R.string.updates_optin_title)
			.setMessage(R.string.updates_optin_message)
			.setNegativeButton(R.string.updates_optin_no) { _, _ ->
				prefs(activity).edit()
					.putBoolean(KEY_ASKED, true)
					.putBoolean(KEY_ENABLED, false)
					.apply()
			}
			.setPositiveButton(R.string.updates_optin_yes) { _, _ ->
				prefs(activity).edit()
					.putBoolean(KEY_ASKED, true)
					.putBoolean(KEY_ENABLED, true)
					.apply()
				checkNow(activity)
			}
			.setCancelable(false)
			.show()
	}

	private data class Release(val versionName: String, val notes: String, val apkUrl: String)

	/** Release notes are written in Markdown for GitHub; an AlertDialog shows
	 * plain text, so the asterisks and backticks arrived on screen as
	 * literals. This strips the handful of marks that actually appear in these
	 * notes rather than pretending to be a Markdown renderer. */
	private fun plainNotes(raw: String): String = raw
		.replace(Regex("\\*\\*(.+?)\\*\\*"), "$1")
		.replace(Regex("`([^`]+)`"), "$1")
		.replace(Regex("(?m)^#{1,6}\\s*"), "")
		.replace(Regex("(?m)^[-*]\\s+"), "• ")
		.replace(Regex("\\[([^]]+)]\\([^)]+\\)"), "$1")
		.trim()

	private fun check(activity: Activity, explicit: Boolean) {
		val current = versionName(activity)
		Thread {
			val release = runCatching { fetchLatest(current) }.getOrElse { e ->
				Log.w(TAG, "update check failed: ${e.message}")
				if (explicit)
					ui.post {
						if (!activity.isFinishing)
							Toast.makeText(activity, R.string.updates_failed, Toast.LENGTH_LONG).show()
					}
				null
			} ?: return@Thread
			ui.post {
				if (activity.isFinishing) return@post
				if (release.apkUrl.isEmpty()) {
					if (explicit)
						Toast.makeText(activity, R.string.updates_none, Toast.LENGTH_LONG).show()
					return@post
				}
				offer(activity, release)
			}
		}.start()
	}

	/** The newest APK across the recent releases, or a Release with an empty URL
	 * when nothing beats what is installed. Runs off the main thread.
	 *
	 * Deliberately NOT keyed on /releases/latest and its tag: this repository
	 * also publishes module packs as releases (tag `rdmods-16k`, for one), so
	 * "latest" is often not the app at all and the tag is not a version number.
	 * The version comes from the APK asset's own filename instead — that is what
	 * the build actually produces — and every recent release is considered. */
	private fun fetchLatest(currentVersion: String): Release? {
		val body = httpGet(RELEASES_API, currentVersion)
		val releases = JSONArray(body)
		var best: Release? = null
		var bestRank = 0
		for (r in 0 until releases.length()) {
			val rel = releases.optJSONObject(r) ?: continue
			// Drafts are invisible without a token anyway; pre-releases are
			// opt-in territory and should not be pushed at everyone.
			if (rel.optBoolean("draft") || rel.optBoolean("prerelease"))
				continue
			val notes = rel.optString("body").trim()
			val assets = rel.optJSONArray("assets") ?: continue
			for (i in 0 until assets.length()) {
				val a = assets.optJSONObject(i) ?: continue
				val name = a.optString("name")
				val url = a.optString("browser_download_url")
				if (!name.endsWith(".apk", ignoreCase = true) || url.isEmpty())
					continue
				// A release carries one APK per distribution as well as per
				// architecture, and this must never take the Play one: same
				// package and same key, so it installs without a word of
				// warning, and from there the updater is a stub and the app
				// holds no INTERNET permission. The user would simply stop
				// receiving updates, with nothing on screen to say why.
				if (name.contains("play", ignoreCase = true))
					continue
				val version = versionInName(name)
					?: rel.optString("tag_name").trim().removePrefix("v")
				if (!isNewer(version, currentVersion))
					continue
				val rank = abiRank(name)
				if (rank < 0)
					continue
				// A release may carry one APK per architecture, so among the
				// assets of one version the best-fitting build wins. Ranking
				// them, rather than letting the last ABI match overwrite the
				// others, is deliberate: the old rule picked the right file
				// only because GitHub happens to return assets sorted by name.
				val better = best == null ||
					isNewer(version, best!!.versionName) ||
					(version == best!!.versionName && rank > bestRank)
				if (better) {
					best = Release(version, notes, url)
					bestRank = rank
				}
			}
		}
		return best ?: Release(currentVersion, "", "")
	}

	/** How well an asset's filename fits this machine: higher is better, and
	 * below zero means it cannot run here at all. SUPPORTED_ABIS is ordered by
	 * preference, so a native build outranks one that would only work through a
	 * translation layer -- which is the case on the x86_64 tablets that also
	 * carry an arm64 bridge and would otherwise take the slower APK. A build
	 * that names no architecture is the single APK the early releases had, and
	 * stays usable; one that names somebody else's is refused outright. */
	private fun abiRank(name: String): Int {
		val abis = android.os.Build.SUPPORTED_ABIS
		val i = abis.indexOfFirst { name.contains(it, ignoreCase = true) }
		if (i >= 0)
			return abis.size - i + 1
		if (name.contains("universal", ignoreCase = true))
			return 1
		return if (ABI_TOKENS.any { name.contains(it, ignoreCase = true) }) -1 else 0
	}

	/** First dotted version inside a filename, e.g. rackdroid-0.1.1-release.apk. */
	private fun versionInName(name: String): String? =
		Regex("""\d+(?:\.\d+)+""").find(name)?.value

	/** Dotted numeric comparison, shorter side padded: 0.2 is newer than 0.1.9,
	 * and a tag that is not numeric at all is never treated as newer. */
	private fun isNewer(candidate: String, current: String): Boolean {
		fun parts(v: String) = v.split('.', '-', '+').mapNotNull { it.toIntOrNull() }
		val a = parts(candidate)
		val b = parts(current)
		if (a.isEmpty())
			return false
		for (i in 0 until maxOf(a.size, b.size)) {
			val x = a.getOrElse(i) { 0 }
			val y = b.getOrElse(i) { 0 }
			if (x != y) return x > y
		}
		return false
	}

	private fun httpGet(url: String, version: String): String {
		val conn = (URL(url).openConnection() as HttpURLConnection).apply {
			connectTimeout = CONNECT_TIMEOUT_MS
			readTimeout = READ_TIMEOUT_MS
			requestMethod = "GET"
			setRequestProperty("Accept", "application/vnd.github+json")
			setRequestProperty("User-Agent", "RackDroid/$version")
		}
		try {
			if (conn.responseCode != 200)
				throw IllegalStateException("HTTP ${conn.responseCode}")
			return conn.inputStream.bufferedReader().use { it.readText() }
		} finally {
			conn.disconnect()
		}
	}

	/** What a release changes, for the dialog that offers it: the part of its
	 * notes between "<!-- changes xx -->" and "<!-- end changes -->" for the
	 * app's language, or the English one. The comments do not show on GitHub;
	 * PUBLISHING.md says how the notes are laid out. Notes without them (and
	 * every release up to 0.1.2.1) fall back to their opening lines. */
	private fun changesIn(raw: String, language: String): String? {
		fun block(code: String) = Regex("<!--\\s*changes\\s+$code\\s*-->(.*?)<!--\\s*end changes\\s*-->",
			RegexOption.DOT_MATCHES_ALL).find(raw)?.groupValues?.get(1)?.trim()
		return (block(language) ?: block("en"))?.takeIf { it.isNotEmpty() }
	}

	private fun offer(activity: Activity, release: Release) {
		val language = activity.resources.configuration.locales[0].language
		val notes = (changesIn(release.notes, language)?.let { plainNotes(it) }
			?: plainNotes(release.notes).take(700))
			.ifEmpty { activity.getString(R.string.updates_no_notes) }
		AlertDialog.Builder(activity)
			.setTitle(activity.getString(R.string.updates_available_title, release.versionName))
			.setMessage(notes)
			.setNegativeButton(android.R.string.cancel, null)
			.setPositiveButton(R.string.updates_install) { _, _ -> download(activity, release) }
			.show()
	}

	private fun download(activity: Activity, release: Release) {
		val version = versionName(activity)
		val progress = AlertDialog.Builder(activity)
			.setTitle(R.string.updates_downloading)
			.setMessage(activity.getString(R.string.updates_downloading_message, release.versionName))
			.setCancelable(false)
			.show()
		Thread {
			val dir = File(activity.filesDir, "updates").apply { mkdirs() }
			// One file, always replaced: a half-finished download from a
			// previous attempt must never be handed to the installer.
			val apk = File(dir, "update.apk")
			val outcome = runCatching {
				apk.delete()
				fetchApk(release.apkUrl, apk, version)
				verify(activity, apk)
			}
			ui.post {
				runCatching { progress.dismiss() }
				if (activity.isFinishing) return@post
				val problem = outcome.exceptionOrNull()
				if (problem != null) {
					apk.delete()
					Log.w(TAG, "update rejected: ${problem.message}")
					AlertDialog.Builder(activity)
						.setTitle(R.string.updates_rejected_title)
						.setMessage(activity.getString(R.string.updates_rejected_message,
							problem.message ?: ""))
						.setPositiveButton(android.R.string.ok, null)
						.show()
					return@post
				}
				install(activity, apk)
			}
		}.start()
	}

	private fun fetchApk(url: String, destination: File, version: String) {
		val conn = (URL(url).openConnection() as HttpURLConnection).apply {
			connectTimeout = CONNECT_TIMEOUT_MS
			readTimeout = READ_TIMEOUT_MS
			instanceFollowRedirects = true
			setRequestProperty("User-Agent", "RackDroid/$version")
		}
		try {
			if (conn.responseCode != 200)
				throw IllegalStateException("HTTP ${conn.responseCode}")
			conn.inputStream.use { input ->
				destination.outputStream().use { output ->
					val buffer = ByteArray(64 * 1024)
					var total = 0L
					while (true) {
						val read = input.read(buffer)
						if (read < 0) break
						total += read
						if (total > MAX_APK_BYTES)
							throw IllegalStateException("download too large")
						output.write(buffer, 0, read)
					}
				}
			}
		} finally {
			conn.disconnect()
		}
	}

	/** Everything that must hold before the system installer is even opened.
	 * Throws with a message meant for the user. */
	private fun verify(activity: Activity, apk: File) {
		val pm = activity.packageManager
		val flags = PackageManager.GET_SIGNING_CERTIFICATES
		val candidate = pm.getPackageArchiveInfo(apk.absolutePath, flags)
			?: throw IllegalStateException(activity.getString(R.string.updates_bad_apk))
		if (candidate.packageName != activity.packageName)
			throw IllegalStateException(activity.getString(R.string.updates_wrong_package))
		val installed = pm.getPackageInfo(activity.packageName, flags)
		val currentCode = installed.longVersionCode
		val newCode = candidate.longVersionCode
		if (newCode <= currentCode)
			throw IllegalStateException(activity.getString(R.string.updates_not_newer))
		val mine = certificates(installed)
		val theirs = certificates(candidate)
		if (mine.isEmpty() || mine != theirs)
			throw IllegalStateException(activity.getString(R.string.updates_signature))
	}

	private fun certificates(info: PackageInfo): Set<String> {
		val signing = info.signingInfo ?: return emptySet()
		val signatures = if (signing.hasMultipleSigners())
			signing.apkContentsSigners else signing.signingCertificateHistory
		val digest = MessageDigest.getInstance("SHA-256")
		return (signatures ?: emptyArray()).map { s ->
			digest.digest(s.toByteArray()).joinToString("") { "%02x".format(it) }
		}.toSet()
	}

	private fun install(activity: Activity, apk: File) {
		// From API 26 the user grants "install unknown apps" per source; without
		// it the session commit fails silently, so send them to the right screen.
		if (!activity.packageManager.canRequestPackageInstalls()) {
			AlertDialog.Builder(activity)
				.setTitle(R.string.updates_permission_title)
				.setMessage(R.string.updates_permission_message)
				.setNegativeButton(android.R.string.cancel, null)
				.setPositiveButton(R.string.updates_permission_open) { _, _ ->
					runCatching {
						activity.startActivity(Intent(
							Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
							Uri.parse("package:${activity.packageName}")))
					}
				}
				.show()
			return
		}
		runCatching {
			val installer = activity.packageManager.packageInstaller
			val params = PackageInstaller.SessionParams(
				PackageInstaller.SessionParams.MODE_FULL_INSTALL)
			params.setAppPackageName(activity.packageName)
			val sessionId = installer.createSession(params)
			installer.openSession(sessionId).use { session ->
				session.openWrite("rackdroid", 0, apk.length()).use { out ->
					apk.inputStream().use { it.copyTo(out) }
					session.fsync(out)
				}
				val callback = PendingIntent.getActivity(
					activity, sessionId,
					Intent(activity, MainActivity::class.java),
					PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
				session.commit(callback.intentSender)
			}
		}.onFailure { e ->
			Log.w(TAG, "install failed: ${e.message}")
			Toast.makeText(activity, R.string.updates_install_failed, Toast.LENGTH_LONG).show()
		}
	}
}
