/* Bundled plugin loader.
 *
 * Plugins ship as real shared libraries in the APK lib dir (Android forbids
 * dlopen from app storage, but the APK lib dir is fine) and are dlopen'd
 * with their own local symbol scope — same isolation as desktop Rack's
 * plugin.so files, which is what lets different plugins reuse global names
 * like pluginInstance or VCOWidget. Manifest handling mirrors the manifest
 * side of Rack's loadPlugin() (src/plugin.cpp) using public API only, but
 * leniently: manifest modules without a registered model are skipped.
 *
 * The .so is resolved by bare name through the caller's library search path
 * (app lib dir on Android, $ORIGIN runpath for the host smoke test).
 */
#include <dlfcn.h>
#include <cstdlib>

#include <jansson.h>

#ifdef ANDROID
#include <jni.h>
#include <android/log.h>
#endif

#include <plugin.hpp>
#include <plugin/Plugin.hpp>
#include <asset.hpp>
#include <system.hpp>
#include <common.hpp>
#include <logger.hpp>

#include "static_plugins.hpp"


namespace rackdroid {


using namespace rack;


typedef void (*InitCallback)(plugin::Plugin*);


/** Finish loading a plugin whose .so is already dlopen'd (handle) and whose
 * manifest/res live at `path`. Shared by the bundled loader and the
 * user-plugin loader. Returns true on success. `label` is only for logs. */
static bool finishLoadPlugin(void* handle, const std::string& path, const std::string& label) {
	std::string manifestPath = system::join(path, "plugin.json");
	InitCallback initCallback = (InitCallback) dlsym(handle, "init");
	if (!initCallback) {
		WARN("Plugin %s has no init(): %s", label.c_str(), dlerror());
		return false;
	}

	plugin::Plugin* plugin = new plugin::Plugin;
	json_t* rootJ = NULL;
	try {
		plugin->path = path;
		plugin->handle = handle;

		FILE* file = std::fopen(manifestPath.c_str(), "r");
		if (!file)
			throw Exception("Bundled plugin manifest %s not found", manifestPath.c_str());
		json_error_t error;
		rootJ = json_loadf(file, 0, &error);
		std::fclose(file);
		if (!rootJ)
			throw Exception("Invalid manifest %s: %s %d:%d", manifestPath.c_str(), error.text, error.line, error.column);

		// Community v2 ports sometimes keep the v1 version string (e.g.
		// Cardinal's AriaModules fork says 1.8.1b). The bundled set is
		// compiled against this Rack tree, so the ABI is known-good;
		// normalize the major version instead of failing fromJson().
		json_t* versionJ = json_object_get(rootJ, "version");
		if (versionJ) {
			std::string v = json_string_value(versionJ);
			if (!v.empty() && v[0] != '2')
				json_object_set_new(rootJ, "version", json_string(("2." + v).c_str()));
		}

		plugin->fromJson(rootJ);

		if (plugin::getPlugin(plugin->slug))
			throw Exception("Plugin %s is already loaded", plugin->slug.c_str());

		initCallback(plugin);
		INFO("Plugin %s registered %d models", label.c_str(), (int) plugin->models.size());

		// Lenient version of Plugin::modulesFromJson(): a manifest entry
		// without a registered model (e.g. behind an #ifdef upstream) is
		// skipped instead of failing the whole plugin.
		json_t* modulesJ = json_object_get(rootJ, "modules");
		if (modulesJ) {
			size_t i;
			json_t* moduleJ;
			json_array_foreach(modulesJ, i, moduleJ) {
				json_t* slugJ = json_object_get(moduleJ, "slug");
				if (!slugJ)
					continue;
				std::string moduleSlug = json_string_value(slugJ);
				plugin::Model* model = plugin->getModel(moduleSlug);
				if (!model) {
					WARN("Plugin %s: manifest module %s has no registered model, skipping",
						label.c_str(), moduleSlug.c_str());
					continue;
				}
				model->fromJson(moduleJ);
			}
		}

		json_decref(rootJ);
	}
	catch (Exception& e) {
		WARN("Could not load plugin %s: %s", label.c_str(), e.what());
		if (rootJ)
			json_decref(rootJ);
		// Never dlclose/delete: registered Models may already point into
		// the library.
		return false;
	}

	plugin::plugins.push_back(plugin);
	INFO("Loaded plugin %s %s", plugin->slug.c_str(), plugin->version.c_str());
	return true;
}


static bool loadBundledPlugin(const std::string& slug, const std::string& libraryName) {
	void* handle = dlopen(libraryName.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		WARN("Could not dlopen bundled plugin %s: %s", libraryName.c_str(), dlerror());
		return false;
	}
	return finishLoadPlugin(handle, asset::system("plugins/" + slug), slug);
}


#ifdef ANDROID
/** Load a user-installed plugin pack: <pluginDir> holds plugin.json + res/,
 * and its native library `soname` was already brought into the process by
 * Java System.load() (the only way to satisfy the app linker namespace for
 * an app-private .so on API 24+). We just grab the already-loaded handle
 * with RTLD_NOLOAD and run the shared manifest/init path. Returns true on
 * success. Called via JNI after Java extracts+loads a pack. */
static bool loadUserPluginAt(const std::string& pluginDir, const std::string& soname) {
	void* handle = dlopen(soname.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
	if (!handle) {
		WARN("User plugin %s not resolvable (System.load first?): %s", soname.c_str(), dlerror());
		return false;
	}
	return finishLoadPlugin(handle, pluginDir, soname);
}
#endif // ANDROID


void loadStaticPlugins() {
	// Lean base: Core (built into the engine) + Fundamental + RackDroid
	// Drums. Every other pack ships as an on-demand .rdmod, loaded from the
	// user Modules folder (ModuleInstaller / MODULES.md). The tutorials use
	// only these base modules.
	bool baseLoaded = loadBundledPlugin("Fundamental", "libplugin_fundamental.so");
	baseLoaded = loadBundledPlugin("RackDroidDrums", "libplugin_drums.so") && baseLoaded;
#ifndef ANDROID
	if (!baseLoaded)
		throw Exception("Host smoke test could not load every base plugin");
#endif

	// Host smoke coverage hook: RACKDROID_EXTRA_PLUGINS is a colon-separated
	// list of slug=soname pairs (e.g. "Bogaudio=libplugin_bogaudio.so:...").
	// Each loads through the same bundled path — manifest and res/ must sit
	// at <systemDir>/plugins/<slug>/ — so `rack_ui_smoke --all-modules` can
	// exercise every pack, not just the lean base. Unused on Android.
	if (const char* extra = std::getenv("RACKDROID_EXTRA_PLUGINS")) {
		std::string list = extra;
		size_t pos = 0;
		int requested = 0;
		int loaded = 0;
		while (pos < list.size()) {
			size_t colon = list.find(':', pos);
			if (colon == std::string::npos) colon = list.size();
			std::string pair = list.substr(pos, colon - pos);
			size_t eq = pair.find('=');
			if (eq != std::string::npos) {
				requested++;
				if (loadBundledPlugin(pair.substr(0, eq), pair.substr(eq + 1)))
					loaded++;
			}
			pos = colon + 1;
		}
#ifndef ANDROID
		if (loaded != requested)
			throw Exception("Host smoke test loaded %d of %d requested extra plugins",
				loaded, requested);
#endif
	}
}


#ifdef ANDROID
// ---- JNI: user-installed plugin packs (MainActivity) ----

extern "C" JNIEXPORT jboolean JNICALL
Java_org_rackdroid_MainActivity_nativeLoadUserPlugin(
		JNIEnv* env, jobject, jstring dirJ, jstring sonameJ) {
	const char* dir = env->GetStringUTFChars(dirJ, NULL);
	const char* soname = env->GetStringUTFChars(sonameJ, NULL);
	bool ok = false;
	try {
		ok = loadUserPluginAt(dir ? dir : "", soname ? soname : "");
	}
	catch (std::exception& e) {
		// Both sinks: a pack that refuses to load is the single most reported
		// problem here, and logcat needs a cable at the other end of it.
		__android_log_print(ANDROID_LOG_ERROR, "rackdroid", "loadUserPlugin: %s", e.what());
		WARN("loadUserPlugin: %s", e.what());
	}
	if (dir) env->ReleaseStringUTFChars(dirJ, dir);
	if (soname) env->ReleaseStringUTFChars(sonameJ, soname);
	return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_rackdroid_MainActivity_nativeIsPluginLoaded(
		JNIEnv* env, jobject, jstring slugJ) {
	const char* slug = env->GetStringUTFChars(slugJ, NULL);
	bool loaded = slug && plugin::getPlugin(slug);
	if (slug) env->ReleaseStringUTFChars(slugJ, slug);
	return loaded ? JNI_TRUE : JNI_FALSE;
}
#endif // ANDROID


} // namespace rackdroid
