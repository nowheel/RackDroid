/* RackDroid NativeActivity entry point (phase 2: full UI).
 *
 * Mirrors adapters/standalone.cpp's bring-up, adapted to the Android app
 * lifecycle:
 *  - Rack runtime + engine start once, on the first APP_CMD_INIT_WINDOW
 *  - window::Window (EGL/GLES3 implementation in window_android.cpp) is
 *    created when the surface exists; on TERM/INIT the surface is swapped
 *    under it while the GL context and the whole Scene survive
 *  - the Looper loop renders a frame per iteration while the surface is live
 *  - touch events are translated to Rack mouse events (touch_input.cpp)
 */
#include <android_native_app_glue.h>
#include <android/api-level.h>
#include <android/configuration.h>
#include <android/log.h>
#include <sys/system_properties.h>

#include <common.hpp>
#include <system.hpp>
#include <asset.hpp>
#include <logger.hpp>
#include <random.hpp>
#include <string.hpp>
#include <settings.hpp>
#include <audio.hpp>
#include <midi.hpp>
#include <midiloopback.hpp>
#include <keyboard.hpp>
#include <plugin.hpp>
#include <library.hpp>
#include <network.hpp>
#include <context.hpp>
#include <engine/Engine.hpp>
#include <history.hpp>
#include <patch.hpp>
#include <widget/event.hpp>
#include <app/Scene.hpp>
#include <app/Browser.hpp>
#include <ui/common.hpp>
#include <window/Window.hpp>

#include "asset_extract.hpp"
#include "audio_oboe.hpp"
#include "window_android.hpp"
#include "touch_input.hpp"
#include "static_plugins.hpp"
#include "jni_bridge.hpp"
#include "amidi_driver.hpp"
#include "label_overlay.hpp"
#include "cable_park.hpp"
#include "selection_glow.hpp"
#include "tour_demo.hpp"

// Both sinks, always. logcat needs a USB cable and a developer at the other
// end of it; user/log.txt is the one a user can export and paste into an
// issue. Startup used to go to logcat only, which meant every bug report
// arrived without the version, the language, or anything else said here.
// logger::log is a no-op until logger::init(), so calling these earlier is
// safe -- it costs the file line, not correctness.
#define LOGI(...) do { __android_log_print(ANDROID_LOG_INFO, "rackdroid", __VA_ARGS__); INFO(__VA_ARGS__); } while (0)
#define LOGW(...) do { __android_log_print(ANDROID_LOG_WARN, "rackdroid", __VA_ARGS__); WARN(__VA_ARGS__); } while (0)
#define LOGE(...) do { __android_log_print(ANDROID_LOG_ERROR, "rackdroid", __VA_ARGS__); WARN(__VA_ARGS__); } while (0)

using namespace rack;


/** The language the frame loop started in. Rack's Help ▸ Language writes
straight into settings::language and tells nobody, so noticing means looking. */
static std::string g_language;

struct RackDroidApp {
	android_app* app = NULL;
	bool rackStarted = false;
	bool patchLaunched = false;

	float getDensity() {
		AConfiguration* config = app->config;
		int32_t density = config ? AConfiguration_getDensity(config) : 0;
		if (density <= 0)
			density = 160;
		return density / 160.f;
	}

	void startRack() {
		if (rackStarted)
			return;

		std::string filesDir = app->activity->internalDataPath;
		asset::systemDir = filesDir + "/system";
		asset::userDir = filesDir + "/user";
		system::createDirectories(asset::systemDir);
		system::createDirectories(asset::userDir);

		// Rebrand: "VCV" is a trademark not licensed for third-party ports.
		// APP_NAME is const but never constant-folded (heap-backed string);
		// overriding here avoids patching upstream sources.
		const_cast<std::string&>(APP_NAME) = "RackDroid";
		const_cast<std::string&>(APP_EDITION) = "";
		const_cast<std::string&>(APP_EDITION_NAME) = "";

		system::init();
		system::resetFpuFlags();
		asset::init();
		logger::logPath = asset::user("log.txt");
		logger::init();
		random::init();

		// APP_VERSION is the RACK ENGINE's version, not the app's, and printing
		// it right after APP_NAME read as "RackDroid 2.6.4" -- a version string
		// that does not exist and that no release can be matched to. The app's
		// own version lives on the Java side (BuildConfig.VERSION_NAME, logged
		// to user/java-log.txt, which is exported and shown beside this file).
		LOGI("%s on Rack engine %s, system=%s user=%s", APP_NAME.c_str(),
			APP_VERSION.c_str(), asset::systemDir.c_str(), asset::userDir.c_str());

		if (!rackdroid::extractSystemAssets(app->activity->assetManager, asset::systemDir))
			LOGE("asset extraction failed; continuing without system resources");
		// Apply the persisted rack color theme over the canonical panel/rail
		// SVGs BEFORE the engine (below) loads and caches any of them.
		rackdroid::applyRackTheme(asset::systemDir, asset::userDir);
		// Module browser tile art (ModuleThumbnails.kt reads PNGs straight
		// from filesDir/thumbnails/<pluginSlug>/<modelSlug>.png, matching
		// each model's "key" field from nativeBrowserModelsJson).
		std::string thumbsDir = filesDir + "/thumbnails";
		system::createDirectories(thumbsDir);
		if (!rackdroid::extractThumbnailAssets(app->activity->assetManager, thumbsDir))
			LOGE("thumbnail asset extraction failed; browser tiles fall back to text");
		rackdroid::seedDemoPatches(asset::systemDir, asset::userDir);

		string::init(); // translations, shipped in system.zip
		settings::init();
		try {
			settings::load();
		}
		catch (Exception& e) {
			LOGE("settings corrupted, resetting: %s", e.what());
		}
		// Rack's own menus and dialogs -- File, Edit, View, every confirm --
		// come from the translation files, which we ship for all seven
		// languages. But settings::language starts at "en" and the only thing
		// that ever changes it is Help > Language, buried two taps in. On an
		// Italian phone that left the app half translated: our strings in
		// Italian, Rack's in English, which is a mixture nobody chose.
		//
		// Default it from the device, once. The marker is written whatever
		// happens -- including when the device speaks a language we do not
		// ship -- so that Help > Language stays the last word ever after: a
		// user who deliberately picks English must not be overruled on the
		// next launch, and "the settings file has a language key" cannot tell
		// us anything, since save() always writes one.
		{
			std::string marker = asset::user("language-defaulted");
			// Only ever over the default. Existing installs have no marker, so
			// without this the update would flip somebody who had gone and
			// picked French on an English phone back to English, once.
			if (!system::isFile(marker) && settings::language == "en") {
				char code[3] = {0, 0, 0};
				if (app->config)
					AConfiguration_getLanguage(app->config, code);
				std::string want(code);
				for (const std::string& have : string::getLanguages()) {
					if (have == want) {
						settings::language = want;
						LOGI("Interface language defaulted to '%s' from the device",
							want.c_str());
						break;
					}
				}
				if (FILE* f = std::fopen(marker.c_str(), "w")) {
					std::fputc('\n', f);
					std::fclose(f);
				}
			}
		}

		// Baseline for checkLanguageChanged(). Taken AFTER the default above,
		// so following the device is not mistaken for the user choosing.
		g_language = settings::language;

		if (rackdroid::startupSafeModeRequested()) {
			settings::safeMode = true;
			LOGW("Recovery startup: autosave and user plugins disabled");
		}
		settings::headless = false;
		// Zero is not "unset", it is Rack's AUTO: the engine then takes the
		// rate the audio device actually opened (Engine::setSuggestedSampleRate,
		// fed by core/Audio.cpp). Forcing 48000 here -- which this did -- turned
		// Auto off on every fresh install, and on a device whose stream is not
		// 48 kHz the Audio module then resampled the whole time, on the audio
		// thread. That is a crackle nobody can explain from the outside.
		//
		// The migration is safe in a way that is worth stating: on a device that
		// really runs at 48 kHz, Auto resolves to 48000 and nothing changes at
		// all. It only differs where the forced value was wrong. Done once, so a
		// rate the user picks by hand afterwards is never overridden.
		{
			std::string marker = asset::user("samplerate-auto-migrated");
			if (!system::isFile(marker)) {
				if (settings::sampleRate == 48000.f) {
					settings::sampleRate = 0.f;
					LOGI("Engine sample rate moved to Auto: it follows the device now");
				}
				if (FILE* f = std::fopen(marker.c_str(), "w")) {
					std::fputc('\n', f);
					std::fclose(f);
				}
			}
		}
		// Everything a crackling report needs and could not previously carry.
		// Rack's getOperatingSystemInfo() goes down its Linux branch here and
		// answers "Linux <kernel> aarch64": true, and useless -- it does not
		// say which phone, which chip, or how many cores. The engine settings
		// are worse off still, because the two we ask users to change first,
		// sample rate and threads, were logged nowhere at all. Written after
		// the migration above so these are the values the run really uses.
		{
			char manufacturer[PROP_VALUE_MAX] = {0};
			char model[PROP_VALUE_MAX] = {0};
			char soc[PROP_VALUE_MAX] = {0};
			__system_property_get("ro.product.manufacturer", manufacturer);
			__system_property_get("ro.product.model", model);
			// ro.soc.model is the documented one (mandatory since Android 12);
			// ro.board.platform is the older vendor name, kept as a fallback
			// because it is the one that answers on the earlier devices we
			// still support.
			if (!__system_property_get("ro.soc.model", soc))
				__system_property_get("ro.board.platform", soc);
			LOGI("Device: %s %s, soc=%s, api=%d, cores=%d",
				manufacturer, model, soc[0] ? soc : "?",
				android_get_device_api_level(), system::getLogicalCoreCount());
		}
		if (settings::sampleRate <= 0.f)
			LOGI("Engine: sample rate auto, threads=%d", settings::threadCount);
		else
			LOGI("Engine: sample rate %g Hz (fixed), threads=%d",
				settings::sampleRate, settings::threadCount);

		// The welcome tips window is a fixed 550-unit-wide overlay that
		// cannot fit portrait phones, and its content is desktop-oriented
		// (right-click, Ctrl+drag, Enter). Never show it on launch.
		settings::showTipsOnLaunch = false;

		network::init();
		audio::init();
		rackdroid::oboeInit();
		midi::init();
		rackdroid::amidiInit();
		keyboard::init();
		midiloopback::init();
		plugin::init();
		rackdroid::loadStaticPlugins();
		app::browserInit();
		library::init();
		ui::init();
		window::init();

		contextSet(new Context);
		APP->midiLoopbackContext = new midiloopback::Context;
		APP->engine = new engine::Engine;
		APP->history = new history::State;
		APP->event = new widget::EventState;
		APP->scene = new app::Scene;
		APP->event->rootWidget = APP->scene;
		APP->patch = new patch::Manager;

		rackStarted = true;
		LOGI("Rack runtime started");
	}

	void createWindow() {
		if (!app->window)
			return;
		rackdroid::windowSetPendingSurface(app->window, getDensity());
		if (!APP->window) {
			try {
				APP->window = new window::Window;
			}
			catch (Exception& e) {
				LOGE("Window creation failed: %s", e.what());
				return;
			}
			if (!patchLaunched) {
				// Side-loaded .rdmod packs must be registered BEFORE the patch
				// is restored, or Rack reports their modules as missing and
				// drops them from the patch. Blocks (pumping the looper) until
				// Java has loaded them; see jni_bridge's loadUserPluginsBlocking
				// and MainActivity.loadUserPluginsFromNative.
				if (!rackdroid::userPluginsDisabled())
					rackdroid::loadUserPluginsBlocking();
				else
					LOGW("Recovery startup: skipped user plugin loading");
				// Loads the last patch or falls back to the template.
				APP->patch->launch("");
				APP->engine->startFallbackThread();
				rackdroid::installLabelOverlay();
				rackdroid::installCableParkBar();
				rackdroid::installSelectionGlow();
				patchLaunched = true;
				LOGI("Patch launched: %s", APP->patch->path.c_str());
				// Every plugin is registered and the patch is up: Java can now
				// build the model list and show the palette.
				rackdroid::nativePatchReady();
			}
		}
		else {
			rackdroid::windowSurfaceChanged(app->window);
		}
	}

	void stopRack() {
		if (!rackStarted)
			return;
		rackdroid::tourDemoRestore();
		if (APP->patch && !settings::safeMode) {
			try {
				// Persist the session like desktop autosave-on-quit.
				APP->patch->saveAutosave();
			}
			catch (Exception& e) {
				LOGE("autosave failed: %s", e.what());
			}
		}
		else if (settings::safeMode) {
			LOGI("Recovery startup: preserving the existing autosave");
		}
		settings::save();

		// Destructors (Window, Scene) use the APP macro: the context must
		// still be set while they run.
		delete APP;
		contextSet(NULL);

		window::destroy();
		ui::destroy();
		library::destroy();
		plugin::destroy();
		midi::destroy();
		audio::destroy();
		settings::destroy();
		logger::destroy();
		rackStarted = false;
	}
};


static void handleCmdInner(RackDroidApp* rd, int32_t cmd);

static void handleCmd(android_app* app, int32_t cmd) {
	RackDroidApp* rd = (RackDroidApp*) app->userData;
	try {
		handleCmdInner(rd, cmd);
	}
	catch (std::exception& e) {
		// Make the reason visible in logcat instead of dying silently through
		// the C glue (unwinding through it aborts anyway).
		LOGE("FATAL during app cmd %d: %s", cmd, e.what());
		throw;
	}
	catch (...) {
		LOGE("FATAL (unknown exception) during app cmd %d", cmd);
		throw;
	}
}


static void handleCmdInner(RackDroidApp* rd, int32_t cmd) {
	switch (cmd) {
		case APP_CMD_INIT_WINDOW:
			rd->startRack();
			rd->createWindow();
			break;
		case APP_CMD_TERM_WINDOW:
			if (rd->rackStarted)
				rackdroid::windowSurfaceLost();
			break;
		case APP_CMD_LOST_FOCUS:
			// Keep engine/audio running in background; only rendering stops
			// (Window::step() is a no-op without a surface).
			break;
		case APP_CMD_STOP:
			// Android may kill a stopped app without APP_CMD_DESTROY:
			// persist the session now. Undo any tour demonstration first, so a
			// module parked mid-animation is never what gets written out.
			if (rd->rackStarted)
				rackdroid::tourDemoRestore();
			if (rd->rackStarted && APP->patch) {
				try {
					APP->patch->saveAutosave();
					settings::save();
				}
				catch (Exception& e) {
					LOGE("autosave on stop failed: %s", e.what());
				}
			}
			break;
		case APP_CMD_DESTROY:
			rd->stopRack();
			break;
		default:
			break;
	}
}


static int32_t handleInput(android_app* app, AInputEvent* event) {
	RackDroidApp* rd = (RackDroidApp*) app->userData;
	if (!rd->rackStarted)
		return 0;
	// While an osdialog result is pending the glue thread is pumping from
	// inside a widget event handler: dispatching more touches would recurse
	// into Rack's event code. The dialog is modal, so just drain them.
	if (rackdroid::dialogIsPumping())
		return 0;
	return rackdroid::touchHandleEvent(event);
}


// One iteration of the glue event loop, used by jni_bridge to keep
// lifecycle cmds and the input queue serviced while a dialog is open
// (blocking there was the app's known input-timeout ANR).
static android_app* pumpApp = NULL;

/** Act on a language chosen from Rack's own menu. Rack asks for a restart
because it cannot relabel widgets already built; the other half of the app --
toolbar, palette, tour, every dialog of ours -- comes from Android resources,
which follow the DEVICE locale and have never heard of Rack's setting, so it
needs the restart even more. On a phone there is no reason to make the user
perform it: save, tell Java, and Java comes back up in the new language. */
static void checkLanguageChanged() {
	if (g_language.empty() || settings::language == g_language)
		return;
	std::string code = settings::language;
	g_language = code; // never fire twice while the restart is in flight
	LOGI("Interface language changed to '%s': saving and restarting", code.c_str());
	// Nothing else will save. The relaunch kills the process outright, so the
	// lifecycle callbacks that normally autosave never get their turn.
	rackdroid::tourDemoRestore();
	try {
		if (APP->patch && !settings::safeMode)
			APP->patch->saveAutosave();
		settings::save();
	}
	catch (Exception& e) {
		LOGE("save before the language restart failed: %s", e.what());
	}
	rackdroid::nativeLanguageChanged(code);
}

static void pumpGlueOnce(int timeoutMs) {
	int events;
	android_poll_source* source;
	int ident = ALooper_pollOnce(timeoutMs, NULL, &events, (void**) &source);
	if (ident >= 0 && source)
		source->process(pumpApp, source);
}


void android_main(android_app* app) {
	RackDroidApp rd;
	rd.app = app;
	app->userData = &rd;
	app->onAppCmd = handleCmd;
	app->onInputEvent = handleInput;

	rackdroid::jniInit(app->activity);
	pumpApp = app;
	rackdroid::jniSetPump(pumpGlueOnce);

	while (true) {
		int events;
		android_poll_source* source;
		// Recompute every iteration: 0 (render continuously, vsync paces us
		// via eglSwapBuffers) while a surface exists, block otherwise.
		int timeout = (rd.rackStarted && rackdroid::windowHasSurface()) ? 0 : -1;
		int ident = ALooper_pollOnce(timeout, NULL, &events, (void**) &source);
		if (ident >= 0 && source)
			source->process(app, source);

		if (app->destroyRequested) {
			rd.stopRack();
			return;
		}

		if (rd.rackStarted && APP->window && rackdroid::windowHasSurface()) {
			try {
				rackdroid::touchStep();
				rackdroid::processTourDemo();
				checkLanguageChanged();
				APP->window->step();
			}
			catch (std::exception& e) {
				LOGE("FATAL in frame step: %s", e.what());
				throw;
			}
		}
	}
}
