import java.util.Properties

val supportedAndroidAbis = setOf("arm64-v8a", "x86_64")
val targetAndroidAbis = providers.gradleProperty("targetAbis").orNull
	?.split(',')
	?.map(String::trim)
	?.filter(String::isNotEmpty)
	?.distinct()
	?: listOf("arm64-v8a")
require(targetAndroidAbis.isNotEmpty() && targetAndroidAbis.all { it in supportedAndroidAbis }) {
	"targetAbis must be a comma-separated subset of ${supportedAndroidAbis.joinToString()}"
}

plugins {
	id("com.android.application")
	id("org.jetbrains.kotlin.android")
}

// Rack's system assets (res/, Core.json, template.vcv) are packed into a
// single zip because AAssetDir cannot enumerate subdirectories at runtime.
// native/port/asset_extract.cpp unpacks it on first launch.
// A handful of Rack's translated messages name the application, and they spell
// it out rather than going through APP_NAME -- so the rebrand in
// main_android.cpp, which does reach every other mention, cannot touch them.
// The one a RackDroid user actually meets is the restart prompt after changing
// language, which spoke of "Rack" in an app called RackDroid.
//
// Upstream stays untouched: the JSONs are rewritten into build/ on the way into
// the asset zip, the same shape of trick as the string(REPLACE) rules in
// native/CMakeLists.txt. Keyed by entry name, not by searching for the word --
// "Requires Rack 2.6+" really does mean upstream Rack, and TipWindow's mentions
// of the VCV Library and the Rack SDK are other people's product names.
val rebrandTranslations = tasks.register("rebrandTranslations") {
	val src = rootProject.file("third_party/Rack/translations")
	val out = layout.buildDirectory.dir("rebranded-translations")
	inputs.dir(src)
	outputs.dir(out)
	doLast {
		val keys = listOf(
			"MenuBar.help.language.restart",
			"MenuBar.help.language.restartDaw",
			"MenuBar.library.restart")
		val dir = out.get().asFile
		dir.mkdirs()
		src.listFiles { f -> f.name.endsWith(".json") }?.forEach { f ->
			var text = f.readText(Charsets.UTF_8)
			for (key in keys) {
				// Rewrite only this entry's value, and only whole words, so
				// "RackDroid" written twice is impossible and neighbouring
				// entries are never touched.
				val entry = Regex("(\"" + Regex.escape(key) + "\"\\s*:\\s*\")((?:[^\"\\\\]|\\\\.)*)(\")")
				text = entry.replace(text) { m ->
					m.groupValues[1] +
						m.groupValues[2].replace(Regex("\\bRack\\b"), "RackDroid") +
						m.groupValues[3]
				}
			}
			File(dir, f.name).writeText(text, Charsets.UTF_8)
		}
	}
}

val packSystemAssets = tasks.register<Zip>("packSystemAssets") {
	dependsOn(rebrandTranslations)
	// Later entries win: original RackDroid graphics (graphics/, GPLv3)
	// override VCV's non-commercial ComponentLibrary + Core + Fundamental
	// panel SVGs so the app is commercially distributable. Upstream res is
	// taken for everything EXCEPT those (fonts, etc.).
	duplicatesStrategy = DuplicatesStrategy.INCLUDE
	from(rootProject.file("third_party/Rack")) {
		include("res/**")
		exclude("res/ComponentLibrary/**")
		exclude("res/Core/**")
		exclude("res/icon.png") // VCV logo (trademark) — not shipped
		include("Core.json")
		include("template.vcv")
		include("LICENSE-GPLv3.txt")
	}
	from(rebrandTranslations.map { it.outputs.files.singleFile }) {
		into("translations")
	}
	from(rootProject.file("graphics/system-res")) {
		into("res")
	}
	// Touch-first tutorial patch (overrides Rack's desktop template, whose
	// notes talk about right-click/Ctrl/QWERTY) + self-playing demo patches
	// copied to the user patches dir on first run (asset_extract.cpp).
	from(rootProject.file("graphics/template-android")) {
	}
	from(rootProject.file("graphics/NOTICE-graphics.md")) {
	}
	// Manifests/resources of the bundled plugins. Fundamental panels come
	// from graphics/ (originals); its knobs/etc. use the system
	// ComponentLibrary (also original now).
	from(rootProject.file("third_party/Fundamental")) {
		include("plugin.json")
		include("presets/**")
		include("LICENSE*")
		into("plugins/Fundamental")
	}
	from(rootProject.file("graphics/fundamental-res")) {
		into("plugins/Fundamental/res")
	}
	// RackDroid Drums: first-party pack, code and panels both original to
	// this repo -- lives directly under drums/, not third_party/.
	from(rootProject.file("drums")) {
		include("plugin.json")
		include("res/**")
		into("plugins/RackDroidDrums")
	}
	// Lean base: only Fundamental (above) + RackDroid Drums are bundled.
	// Every other pack ships as an on-demand .rdmod (scripts/make_rdmods.sh,
	// MODULES.md); its res/ travels inside the pack, not in system.zip.

	// Themed rack graphics (background/rail + module panels). Each theme is a
	// full recolor of the bundled base set, laid out under themes/<name>/ to
	// mirror the canonical res/ + plugins/*/res/ layout so asset_extract.cpp
	// can copy a chosen theme's files over the canonical paths verbatim at
	// startup. "amber" is the default look (the canonical files above) and is
	// re-packed here as themes/amber/ so switching BACK to amber has a pristine
	// source to restore from. Non-amber sets come from graphics/themes/<name>/,
	// generated by graphics/gen_themes.py. Cost is tiny: SVGs are text.
	from(rootProject.file("graphics/system-res")) { into("themes/amber/res") }
	from(rootProject.file("graphics/fundamental-res")) { into("themes/amber/plugins/Fundamental/res") }
	from(rootProject.file("drums/res")) { into("themes/amber/plugins/RackDroidDrums/res") }
	for (t in listOf("blue", "emerald", "violet")) {
		from(rootProject.file("graphics/themes/$t/system-res")) { into("themes/$t/res") }
		from(rootProject.file("graphics/themes/$t/fundamental-res")) { into("themes/$t/plugins/Fundamental/res") }
		from(rootProject.file("graphics/themes/$t/drums-res")) { into("themes/$t/plugins/RackDroidDrums/res") }
	}

	archiveFileName.set("system.zip")
	destinationDirectory.set(layout.buildDirectory.dir("generated/assets"))
}

// Module browser tile art (ModuleThumbnails.kt): one PNG per model,
// generated once by `rack_ui_smoke --export-thumbnails` (see
// native/host/main_ui_host.cpp) and committed under graphics/browser-thumbs/,
// same convention as graphics/regen_graphics.py's SVG output. Packed
// separately from system.zip so it can be revisioned/extracted independently
// (native/port/asset_extract.cpp) — it's pure UI art, unrelated to the engine
// assets and an order of magnitude larger.
//
// Only the plugins actually bundled in the APK (Core, Fundamental,
// RackDroidDrums) ship their thumbnails here. Every other plugin's
// thumbnails travel inside its own .rdmod instead (scripts/make_rdmods.sh
// packs graphics/browser-thumbs/<slug>/ as thumbs/ in the pack), since the
// tile art is useless until that plugin is actually installed —
// bundling all of them in the base APK was pure dead weight for anyone who
// never side-loads that pack. ThumbnailCache.get() (ModuleThumbnails.kt)
// falls back to a side-loaded pack's own thumbs/ dir when a key isn't found
// here.
val packThumbnailAssets = tasks.register<Zip>("packThumbnailAssets") {
	from(rootProject.file("graphics/browser-thumbs")) {
		include("Core/**", "Fundamental/**", "RackDroidDrums/**")
	}
	archiveFileName.set("thumbnails.zip")
	destinationDirectory.set(layout.buildDirectory.dir("generated/assets"))
}

android {
	namespace = "org.rackdroid"
	compileSdk = 35
	// r27+: needed for ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES below. Play
	// rejects targetSdk 35 apps whose native libs aren't 16 KB-aligned.
	ndkVersion = "27.2.12479018"

	defaultConfig {
		applicationId = "org.rackdroid"
		// Android 10 (Q). Bionic's <execinfo.h> (Rack's system::getStackTrace)
		// only exists from API 33; native/compat/execinfo.h shims backtrace()
		// via the unwind.h EH API below that, so the true floor is Q's AMidi
		// native MIDI API (native/port/amidi_driver.cpp), also API 29.
		minSdk = 29
		targetSdk = 35
		// A fourth component marks a fix release over 0.1.2 rather than new
		// work. The updater compares versionName numerically component by
		// component, padding the shorter side, so 0.1.2.1 beats 0.1.2.
		versionCode = 8
		versionName = "0.1.2.5"

		ndk {
			// Default remains the physical-device release. Build emulator /
			// Chromebook variants with -PtargetAbis=x86_64. AAB builds may
			// request both supported 64-bit ABIs as a comma-separated list.
			abiFilters += targetAndroidAbis
		}
		externalNativeBuild {
			cmake {
				arguments += listOf(
					"-DANDROID_STL=c++_shared",
					"-DANDROID_PLATFORM=android-29",
					// 16 KB page-size support (max-page-size=16384 on every
					// .so, including FetchContent deps like libsamplerate)
					"-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON"
				)
				// rackdroid pulls in librack_engine.so; the plugin .so are
				// dlopen'd at runtime and must be packaged explicitly. Normal
				// builds compile only the base set; release-pack CI uses
				// -PallPlugins so scripts/make_rdmods.sh has every selected-ABI library.
				val baseTargets = listOf("rackdroid", "plugin_fundamental", "plugin_drums")
				val optionalTargets = listOf(
					"plugin_bogaudio", "plugin_valley", "plugin_hetrickcv", "plugin_jw",
					"plugin_ml", "plugin_rj", "plugin_computerscare", "plugin_littleutils",
					"plugin_autinn", "plugin_venom", "plugin_sonus", "plugin_nlc",
					"plugin_aria", "plugin_packone", "plugin_frozenwasteland",
					"plugin_audible", "plugin_impromptu", "plugin_bidoo", "plugin_grande",
					"plugin_countmodula", "plugin_befaco"
				)
				targets += if (project.hasProperty("allPlugins"))
					baseTargets + optionalTargets
				else
					baseTargets
			}
		}
	}

	externalNativeBuild {
		cmake {
			path = file("../native/CMakeLists.txt")
			version = "3.22.1"
		}
	}

	// Two distributions, because they are allowed to do different things.
	//
	//   sideload — what people download from GitHub. It may check for a new
	//              release and install it, so it declares INTERNET and
	//              REQUEST_INSTALL_PACKAGES (see src/sideload/).
	//   play     — what Google Play would receive. Play's Device and Network
	//              Abuse policy forbids an app updating itself outside the
	//              store, so this flavor carries neither the permissions nor
	//              the code: src/play/ stubs the updater out entirely.
	//
	// Both share every source file in src/main/, and the CMake arguments are
	// identical, so the native build is configured once and reused.
	flavorDimensions += "distribution"
	productFlavors {
		create("sideload") { dimension = "distribution" }
		create("play") { dimension = "distribution" }
	}

	sourceSets {
		getByName("main") {
			assets.srcDir(layout.buildDirectory.dir("generated/assets"))
		}
	}

	signingConfigs {
		create("release") {
			// Production signing: if ~/rackdroid-keystore.properties exists
			// (storeFile/storePassword/keyAlias/keyPassword), it is used —
			// the keystore lives OUTSIDE the repo and must never be
			// committed. Otherwise fall back to the public development
			// keystore (sideload update continuity only, NOT authenticity).
			// -PdevKeystore forces the dev key even when the production
			// properties exist: GitHub sideload APKs keep update continuity
			// with installs made before the production key existed, while
			// Play AABs (built without the flag) get the private key.
			val useDev = project.hasProperty("devKeystore")
			val propsFile = File(System.getProperty("user.home"), "rackdroid-keystore.properties")
			if (!useDev && propsFile.exists()) {
				val props = Properties()
				propsFile.inputStream().use { stream -> props.load(stream) }
				storeFile = file(props.getProperty("storeFile"))
				storePassword = props.getProperty("storePassword")
				keyAlias = props.getProperty("keyAlias")
				keyPassword = props.getProperty("keyPassword")
			} else {
				storeFile = file("../keystore/rackdroid.keystore")
				storePassword = "rackdroid"
				keyAlias = "rackdroid"
				keyPassword = "rackdroid"
			}
		}
	}

	buildTypes {
		release {
			isMinifyEnabled = false
			signingConfig = signingConfigs.getByName("release")
		}
	}

	// Lean base APK: -PallPlugins compiles every optional plugin for
	// scripts/make_rdmods.sh, but only Fundamental + Drums ride along in the
	// APK. Every other plugin .so is dropped at packaging time and shipped
	// instead as an on-demand .rdmod (MODULES.md). Keep this list in sync with
	// optionalTargets and native/CMakeLists.txt.
	packaging {
		jniLibs {
			excludes += listOf(
				"**/libplugin_aria.so",
				"**/libplugin_audible.so",
				"**/libplugin_autinn.so",
				"**/libplugin_befaco.so",
				"**/libplugin_bidoo.so",
				"**/libplugin_bogaudio.so",
				"**/libplugin_computerscare.so",
				"**/libplugin_countmodula.so",
				"**/libplugin_frozenwasteland.so",
				"**/libplugin_grande.so",
				"**/libplugin_hetrickcv.so",
				"**/libplugin_impromptu.so",
				"**/libplugin_jw.so",
				"**/libplugin_littleutils.so",
				"**/libplugin_ml.so",
				"**/libplugin_nlc.so",
				"**/libplugin_packone.so",
				"**/libplugin_rj.so",
				"**/libplugin_sonus.so",
				"**/libplugin_valley.so",
				"**/libplugin_venom.so"
			)
		}
	}

	compileOptions {
		sourceCompatibility = JavaVersion.VERSION_17
		targetCompatibility = JavaVersion.VERSION_17
	}
	kotlinOptions {
		jvmTarget = "17"
	}
}

tasks.named("preBuild") {
	dependsOn(packSystemAssets)
	dependsOn(packThumbnailAssets)
}

dependencies {
	// FileProvider for sharing patches
	implementation("androidx.core:core-ktx:1.13.1")
	// Module browser grid
	implementation("androidx.recyclerview:recyclerview:1.3.2")
}
