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
#include <dirent.h>
#include <cstring>

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
/** Give the engine's worker threads audio priority.

Rack names them ("Worker 0", "Worker 1", ...) and sets their FPU flags, and
that is all: they run at ordinary priority. The audio callback thread does
not -- AAudio raises it -- so on a big.LITTLE phone the workers can sit on a
slow core while the audio thread, which syncs with them TWICE PER SAMPLE
through a spin barrier, waits for the slowest of them. Every sample.

Rack creates the threads, so there is nothing to pass a priority to at
creation; the thread names are the only handle, and /proc/self/task is where
they are legible. A first version called setpriority() on the tid directly,
reasoning that a thread renicing its own process's threads needs no special
permission -- measured wrong: setpriority() to a negative nice value needs
CAP_SYS_NICE (or a raised RLIMIT_NICE) regardless of same-process/same-uid,
which an app has for none of its threads, so it silently failed on every tid,
every call, for the life of the session (confirmed on hardware: the workers
sat at nice +1, and the "raised" log line below never once printed). Real
audio-priority threads on Android (RenderThread, AAudio's callback thread)
get there through android.os.Process.setThreadPriority, which ALSO moves the
thread into the audio/foreground scheduler group (libcutils set_sched_policy)
-- a privilege an app is granted over its own threads that a raw setpriority()
syscall does not carry. So this goes through that Java call via JNI instead
(jniSetThreadPriority) rather than reimplementing it natively. */
static void applyWorkerPriority() {
	DIR* dir = opendir("/proc/self/task");
	if (!dir)
		return;
	int raised = 0;
	while (dirent* e = readdir(dir)) {
		int tid = atoi(e->d_name);
		if (tid <= 0)
			continue;
		char path[64];
		std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
		FILE* f = std::fopen(path, "r");
		if (!f)
			continue;
		char name[32] = {0};
		bool worker = std::fgets(name, sizeof(name), f)
			&& std::strncmp(name, "Worker ", 7) == 0;
		std::fclose(f);
		// -19 is URGENT_AUDIO. Not -20: that is reserved for the thread that
		// must never be late, and these are helpers to it, not it.
		if (worker && rackdroid::jniSetThreadPriority(tid, -19))
			raised++;
	}
	closedir(dir);
	if (raised > 0)
		LOGI("Engine: raised %d worker threads to audio priority", raised);
}

/** Workers are created by Engine::setThreadCount, which the Threads menu calls
and tells nobody about, so this watches the setting the same way the language
check below does. The delay is not decoration: a thread that has just been
created has not necessarily reached system::setThreadName yet, and until it
does it has no name to match. */
static void checkWorkerPriority() {
	static int lastThreadCount = -1;
	static double applyAt = 0.0;
	if (settings::threadCount != lastThreadCount) {
		lastThreadCount = settings::threadCount;
		applyAt = system::getTime() + 0.5;
	}
	if (applyAt > 0.0 && system::getTime() >= applyAt) {
		applyAt = 0.0;
		applyWorkerPriority();
	}
}

/** Shared between checkEngineOverload() and checkEngineUnderload().
g_escalatedThreads is >= 0 while an escalation THIS SESSION made is still in
effect; g_preEscalationThreads is what to put back. g_lastWrittenThreadCount
is the value as of the last look, so a change neither function just made --
the user, through the Threads menu -- can be told apart from one of their own
writes (which update it to match in the same breath). g_manualGuardUntil
holds the escalation check off for a few seconds after such a change: without
it, a manual drop made WHILE the engine is genuinely underrunning (the exact
moment someone reaches for the menu) reads as a fresh ceiling underrun on the
very next frame and gets escalated straight back before the user's own choice
ever has a chance to be felt. */
static int g_preEscalationThreads = -1;
static int g_escalatedThreads = -1;
static int g_lastWrittenThreadCount = -1;
static double g_manualGuardUntil = 0.0;

/** Held back for Android itself -- the compositor, system_server, the touch
pipeline -- so the engine's own thread pool never wants every core at once.
See engineThreadCeiling() for why one core, unconditionally, rather than a
fraction: a real device (OnePlus 8T, 8 cores) went sluggish system-wide, not
just RackDroid, the moment the pool held all 8 at real audio priority
(applyWorkerPriority() above). Android's own cpuset reservations (RenderThread
and friends normally live in "top-app"/"foreground") don't help here, because
moving Workers into the real-time scheduling class is specifically what lets
them preempt across that boundary -- the same privilege that fixed the
underruns in the first place. Lowering the priority back down for some
Workers was considered and rejected: the engine's barrier syncs every worker
twice per sample, so ANY one of them left preemptible stalls all the others
just as effectively as before the priority fix existed -- there is no such
thing as "mostly" holding the barrier. The only lever that does not reopen
that problem is asking for fewer threads, not lower-priority ones.

If one reserved core ever proves not to be enough on some device, the
sturdier fix is CPU-affinity pinning (keep the engine off one specific core
via sched_setaffinity) rather than reserving more threads -- not implemented,
no evidence yet that it is needed. */
static const int RESERVED_CORES = 1;

/** The most threads checkEngineOverload() will ever ask for, and the point
checkMaxedOutOverload() calls "nothing left to raise" -- one short of the
device's core count. A 1-core device has nothing to spare and keeps the
previous behavior of never escalating at all. */
static int engineThreadCeiling() {
	int cores = system::getLogicalCoreCount();
	return cores <= 1 ? cores : cores - RESERVED_CORES;
}

/** Spend the cores the device can give up, once it is clear the patch needs
them.

Measured on hardware, 224 modules at 48 kHz, underruns over 30 s: 1 thread
1522, 2 threads 2298, 4 threads 3217, 8 threads 22. Eight is the core count of
that phone. The shape is not a gentle curve with a peak in the middle -- the
fractions of the core count are all bad and the full count is a hundred times
better -- so the rule is "as many as we will ever ask for", never a smaller
fraction, and a device with four cores gets three (its ceiling) rather than
the two that would be its bad middle. (Eight cores was measured; the ceiling
introduced by RESERVED_CORES -- one short of that -- has not been separately
benchmarked yet, only reasoned about: see the comment above it.)

This does not run on a hunch: it reacts to the audio thread reporting a FRESH
underrun with the buffer already at its ceiling, which is the difference
between a patch that is too heavy and a device that merely jitters. Extra
threads are not free -- the engine syncs them twice per sample, which costs
battery and heat, and (now that applyWorkerPriority() actually reaches them)
idle workers spin at real audio priority too -- so a patch that never asks
never pays, and checkEngineUnderload() below is what stops a patch that asked
once from paying forever after.

Reacts to the ceiling-underrun COUNT moving, not merely "it happened at some
point this session": that is what lets a device checkEngineUnderload() has
already brought back down pay this cost again if a later patch earns it,
without a stale one-time flag standing in the way. The Threads menu still
keeps the last word in both directions: writing it by hand is detected below
and given a few seconds' peace before this reacts to anything. */
static void checkEngineOverload() {
	static int32_t lastCeilingCount = 0;
	int32_t ceilingCount = rackdroid::audioCeilingUnderrunCount();
	bool freshCeilingUnderrun = ceilingCount != lastCeilingCount;
	lastCeilingCount = ceilingCount;

	if (g_lastWrittenThreadCount < 0) {
		g_lastWrittenThreadCount = settings::threadCount; // first look, ever
	}
	else if (settings::threadCount != g_lastWrittenThreadCount) {
		// Not our doing (both of our own writes below update this to match),
		// so: the Threads menu. Whatever we thought was in effect no longer
		// is, and this gets a few seconds before any auto-adjustment reacts.
		g_lastWrittenThreadCount = settings::threadCount;
		g_manualGuardUntil = system::getTime() + 3.0;
		g_escalatedThreads = -1;
	}

	if (g_escalatedThreads >= 0 || !freshCeilingUnderrun)
		return;
	if (system::getTime() < g_manualGuardUntil)
		return;
	int ceiling = engineThreadCeiling();
	if (ceiling <= 1 || settings::threadCount >= ceiling)
		return;
	LOGW("Engine: underrunning with the buffer at its ceiling; raising threads "
		"from %d to %d, one short of this device's %d cores (Engine > Threads "
		"to change it back)",
		settings::threadCount, ceiling, ceiling + RESERVED_CORES);
	g_preEscalationThreads = settings::threadCount;
	g_escalatedThreads = ceiling;
	// Setting it is all that is needed: Engine::stepBlock relaunches its
	// workers from settings::threadCount on every block (Engine.cpp:572),
	// which is also how the Threads menu works -- it writes the setting and
	// nothing else.
	settings::threadCount = ceiling;
	g_lastWrittenThreadCount = ceiling;
}

/** Symmetric to checkEngineOverload() above: undoes an escalation THIS device
made, rather than leaving every core (and its worker's now-real audio-priority
spin loop, see applyWorkerPriority()) running flat out against a patch that no
longer asks for it.

This does NOT wait for things to go quiet first -- measured on hardware, they
often never do: a patch that genuinely needs only one thread underran at
~42-58/s with eight cores left spinning at real audio priority from an
earlier heavy patch, because the idle workers competing for CPU at that
priority are themselves enough to cause underruns. Waiting for quiet at the
escalated count is waiting for a condition the escalation itself prevents.

So instead this tries reverting after a flat cooldown, once, and leans on
checkEngineOverload() to correct a bad guess: if the patch genuinely still
needs every core, reverting immediately produces a fresh ceiling underrun,
which escalates straight back on the next frame it is checked (past its own
manual-change guard, since that write updates g_lastWrittenThreadCount to
match) -- a brief, self-healing dip rather than a wait that can never end.
Only ever reverts what checkEngineOverload() itself set: a manual change is
noticed by checkEngineOverload() above (called first each frame), which
already clears g_escalatedThreads, so by the time this looks the revert is
simply no longer its to make. */
static void checkEngineUnderload() {
	static double escalatedAt = 0.0;
	// Not a "how long until it's safe" number, just "don't re-litigate this
	// every frame" -- one bad section of a patch (a build-up, a dense fill)
	// gets this long before the guess is tried.
	static const double COOLDOWN_SEC = 15.0;

	if (g_escalatedThreads < 0) {
		escalatedAt = 0.0; // nothing of ours in effect; reset for next time
		return;
	}
	if (settings::threadCount != g_escalatedThreads) {
		// Already handled above (checkEngineOverload runs first each frame
		// and clears g_escalatedThreads the moment it sees this); just stop.
		escalatedAt = 0.0;
		return;
	}
	if (escalatedAt == 0.0) {
		escalatedAt = system::getTime(); // just escalated: start the clock
		return;
	}
	if (system::getTime() - escalatedAt < COOLDOWN_SEC)
		return;
	LOGW("Engine: trying threads back down from %d to %d after %.0fs "
		"(escalates straight back if that turns out to still be too few; "
		"Engine > Threads to change it back)",
		settings::threadCount, g_preEscalationThreads, COOLDOWN_SEC);
	settings::threadCount = g_preEscalationThreads;
	g_lastWrittenThreadCount = g_preEscalationThreads;
	g_escalatedThreads = -1;
	escalatedAt = 0.0;
}

/** Set once checkBlockSizeOverload() below has made its one attempt (whether
or not it actually changed anything). Read by checkMaxedOutOverload() so its
"nothing left to raise" diagnosis waits for the block-size lever too, not
just thread count, before calling the situation hopeless. */
static bool g_blockSizeTried = false;

/** The second, much blunter lever, tried only after checkEngineOverload()
above has already spent the thread-count one: reaches for a bigger block
size. NOT symmetric with checkEngineUnderload() on purpose -- changing block
size closes and reopens the audio stream (audioSetBlockSize(), in
audio_oboe.cpp), an audible gap, not a free reallocation the way changing
thread count is. Reverting it later would buy back only latency, never CPU
or heat, so there is little reason to want it back down automatically the
way an idle thread is, and doing so on a cooldown timer the way
checkEngineUnderload() does would mean a real dropout every 15 seconds for a
much weaker reason. So: escalate once, ever, per session; never revert. A
user who wants lower latency back can pick a smaller block size by hand in
the Audio module, same as always.

Measured on hardware (see audio_oboe.cpp's DEFAULT_BLOCK_SIZE comment):
doubling block size roughly halves underruns, so this tries exactly one
further doubling -- to 1024, the top of Rack's own block-size list -- not a
ladder down from there. */
static void checkBlockSizeOverload() {
	static int32_t lastCeilingCount = 0;
	int32_t ceilingCount = rackdroid::audioCeilingUnderrunCount();
	bool freshCeilingUnderrun = ceilingCount != lastCeilingCount;
	lastCeilingCount = ceilingCount;

	if (g_blockSizeTried || !freshCeilingUnderrun)
		return;
	int ceiling = engineThreadCeiling();
	if (ceiling <= 1 || settings::threadCount < ceiling)
		return; // the cheaper lever hasn't been maxed yet; let it go first
	int current = rackdroid::audioBlockSize();
	if (current <= 0)
		return; // no device open yet -- wait for one rather than trying nothing
	g_blockSizeTried = true;
	if (current >= 1024)
		return; // already at the top of Rack's own block-size list
	int next = current * 2;
	LOGW("Engine: still underrunning at this device's thread ceiling; trying "
		"block size %d instead of %d (Audio module > Block size to change it "
		"back -- this will not be undone automatically)",
		next, current);
	rackdroid::audioSetBlockSize(next);
}

/** Surfaces the one case the two levers above can do nothing further about:
already at engineThreadCeiling() -- our own limit, one core short of the
device's count, never the hardware max itself -- already past
checkBlockSizeOverload()'s one attempt, and still producing fresh ceiling
underruns. Until now that state was invisible -- the audio thread keeps
warning to a log file nobody but a developer reads, and the user just hears
crackling with no explanation, forever, since there is no "try lowering it"
step symmetric to checkEngineUnderload() for a number that was never raised
in the first place.

Distinguishes two causes, because they call for different reactions: the
device's OWN thermal throttling (Android's verdict, SEVERE or worse) means
it has less to give right now than its core count promises, and likely
recovers once it cools -- waiting helps. Short of that, the patch itself
asks for more than this device has even at full tilt and cooled; no amount
of waiting fixes that, and the honest answer is to simplify the patch or
lower the sample rate. Once per session: this is a diagnosis, not a
running commentary. */
static void checkMaxedOutOverload() {
	static bool shown = false;
	static int32_t lastCeilingCount = 0;

	int32_t ceilingCount = rackdroid::audioCeilingUnderrunCount();
	bool freshCeilingUnderrun = ceilingCount != lastCeilingCount;
	lastCeilingCount = ceilingCount;

	if (shown || !freshCeilingUnderrun || !g_blockSizeTried)
		return;
	int ceiling = engineThreadCeiling();
	// Below our ceiling: there is still room for checkEngineOverload() to
	// act, so this is not yet the maxed-out case. (A manual pick at or above
	// the hardware max also counts as maxed out here, same as before -- it is
	// >= ceiling too.)
	if (ceiling <= 1 || settings::threadCount < ceiling)
		return;
	shown = true;

	int thermal = rackdroid::thermalStatus();
	// PowerManager.THERMAL_STATUS_SEVERE = 3.
	bool throttled = thermal >= 3;
	LOGW("Engine: underrunning at %d threads (this device's ceiling, one core "
		"short of its %d) with nothing left to raise; thermal status %d (%s)",
		ceiling, ceiling + RESERVED_CORES, thermal, throttled ? "throttled" : "not throttled");
	rackdroid::showEngineNotice(throttled ? 1 : 0);
}

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
				checkWorkerPriority();
			checkEngineOverload();
			checkEngineUnderload();
			checkBlockSizeOverload();
			checkMaxedOutOverload();
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
