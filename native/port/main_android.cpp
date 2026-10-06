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
#include <cerrno>
#include <vector>
#include <algorithm>
#include <sched.h>

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
#include "adpf.hpp"
#include "window_android.hpp"
#include "touch_input.hpp"
#include "static_plugins.hpp"
#include "jni_bridge.hpp"
#include "amidi_driver.hpp"
#include "label_overlay.hpp"
#include "cable_park.hpp"
#include "selection_glow.hpp"
#include "tour_demo.hpp"
#include "engine_barrier.hpp"

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
		// Keep the session before this one. logger::init() opens the file for
		// writing, so the log of a launch that went wrong is destroyed by the
		// very next launch -- and the next launch is what the user does first,
		// every time, before it occurs to anyone to ask for a log. That has
		// now cost this project four separate investigations where the only
		// copy of the interesting run had already been overwritten, including
		// a startup hang that had to be chased on a screenshot of an "app
		// isn't responding" dialog because the evidence was gone.
		//
		// One rename, on a file a few hundred kilobytes at most, before
		// anything opens it. The previous log is exported beside the current
		// one, so a user who reproduces a fault and then reopens the app --
		// which is the only way they can reach the export at all -- still has
		// the run that failed.
		{
			std::string prev = asset::user("log-previous.txt");
			std::remove(prev.c_str());
			std::rename(logger::logPath.c_str(), prev.c_str());
		}
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
		// come from the translation files, which we ship for all seven languages.
		// But settings::language starts at "en", and the only thing that ever
		// changes it is Help > Language, buried two taps in. That left the app
		// half translated on an Italian phone: our strings in Italian, Rack's in
		// English, a mixture nobody chose. Seen on a real 8T, whose menu button
		// said MOTORE while the rows inside it said "Performance meters".
		//
		// The two halves follow different masters -- ours the Android resource
		// locale, Rack's this setting -- so keeping them together means deciding
		// which wins. Java knows, and nothing else does: it writes ui/lang if and
		// only if the user picked a language from that menu. Empty therefore means
		// no choice has ever been made, and following the device is not overruling
		// anybody. A real choice is left alone -- and since making one moves BOTH
		// halves, they cannot disagree afterwards.
		//
		// This replaces a one-shot marker file that recorded only THAT a default
		// had been attempted, never what it found. A launch where the device
		// language could not be read wrote the marker anyway and gave up for
		// good, which is how a phone ends up permanently half-translated.
		{
			std::string chosen = rackdroid::startupChosenLanguage();
			char code[3] = {0, 0, 0};
			if (app->config)
				AConfiguration_getLanguage(app->config, code);
			std::string device(code);
			if (!chosen.empty()) {
				if (settings::language != chosen) {
					LOGI("Interface language: following the choice made in Help > "
						"Language ('%s')", chosen.c_str());
					settings::language = chosen;
				}
			}
			else if (!device.empty()) {
				bool shipped = false;
				for (const std::string& have : string::getLanguages()) {
					if (have == device) {
						shipped = true;
						break;
					}
				}
				if (shipped && settings::language != device) {
					LOGI("Interface language: nobody has chosen one, following the "
						"device ('%s')", device.c_str());
					settings::language = device;
				}
				else if (!shipped) {
					LOGI("Interface language: the device speaks '%s', which is not "
						"one of the seven shipped; staying on '%s'",
						device.c_str(), settings::language.c_str());
				}
			}
			else {
				LOGW("Interface language: could not read the device's own; staying "
					"on '%s' and asking again next launch", settings::language.c_str());
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

		// On Android this ONE setting has one job, and it is not the frame
		// rate: nothing in this port sleeps on it. Window::getFrameDurationRemaining()
		// divides by it, and FramebufferWidget::draw() uses THAT to decide
		// whether it still has time to re-render a module's framebuffer or
		// should draw the one it already has, scaled. Upstream's own way of
		// keeping a heavy frame from running long -- and we had never set it.
		//
		// FramebufferWidget keeps re-rendering while the frame is younger
		// than 1/frameRateLimit plus a hardcoded 1/60 s allowance: 33 ms at
		// the inherited 60, 25 ms at 120. So this trims a slow frame, it does
		// not cap it at a budget -- no value below 16.7 ms is reachable, and
		// past 120 the gain is a few milliseconds. With a seven-module patch
		// every module fits inside either figure, so a pinch still rebuilds
		// them all.
		//
		// Kept because it is the direction upstream intended and costs
		// nothing. It did NOT fix "si interrompe mentre si fa lo zoom" on a
		// Nothing A024: the log after it showed the audio callback stalled
		// for up to a second, which no amount of drawing on another core
		// explains. audioReportSlowCallbacks() is what looks for the cause.
		settings::frameRateLimit = 120.f;

		network::init();
		audio::init();
		// Before opening the Oboe stream, not after: AAudio's audio policy
		// service weighs the caller's current focus/attributes state when
		// deciding whether to grant an Exclusive stream or fall back to
		// Shared -- asking too late would not un-negotiate the first stream
		// open. See requestAudioFocus()'s doc comment (jni_bridge.hpp) for why
		// this exists at all; the return value is just for the log, since the
		// stream opens the same way either way.
		if (!rackdroid::requestAudioFocus())
			LOGW("Engine: audio focus request was denied or unavailable; the "
				"audio stream may be granted Shared instead of Exclusive mode "
				"if another app is also using audio");
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
			// Startup phase timings. The gap between "Rack runtime started" and
			// "Patch launched" was over a second and nothing said what was in
			// it; guessing "patch loading" is how the two audio-stream wastes
			// went unnoticed for as long as they did.
			double tWindow = system::getTime();
			try {
				APP->window = new window::Window;
			}
			catch (Exception& e) {
				LOGE("Window creation failed: %s", e.what());
				return;
			}
			LOGI("Startup: window created in %.0f ms",
				(system::getTime() - tWindow) * 1000.0);
			if (!patchLaunched) {
				// Side-loaded .rdmod packs must be registered BEFORE the patch
				// is restored, or Rack reports their modules as missing and
				// drops them from the patch. Blocks (pumping the looper) until
				// Java has loaded them; see jni_bridge's loadUserPluginsBlocking
				// and MainActivity.loadUserPluginsFromNative.
				double tPlugins = system::getTime();
				if (!rackdroid::userPluginsDisabled())
					rackdroid::loadUserPluginsBlocking();
				else
					LOGW("Recovery startup: skipped user plugin loading");
				LOGI("Startup: user plugins loaded in %.0f ms",
					(system::getTime() - tPlugins) * 1000.0);
				// Loads the last patch or falls back to the template.
				double tPatch = system::getTime();
				APP->patch->launch("");
				LOGI("Startup: patch loaded in %.0f ms",
					(system::getTime() - tPatch) * 1000.0);
				APP->engine->startFallbackThread();
				rackdroid::installLabelOverlay();
				rackdroid::windowInstallDrawMarkers();
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
		if (APP->patch && patchLaunched && !settings::safeMode) {
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
		case APP_CMD_WINDOW_RESIZED:
		case APP_CMD_CONFIG_CHANGED:
		case APP_CMD_LOST_FOCUS:
		case APP_CMD_GAINED_FOCUS:
			// Keep engine/audio running in background; only rendering stops
			// (Window::step() is a no-op without a surface). But every one of
			// these costs a burst of dropped frames and underruns, and the
			// thread tuner must not read that as the patch being too heavy.
			// Rotation in particular never reaches TERM_WINDOW at all: the
			// activity declares configChanges for orientation, so the surface
			// survives and only a resize arrives.
			rackdroid::windowNoteSurfaceChange();
			break;
		case APP_CMD_STOP:
			// Android may kill a stopped app without APP_CMD_DESTROY:
			// persist the session now. Undo any tour demonstration first, so a
			// module parked mid-animation is never what gets written out.
			if (rd->rackStarted)
				rackdroid::tourDemoRestore();
			// Only a patch that has been LOADED is worth writing out. Startup
			// pumps this very queue while the user's plugin packs load, so a
			// stop arriving then -- the screen locking, the user switching
			// away during a slow start -- used to save the still-empty engine
			// over the autosave, and the load that followed read the empty
			// patch back: the session was gone without a word. Seen on a
			// OnePlus 8T launched with the screen locked. Nor in a recovery
			// start, which promises to leave the autosave as it found it.
			if (rd->rackStarted && rd->patchLaunched && APP->patch && !settings::safeMode) {
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
	int prev = rackdroid::windowPhase();
	rackdroid::windowSetPhase(rackdroid::RENDER_INPUT);
	int handled = rackdroid::touchHandleEvent(event);
	rackdroid::windowSetPhase(prev);
	return handled;
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

/** Which logical CPU applyWorkerAffinity() below reserves for the system.
Picks the one with the lowest maximum clock frequency -- a LITTLE/efficiency
core -- rather than assuming an index. Core numbering order is NOT consistent
across SoC vendors: verified the hard way. Pinning off cpu(cores-1) worked
perfectly on the S22 (0 underruns/30s on a heavy patch), but made a genuinely
light patch underrun continuously on a real OnePlus 8T (Snapdragon 865),
whose highest-numbered core -- cpu7 -- is the single fastest "prime" core,
not a spare one; reserving it forced every worker onto weaker cores instead.
Reading actual clock ceilings sidesteps the whole question of which vendor's
convention applies. Falls back to cores-1 (the previous, S22-verified
behavior) if the frequency files cannot be read at all -- e.g. no
CONFIG_CPU_FREQ, or restricted sysfs -- rather than guessing further. */
static int pickReservedCpu(int cores) {
	int bestCpu = cores - 1;
	long bestFreq = -1;
	bool ok = true;
	for (int cpu = 0; cpu < cores; cpu++) {
		char path[96];
		std::snprintf(path, sizeof(path),
			"/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
		FILE* f = std::fopen(path, "r");
		if (!f) {
			ok = false;
			break;
		}
		long freq = -1;
		int n = std::fscanf(f, "%ld", &freq);
		std::fclose(f);
		if (n != 1 || freq <= 0) {
			ok = false;
			break;
		}
		if (bestFreq < 0 || freq < bestFreq) {
			bestFreq = freq;
			bestCpu = cpu;
		}
	}
	return ok ? bestCpu : cores - 1;
}


/** The core the audio callback thread is pinned to, so that no Worker can ever
share one with it. Picked as the fastest core (the last of equals, which on
every big.LITTLE layout seen so far is the prime core), never the one
pickReservedCpu() keeps for the system. -1 when there are too few cores to
set one aside.

Why the callback needs a core of its own, measured on a Nothing A024 with a
seven-module patch at two threads: "slow callback: 1633.4 ms wall, 1628.0 ms
cpu, switches 0 voluntary". A second and a half of CPU for a block that
normally takes 14% of 10 ms, without once going to sleep -- the callback was
spinning. Engine::stepFrame syncs every thread at SpinBarrier twice per sample,
and a spinner is only as fast as the slowest thread it waits for. The callback
runs SCHED_FIFO (AAudio grants it); the Workers are ordinary threads at nice
-19. When the scheduler puts both on one core, the real-time spinner preempts
the very Worker it is waiting for, and nothing can run that Worker there until
the kernel's real-time throttle steps in or the load balancer moves it -- the
stalls measured were 0.3 to 1.9 s. It surfaced during a pinch because that is
when the render thread keeps the other cores busy enough for the collision to
happen and to last. More threads only add more chances of it, which is why the
tuner climbing to seven made it worse, never better. */
static int pickAudioCpu(int cores, int reservedCpu) {
	if (cores <= 2)
		return -1;
	int bestCpu = -1;
	long bestFreq = -1;
	for (int cpu = 0; cpu < cores; cpu++) {
		if (cpu == reservedCpu)
			continue;
		char path[96];
		std::snprintf(path, sizeof(path),
			"/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
		FILE* f = std::fopen(path, "r");
		if (!f)
			return reservedCpu == cores - 1 ? cores - 2 : cores - 1;
		long freq = -1;
		int n = std::fscanf(f, "%ld", &freq);
		std::fclose(f);
		if (n != 1 || freq <= 0)
			return reservedCpu == cores - 1 ? cores - 2 : cores - 1;
		if (freq >= bestFreq) {
			bestFreq = freq;
			bestCpu = cpu;
		}
	}
	return bestCpu;
}


/** The engine's worker threads, by id. One walk of /proc/self/task rather
than the three that would otherwise be needed -- priority, affinity and the
ADPF hint session all want the same list, and they are all applied together
whenever the tuner changes the count. */
static std::vector<int> collectWorkerThreads() {
	std::vector<int> workers;
	DIR* dir = opendir("/proc/self/task");
	if (!dir)
		return workers;
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
		if (worker)
			workers.push_back(tid);
	}
	closedir(dir);
	// Directory order is not promised to repeat, and callers compare sets.
	std::sort(workers.begin(), workers.end());
	return workers;
}


static void applyWorkerPriority(const std::vector<int>& workers) {
	int raised = 0;
	for (int tid : workers) {
		// -19 is URGENT_AUDIO. Not -20: that is reserved for the thread that
		// must never be late, and these are helpers to it, not it.
		if (rackdroid::jniSetThreadPriority(tid, -19))
			raised++;
	}
	if (raised > 0)
		LOGI("Engine: raised %d worker threads to audio priority", raised);
}


/** The callback thread applyWorkerAffinity() last dealt with, and the core it
got -- -1 when no core would take it. */
static int g_pinnedAudioTid = 0;
static int g_pinnedAudioCpu = -1;
/** There is a core to give it at all (more than two cores). */
static bool g_audioPinWanted = false;
/** Cores that refused the callback or let go of it, one bit each: not asked
again this session. A Snapdragon's prime core is the fastest and also the one
the SoC halts whenever the load allows -- an 8T took the pin on cpu7 at 0.8 s
and dropped it at 4 s, and until the next look the callback wandered over the
Workers' cores: four callbacks of 43-75 ms, 8 underruns. The second-fastest
core, which stays up, is the better home. */
static uint64_t g_audioCpusGivenUp = 0;

/** True while the callback thread is still on the one core it was given.

It does not stay there by itself. On a Nothing A024 (SM8735) the first pin
failed outright -- "could not pin the audio callback thread to cpu7: Invalid
argument" -- because the SoC halts its prime core when the load is low, and a
halted core is not a valid affinity. The old code tried once per thread and
remembered the thread as dealt with, so it never tried again, went on telling
the Workers to keep off cpu7 "the audio callback's", and the callback floated
over the Workers' cores for the rest of the stream: 0.2 to 4 s spins, the very
thing the pin exists to prevent. And a core halted AFTER a successful pin
has the kernel move the thread off it and widen its mask, which looks the same
from here. */
static bool audioStillPinned() {
	int tid = rackdroid::audioCallbackThreadTid();
	if (tid <= 0 || tid != g_pinnedAudioTid || g_pinnedAudioCpu < 0)
		return false;
	cpu_set_t mask;
	CPU_ZERO(&mask);
	if (sched_getaffinity(tid, sizeof(mask), &mask) != 0)
		return false;
	return CPU_COUNT(&mask) == 1 && CPU_ISSET(g_pinnedAudioCpu, &mask);
}


/** A core's top clock, or -1 when sysfs will not say. */
static long cpuMaxFreq(int cpu) {
	char path[96];
	std::snprintf(path, sizeof(path),
		"/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
	FILE* f = std::fopen(path, "r");
	if (!f)
		return -1;
	long freq = -1;
	if (std::fscanf(f, "%ld", &freq) != 1)
		freq = -1;
	std::fclose(f);
	return freq;
}


/** Keeps the render thread -- the caller -- off as many of the fastest cores
as there are Workers, so that each Worker has a core the drawing cannot take.

Drawing is the one other heavy thing this process does, and it peaks exactly
when the user zooms: every module's framebuffer is rebuilt, 50-130 ms of solid
work per frame. A Worker sharing that core is run in slices, the engine waits
for it at the barrier twice per sample, and the callback is late by the length
of a slice. Measured with the same patch and a wheel-zoom in and out for forty
seconds on a OnePlus 8T: 6 underruns and callbacks of 50-70 ms that spent all
but 8 ms asleep, waiting. A Nothing A024 showed the same thing as 6-12 ms and
"a little crackle while zooming".

Only the render thread is fenced in. The Workers keep the whole mask they had:
confining each to one core would hand the same stall to the first core the SoC
halts. And the render thread always keeps at least two cores, however many
Workers there are. */
static void applyRenderAffinity(int cores, int audioCpu, int workerCount) {
	std::vector<int> order; // every core but the callback's, fastest first
	for (int cpu = cores - 1; cpu >= 0; cpu--) {
		if (cpu != audioCpu)
			order.push_back(cpu);
	}
	std::stable_sort(order.begin(), order.end(), [](int a, int b) {
		return cpuMaxFreq(a) > cpuMaxFreq(b);
	});
	int fenced = std::min(workerCount, (int) order.size() - 2);
	if (fenced < 0)
		fenced = 0;
	cpu_set_t mask;
	CPU_ZERO(&mask);
	std::string off;
	for (int i = 0; i < (int) order.size(); i++) {
		if (i < fenced)
			off += string::f(" cpu%d", order[i]);
		else
			CPU_SET(order[i], &mask);
	}
	static std::string lastSaid;
	std::string say;
	if (sched_setaffinity(0, sizeof(mask), &mask) != 0)
		say = string::f("Engine: could not move the render thread off the workers' cores: %s",
			strerror(errno));
	else if (fenced > 0)
		say = string::f("Engine: render thread kept off%s, left to the %d worker threads",
			off.c_str(), workerCount);
	if (!say.empty() && say != lastSaid)
		LOGI("%s", say.c_str());
	lastSaid = say;
}


/** Where the engine's threads are running right now, for the line after a slow
callback. Read a moment AFTER the callback, from the render thread -- it says
where the threads live, not where they were in that instant. */
static void reportEngineThreadCores() {
	static double saidAt = 0.0;
	double now = system::getTime();
	if (now - saidAt < 1.0)
		return;
	saidAt = now;
	std::string where;
	for (int tid : collectWorkerThreads()) {
		char path[64];
		std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
		FILE* f = std::fopen(path, "r");
		if (!f)
			continue;
		char buf[512] = {0};
		size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
		std::fclose(f);
		buf[n] = 0;
		// "pid (comm) state ..." -- comm holds a space ("Worker 1"), so count
		// fields from the closing bracket: state is the 3rd, processor the 39th.
		const char* p = std::strrchr(buf, ')');
		int cpu = -1;
		for (int field = 2; p && field < 39; field++) {
			p = std::strchr(p + 1, ' ');
		}
		if (p)
			cpu = atoi(p + 1);
		where += string::f(" cpu%d", cpu);
	}
	LOGI("Engine: just after that, render thread on cpu%d, workers on%s",
		sched_getcpu(), where.empty() ? " none" : where.c_str());
}


static void applyWorkerAffinity(const std::vector<int>& workers) {
	int cores = system::getLogicalCoreCount();
	if (cores <= 1)
		return;
	int reservedCpu = pickReservedCpu(cores);
	int audioCpu = pickAudioCpu(cores, reservedCpu);
	g_audioPinWanted = audioCpu >= 0;

	// The callback first: it is the thread the Workers must never meet. Redone
	// whenever it is not where it was put -- a reopened stream brings a new
	// thread, and checkWorkerPriority() calls back here for that and for a
	// pin that failed or was undone.
	int audioTid = rackdroid::audioCallbackThreadTid();
	if (audioCpu >= 0 && audioTid > 0 && !audioStillPinned()) {
		int before = g_pinnedAudioTid == audioTid ? g_pinnedAudioCpu : -2;
		// Same thread, and it had a core: that core let go of it.
		if (before >= 0)
			g_audioCpusGivenUp |= (uint64_t) 1 << before;
		g_pinnedAudioTid = audioTid;
		g_pinnedAudioCpu = -1;
		int err = 0;
		// The fastest core first, then the others from the top down: when the
		// first choice is halted, any core of its own beats none.
		for (int i = -1; i < cores && g_pinnedAudioCpu < 0; i++) {
			int cpu = i < 0 ? audioCpu : cores - 1 - i;
			if (cpu == reservedCpu || (i >= 0 && cpu == audioCpu))
				continue;
			if ((g_audioCpusGivenUp >> cpu) & 1)
				continue;
			cpu_set_t audioMask;
			CPU_ZERO(&audioMask);
			CPU_SET(cpu, &audioMask);
			if (sched_setaffinity(audioTid, sizeof(audioMask), &audioMask) == 0) {
				g_pinnedAudioCpu = cpu;
			}
			else {
				if (i < 0)
					err = errno;
				g_audioCpusGivenUp |= (uint64_t) 1 << cpu;
			}
		}
		// Said once per outcome, not once per retry.
		if (g_pinnedAudioCpu != before) {
			if (g_pinnedAudioCpu < 0)
				LOGW("Engine: could not pin the audio callback thread to any core "
					"(cpu%d: %s); will keep trying", audioCpu, strerror(err));
			else if (g_pinnedAudioCpu != audioCpu)
				LOGW("Engine: cpu%d would not keep the audio callback thread%s%s; "
					"pinned it to cpu%d instead, which no worker may use",
					audioCpu, err ? ": " : "", err ? strerror(err) : "",
					g_pinnedAudioCpu);
			else
				LOGI("Engine: pinned the audio callback thread to cpu%d, which no "
					"worker may use", audioCpu);
		}
	}
	// The Workers keep off the core the callback is actually on, never off
	// one it merely should have been on: that costs a core and buys nothing.
	audioCpu = (audioTid > 0 && audioTid == g_pinnedAudioTid) ? g_pinnedAudioCpu : -1;

	cpu_set_t mask;
	CPU_ZERO(&mask);
	for (int cpu = 0; cpu < cores; cpu++) {
		if (cpu != reservedCpu && cpu != audioCpu)
			CPU_SET(cpu, &mask);
	}
	// Workers launched from now on take this mask as they start, instead of
	// inheriting the callback's single core. See engineWorkerStarted().
	uint64_t bits = 0;
	for (int cpu = 0; cpu < cores && cpu < 64; cpu++) {
		if (CPU_ISSET(cpu, &mask))
			bits |= (uint64_t) 1 << cpu;
	}
	rackdroid::engineSetWorkerCpus(bits);
	int pinned = 0;
	for (int tid : workers) {
		if (sched_setaffinity(tid, sizeof(mask), &mask) == 0)
			pinned++;
	}
	// Not repeated while nothing about it changes: the callback's pin is
	// retried every two seconds for as long as it fails.
	static std::string lastSaid;
	std::string say;
	if (pinned > 0 && audioCpu >= 0)
		say = string::f("Engine: pinned %d worker threads off cpu%d, reserved for the system, "
			"and cpu%d, the audio callback's", pinned, reservedCpu, audioCpu);
	else if (pinned > 0)
		say = string::f("Engine: pinned %d worker threads off cpu%d, reserved for the system",
			pinned, reservedCpu);
	if (!say.empty() && say != lastSaid)
		LOGI("%s", say.c_str());
	lastSaid = say;

	applyRenderAffinity(cores, audioCpu, (int) workers.size());
}


/** Tells ADPF which threads are doing the work: the audio callback first --
it is the one with the deadline -- then the engine's workers behind it. */
static void applyAdpfThreads(const std::vector<int>& workers) {
	int audioTid = rackdroid::audioCallbackThreadTid();
	if (audioTid <= 0)
		return; // no callback has run yet; nothing to describe
	std::vector<int> tids;
	tids.reserve(workers.size() + 1);
	tids.push_back(audioTid);
	tids.insert(tids.end(), workers.begin(), workers.end());
	rackdroid::adpfSetThreads(tids.data(), tids.size());
}

/** Workers are created by Engine::setThreadCount, which the Threads menu calls
and tells nobody about, so this watches the setting the same way the language
check below does. The delay is not decoration: a thread that has just been
created has not necessarily reached system::setThreadName yet, and until it
does it has no name to match. Applies both the priority boost and the CPU
affinity exclusion together: both act on the same "Worker N" threads, and
both need to be redone every time Engine::setThreadCount recreates them. */
static void checkWorkerPriority() {
	static int lastThreadCount = -1;
	static double applyAt = 0.0;
	// The Workers that have been given their priority and cores so far.
	static std::vector<int> servedWorkers;
	if (settings::threadCount != lastThreadCount) {
		lastThreadCount = settings::threadCount;
		applyAt = system::getTime() + 0.5;
	}
	// Half a second was a guess at how long the engine takes to replace its
	// Workers after the count changes, and it was also the only trigger: a
	// patch loaded later than that got Workers nobody was waiting for, which
	// ran at ordinary priority on whatever cores they were born on until the
	// next thing happened to ask. A Nothing A024 logged its pin 0.57 s after a
	// patch had loaded and 504 underruns in that same second; a OnePlus 8T
	// 0.5 s after. So look, twenty times a second, and serve a new set the
	// moment it is complete. The timer stays for the first launch of the set.
	static double scanAt = 0.0;
	if (system::getTime() >= scanAt) {
		scanAt = system::getTime() + 0.05;
		std::vector<int> now = collectWorkerThreads();
		if (now != servedWorkers && (int) now.size() == settings::threadCount - 1)
			applyAt = system::getTime();
	}
	// A reopened stream (block size change, route change) calls back on a new
	// thread, which is neither pinned nor known to ADPF. No delay needed: the
	// id is only published from inside a running callback.
	static int lastAudioTid = 0;
	int audioTid = rackdroid::audioCallbackThreadTid();
	if (audioTid > 0 && audioTid != lastAudioTid) {
		lastAudioTid = audioTid;
		if (applyAt <= 0.0)
			applyAt = system::getTime();
	}
	// The callback's pin can fail or be undone behind our back (see
	// audioStillPinned()); look four times a second -- one syscall -- and redo
	// it when it has. Every 2 s left the callback loose for up to that long.
	static double pinCheckAt = 0.0;
	if (audioTid > 0 && applyAt <= 0.0 && system::getTime() >= pinCheckAt) {
		pinCheckAt = system::getTime() + 0.25;
		// Only the affinity: priority and ADPF have not changed, and each
		// would say so in the log every two seconds.
		if (g_audioPinWanted && !audioStillPinned())
			applyWorkerAffinity(collectWorkerThreads());
	}
	if (applyAt > 0.0 && system::getTime() >= applyAt) {
		applyAt = 0.0;
		std::vector<int> workers = collectWorkerThreads();
		servedWorkers = workers;
		applyWorkerPriority(workers);
		applyWorkerAffinity(workers);
		applyAdpfThreads(workers);
	}
}

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

This is the soft half of the guarantee: it assumes the scheduler leaves the
excluded core alone, and stops meaning anything if the user overrides the
count through the Threads menu. applyWorkerAffinity() above is the hard
half -- CPU-affinity pinning that excludes Worker threads from one specific
core by mask, independent of how many of them there end up being. The two
together are belt and suspenders, not either/or: a device where the
scheduler already behaved would see no difference from the affinity mask,
and a manual thread-count override still can't undo it. */
static const int RESERVED_CORES = 1;

/** Highest thread count checkThreadCount() keeps a score for. */
static const int MAX_TRACKED_THREADS = 64;


/** How long after the last touch the engine keeps its hands off the thread
count. Covers the gesture itself and the moment after it, so a pinch or a drag
is never mistaken for the patch outgrowing its threads. */
static const double INTERACTION_SETTLE_SEC = 2.0;

/** True once the audio device has been open long enough that what it reports
means something. Starting up is not a measurement: the patch is still loading,
Rack rewrites the audio port's settings several times, and the underruns that
produces say nothing about the workload. Both levers below consult this --
checkThreadCount() was already discarding that window, checkBlockSizeOverload()
was not, and reacted to it on every single launch by doubling the block size,
which costs a stream close (~170 ms blocking) plus an open: 677 ms of startup
and an audible gap, spent redoing a decision the previous run had made. */
static bool startupSettled() {
	static const double WARMUP_SEC = 5.0;
	static double audioReadyAt = 0.0;
	if (rackdroid::audioBlockSize() <= 0) {
		audioReadyAt = 0.0; // no device yet; the clock has not started
		return false;
	}
	double now = system::getTime();
	if (audioReadyAt <= 0.0) {
		audioReadyAt = now;
		return false;
	}
	return now - audioReadyAt >= WARMUP_SEC;
}

/** The most threads checkThreadCount() will ever ask for, and the point
checkMaxedOutOverload() calls "nothing left to raise" -- one short of the
device's core count, so Android always has one no Worker will claim. A 1-core
device has nothing to spare and is left alone entirely. */
static int engineThreadCeiling() {
	int cores = system::getLogicalCoreCount();
	int ceiling = cores <= 1 ? cores : cores - RESERVED_CORES;
	// checkThreadCount() indexes scores[] by thread count and reads one rung
	// above the current one, so the ceiling has to stay inside that array. No
	// phone has anything like this many cores; a desktop-class tablet one day
	// might, and a buffer overrun is not how it should find out.
	if (ceiling > MAX_TRACKED_THREADS - 1)
		ceiling = MAX_TRACKED_THREADS - 1;
	return ceiling;
}
/** Holds the rack to rackdroid::MAX_RACK_ZOOM however it got past it -- the
View menu's zoom slider, or a patch saved on desktop at 4x and opened here.
The pinch path stops itself (touch_input.cpp) rather than being clamped, so
this only ever fires for those other routes and costs one comparison a frame.
See MAX_RACK_ZOOM's comment for why the ceiling exists at all. */
static void checkZoomCeiling() {
	if (!APP->scene || !APP->scene->rackScroll)
		return;
	float zoom = APP->scene->rackScroll->getZoom();
	if (zoom <= rackdroid::MAX_RACK_ZOOM)
		return;
	// Worth a line: the view will not be where the patch said, and the reason
	// is ours rather than anything wrong with the file.
	LOGW("View: patch asked for %.2fx zoom; holding at %.0fx (a phone reallocates "
		"every module's framebuffer on each zoom step, and past this it breaks "
		"the audio up)", zoom, rackdroid::MAX_RACK_ZOOM);
	APP->scene->rackScroll->setZoom(rackdroid::MAX_RACK_ZOOM);
}

/** Owns settings::threadCount outright. Nothing else writes it, and the user
is not asked: the Threads menu is filtered out on Android (hiddenOnAndroid in
menu_native.cpp), because the right number is not a preference anyone can be
expected to hold an opinion about -- it depends on the audio path the device
granted, the patch, and how hot the phone is right now.

This replaces three functions that each wrote the same number from a different
motive -- escalate on underrun, revert when idle, search downward when raising
stopped helping -- and spent twelve flags and two timers trying to tell each
other's writes apart from the user's. Every one of today's thread-count bugs
lived in that seam: a pinch-zoom escalating to the ceiling and costing half a
minute of recovery, a search whose answer the escalator undid on the next
frame, a device stranded at one thread with nothing able to raise it again.
One owner, one rule, no seam.

The rule: measure, then move toward what measured better.

The FIRST guess comes from the sharing mode, and on most devices it is also
the last. An Exclusive stream wants every core it can have -- measured on an
S22, 224 modules: 1 thread 1522 underruns, 8 threads 22. A Shared stream is
the opposite, because Engine_stepFrame() synchronises every worker at two spin
barriers per SAMPLE: that cost scales with the thread count and not with the
patch, so on a path already denied the low-latency route the spinning starves
the very callback it feeds. Measured on an 8T, at fifteen modules AND at
seventy-six: 1-2 threads clean, 4 and up underrunning about nine times a
second. Starting there means no search happens at all in the common case.

After that it is hill-climbing on measured underruns: try an unmeasured
neighbour, keep the best, and stay put once a window comes back clean. It can
move in both directions, which the descending-only search it replaces could
not -- that one stranded a device at a single thread where a heavy patch
needed more, with nothing left that could raise it.

Windows the user's fingers were in are thrown away rather than scored. A
pinch-zoom or a drag makes the render thread crowd the audio callback for
exactly as long as the gesture lasts (436 underruns, in the report that
prompted this), and none of that says anything about the patch. */

/** Set by checkThreadCount() when it is underrunning and has nothing left to
try: every neighbouring count has been measured and none is clearly better.
Cleared the moment it moves again. Only then is "this patch is too heavy" an
honest thing to tell someone -- while the tuner is still walking down from its
opening guess it is underrunning on purpose, and the answer is usually three
rungs away. */
static bool g_threadTunerExhausted = false;

/** Set by checkThreadCount() while every thread count it has measured leaves
the engine needing more time than the audio lasts, and it has parked on the
least bad of them. That is a different state from "underrunning": no count and
no buffer fixes it, so the block-size ladder stands down and the user is told.

Before this the tuner compared rungs by underruns, which are all alike once
none can keep up, so it never found a best one. It walked the whole ladder each
time the scores went stale -- a minute at two threads, a minute at seven -- and
the block size was doubled to 1024 on the way for nothing. Seven Workers at
audio priority leave the interface no CPU on a small device: on a TB-X306X a
133-module patch took the File menu up to eight seconds to open. */
static bool g_engineOverloaded = false;

/** The share of its deadline the callback used in the last window that ran
clean, or 0 before there has been one. What the block-size steps consult
before giving headroom back: a patch clean at 90% is clean because of the
headroom it has. */
static int32_t g_lastCleanLoad = 0;

/** The load the engine is parked at while g_engineOverloaded, and a request
nothing else. A bigger block has been seen to help an overloaded engine once
-- an SM-S901E from 116% of its deadline at 128 frames to a clean 85% at 256,
though on a phone whose throttling was moving at the time -- so the ladder is
allowed its steps while the shortfall is of that order, and none once it is
several times over (128 -> 1024 changed nothing at 150-240%). */
static int32_t g_overloadPercent = 0;

/** When the thread tuner last finished measuring a patch in silence. The
block ladder gives the count it chose fifteen seconds before judging it: it
used to step up 1.2 s after the audio was let out, on underruns from the
silence itself, and bought four more seconds of silence for the re-measuring. */
static double g_sweptAt = -1e9;

/** What each block size has cost this patch: the share of the deadline at the
thread count the tuner ended on, clean or not, and when that was learned. The
ladder used to know only "up". It is not that simple: on an SM-S901E one patch
measured 116% at 128 frames, a clean 81% at 256, 119% at 512 and 115% at 1024
-- and the ladder, answering a handful of underruns at 256, climbed past the
one size that worked and sat at 1024 underrunning forty times a second with
nothing to bring it back. Indexed by log2 of the size. */
static int32_t g_blockLoad[16];
static double g_blockLoadAt[16];
static const double BLOCK_LOAD_TTL_SEC = 600.0;

static int blockSlot(int block) {
	int slot = 0;
	while (block > 1 && slot < 15) {
		block >>= 1;
		slot++;
	}
	return slot;
}

static void noteBlockLoad(int32_t percent) {
	int block = rackdroid::audioBlockSize();
	if (block <= 0 || percent <= 0)
		return;
	// The best it has done at this size while the verdict is fresh. The latest
	// instead made the comparison depend on which thread count the tuner
	// happened to be passing through: 128 frames read 86% at four threads and
	// 98% at six on an SM-S901E, and the block went 128, 256, 128, 256, 512,
	// 256 in three minutes on the strength of it.
	int slot = blockSlot(block);
	double now = system::getTime();
	bool fresh = g_blockLoad[slot] > 0 && now - g_blockLoadAt[slot] < BLOCK_LOAD_TTL_SEC;
	if (!fresh || percent < g_blockLoad[slot])
		g_blockLoad[slot] = percent;
	g_blockLoadAt[slot] = now;
}

/** The load `block` is known to cost, or 0 if nobody has measured it lately. */
static int32_t knownBlockLoad(int block) {
	int slot = blockSlot(block);
	if (g_blockLoad[slot] <= 0 || system::getTime() - g_blockLoadAt[slot] > BLOCK_LOAD_TTL_SEC)
		return 0;
	return g_blockLoad[slot];
}

/** Set the first time a thread count runs a window clean, and never cleared.
Not a claim that the engine is settled NOW -- it is a claim that the opening
search is over, which is the only thing anyone else needs to wait for.

checkBlockSizeStepDown() below is the reader, and it learned this the hard way.
It fires once per launch, roughly ten seconds in, and reopens the audio stream
to try a smaller block. That used to land after the thread search had finished;
now the search takes a couple of seconds, and on a freshly installed A024 the
block probe landed in the middle of it instead. Two tuners moving at once, each
charging the other's stream reopens to its own candidate: the thread count went
2 -> 3 -> 4 -> 5 -> 6 -> 7 -> 3 -> 2 in nine seconds, learned nothing true
about any of them, and the block probe concluded 256 frames "does not hold"
on evidence that was entirely the thread tuner's. One tuner moves at a time. */
static bool g_threadTunerSettled = false;

/* Where the tuner settled last time. Without it every launch re-walks the
same ladder from the core ceiling, and on a device that grants Exclusive that
is ten to fifteen seconds of audible crackle before each session starts --
paid again on every launch, to rediscover an answer that had not changed.
Only a count that ran a full window clean is written, and it is only ever an
opening guess: the tuner measures from there like anywhere else, so a patch
that has grown heavier since, or a phone that is warmer, moves off it. */
static std::string threadMemoPath() {
	return asset::user("engine-threads");
}

static int rememberedThreadCount(int ceiling) {
	FILE* f = std::fopen(threadMemoPath().c_str(), "r");
	if (!f)
		return 0;
	int v = 0;
	bool ok = std::fscanf(f, "%d", &v) == 1;
	std::fclose(f);
	if (!ok || v < 1 || v > ceiling)
		return 0; // absent, corrupt, or from a phone with different cores
	return v;
}

static void rememberThreadCount(int n) {
	static int lastWritten = 0;
	if (n == lastWritten)
		return; // settling is checked every window; the flash write is not
	FILE* f = std::fopen(threadMemoPath().c_str(), "w");
	if (!f)
		return;
	std::fprintf(f, "%d\n", n);
	std::fclose(f);
	lastWritten = n;
}

/** Keeps ADPF's deadline in step with the stream. One callback has to deliver
`block` frames, so the time it may take is block / sampleRate -- the same
number the audio device is already clocked by. Recomputed rather than set once
because both halves move: the tuner can change the block size, and the device
can open at a rate the engine did not ask for. */
static void checkAdpfTarget() {
	static int64_t lastNanos = 0;
	// The frames the callback is actually handed, not the engine's block size.
	// alignToBurst() rounds the request to a whole number of the device's
	// bursts, so on a 96-burst phone a 64-frame block arrives as a 96-frame
	// callback -- and deriving the deadline from 64 told ADPF 1.3 ms for work
	// that had 2.0, a third tighter than the truth. Harmless in the sense that
	// asking for more performance than needed does not break anything, but it
	// is a wrong number handed to a system that acts on it. Falls back to the
	// block size only before the first callback has run.
	int block = rackdroid::audioCallbackFrames();
	if (block <= 0)
		block = rackdroid::audioBlockSize();
	if (block <= 0 || !APP->engine)
		return;
	float rate = APP->engine->getSampleRate();
	if (rate <= 0.f)
		return;
	int64_t nanos = (int64_t) (block / (double) rate * 1e9);
	if (nanos == lastNanos)
		return;
	lastNanos = nanos;
	rackdroid::adpfSetTargetNanos(nanos);
}


static void checkThreadCount() {
	static const double WINDOW_SEC = 5.0;
	/** The window used while the search has not yet found a rung that runs
	clean. Long enough that the count has been asked to do real work through
	several dozen callbacks, short enough that walking a ladder is over before
	anyone has finished looking at the screen. */
	static const double SEARCH_WINDOW_SEC = 1.5;
	/** Nothing is judged on less than this, however bad the load looks -- the
	first callbacks after a thread count changes are the engine relaunching
	its workers, not the engine running. */
	static const double MIN_WINDOW_SEC = 0.4;
	/** Past this share of the callback's deadline ON AVERAGE the count is not
	slow, it is failing: frames are not being produced as fast as they are
	consumed, and no buffer depth fixes that -- it only postpones it. Slightly
	under 100 to catch it just before it becomes audible.

	Judged on the mean and never on the peak. A peak over the deadline is one
	callback preempted by the system, which is exactly what the buffer is there
	to absorb: at a 96-frame callback the deadline is 2 ms and an ordinary
	hiccup reads several hundred percent while the audio stays clean. Reading
	the peak here was a real regression -- once the block-size ladder reached
	64 frames this rejected configurations producing ZERO underruns ("0
	underruns in 1.2s ... it cannot keep up"), thrashed every rung from 2 to 7,
	and caused the first genuine underruns of the session by doing so. */
	// It was 90, "to catch it just before it becomes audible", and what it
	// caught was counts that were holding: a Nothing A024 ran a 177-module
	// patch clean at three threads and 91% for four minutes, and the first
	// window this rule got a look at sent it to two threads and 101%, where
	// it underran forty times a second until the app was closed. Near the
	// limit is the business of the gentler rules further down; this one is
	// for counts that are over it.
	static const int32_t LOAD_HOPELESS_PERCENT = 100;
	/** The reading is wall time around one callback, so a stream that is being
	torn down and rebuilt underneath it produces a number that describes the
	reopen and nothing else: 42705% of the deadline was logged against a rung
	that had taken exactly one underrun, half a second after closeStreams()
	spent 722 ms. Ignore the load until the stream has been up this long. */
	static const double STREAM_SETTLE_SEC = 1.5;
	static int32_t windowLoadPeak = 0;
	static int64_t windowLoadSum = 0;
	static int32_t windowLoadSamples = 0;
	static int scores[MAX_TRACKED_THREADS + 1];
	// When each score was taken. A measurement describes the conditions it was
	// made under, and those expire: scores collected while another app was
	// eating the phone become lies the moment it stops. Without this the engine
	// could park for good on a rung chosen under conditions that no longer
	// exist -- seen doing exactly that on an S22, stuck at seven threads and
	// thirty underruns a second for four minutes after an artificial load was
	// removed, because every alternative had been scored during it.
	static double scoreAt[MAX_TRACKED_THREADS + 1];
	// The mean share of the deadline each rung used when it was scored, or 0
	// where the window had too few readings to say. What rungs are compared by
	// once none of them keeps up.
	static int32_t loads[MAX_TRACKED_THREADS + 1];
	static bool provenClean[MAX_TRACKED_THREADS + 1];
	static bool initialised = false;
	static int settledAt = -1;
	static const double WARMUP_SEC = 5.0;
	static double windowStartedAt = 0.0;
	static double warmupUntil = 0.0;
	static int32_t windowStartCount = 0;
	static bool windowTouched = false;

	int ceiling = engineThreadCeiling();
	if (ceiling <= 1)
		return; // a single-core device has no choice to make

	// One thread is a count like any other, and where a patch fits on it the
	// best there is. It used to be ruled out -- "no measurement has ever made
	// it the best rung" -- when rungs were judged by trying them and counting
	// underruns. But one thread is the only count at which the callback waits
	// for nobody: every other one has it meet its Workers twice a sample, and
	// a Worker the system parks for five milliseconds stops the audio for
	// five milliseconds. That is what a volume key did on a Nothing A024.
	// Every other audio app on the platform computes in the callback alone
	// for this reason. So the fewest threads that leave the patch room.
	int floorCount = 1;

	if (rackdroid::audioBlockSize() <= 0)
		return; // no audio device open yet: nothing to measure with

	double now = system::getTime();
	int32_t total = rackdroid::audioUnderrunCount();

	if (!initialised) {
		for (int i = 0; i <= MAX_TRACKED_THREADS; i++) {
			scores[i] = -1;
			scoreAt[i] = 0.0;
		}
		bool shared = rackdroid::audioIsSharedMode();
		int want;
		const char* why;
		if (shared) {
			// A Shared stream opens low, always, whatever last time said. The
			// two mistakes are not equally priced here: too high costs seconds
			// of audible crackle -- an 8T opened at seven threads on a
			// remembered count and took 28 underruns in the first window --
			// while too low costs one quiet window before the ladder climbs.
			// And a memo carries no patch with it: the count it recorded ran
			// clean on whatever was loaded then, which on that device was a
			// fifteen-module patch, and says nothing about the next one.
			// So the memo gets no say at all on this path, rather than a say
			// that is then taken away by a cap -- same result, but the code
			// and the log then agree on what happened.
			want = floorCount;
			why = "the Shared path always opens low";
		}
		else {
			// Exclusive: no such cliff, and walking the whole ladder from the
			// ceiling costs ten to fifteen seconds of crackle at every launch.
			// The memo is worth having here, and a count written there ran a
			// full window clean, so it starts trusted rather than on probation.
			// With no memo, open low and climb -- not high and walk down. The
			// two mistakes are not priced the same. Opening too high is
			// audible: a Nothing A024 opened at its seven-thread ceiling on a
			// seven-module patch and produced 885 underruns over the 45
			// seconds it took to walk down to the 2 it actually wanted, and
			// every one of those was a click the owner heard. Opening too low
			// is inaudible -- a quiet window or two of using fewer cores than
			// the patch could have had, and the search climbs out of it the
			// moment anything underruns.
			//
			// It is also the likelier answer. Rack's engine synchronises every
			// worker at two spin barriers per SAMPLE, so on a phone extra
			// workers regularly cost more in barrier traffic than they return
			// in parallelism: measured on that A024, seven threads underran
			// continuously and two ran clean at 14 ms.
			int remembered = rememberedThreadCount(ceiling);
			want = remembered > 0 ? remembered : floorCount;
			why = remembered > 0 ? "where it settled last time"
				: "opening low and climbing if the patch needs it";
			if (want > ceiling)
				want = ceiling;
			if (want < floorCount)
				want = floorCount;
			if (want == remembered && want <= MAX_TRACKED_THREADS)
				provenClean[want] = true;
		}
		LOGI("Engine: starting at %d threads (%s, %s audio path, %d-thread ceiling)",
			want, why, shared ? "Shared" : "Exclusive", ceiling);
		settings::threadCount = want;
		initialised = true;
		windowStartedAt = now;
		warmupUntil = now + WARMUP_SEC;
		windowStartCount = total;
		windowTouched = false;
		return;
	}

	// A different patch is a different question, and everything the tuner
	// knows is about the last one: a count proven clean, scores, a settled
	// state that makes it wait five-second windows and forgive a few
	// underruns. Carried over, that had an SM-S901E play ten seconds of solid
	// underruns after a 133-module patch replaced a light one, patiently
	// re-deciding what it had settled for the other patch. Start the search
	// again, from where it is, with the short windows a search uses.
	static int portEpoch = 0;
	static int tunedBlock = 0;
	static bool resetLate = false;
	// The sweep: which count is being measured (0 = the one already in use,
	// before deciding whether to sweep at all), since when, and what so far.
	static bool sweepWanted = false;
	static int sweepCount = 0;
	static double sweepStepAt = 0.0;
	static int64_t sweepSum = 0;
	static int32_t sweepCallbacks = 0;
	static double sweptAt = -1e9;
	static bool sweepConfirming = false;
	// A step's reading is the worst of its 0.2 s slices, not its mean, and the
	// count finally chosen is measured a second time before it is believed.
	static int32_t sweepWorst = 0;
	static int sweepSlices = 0;
	static double sweepSliceAt = 0.0;
	static int sweepVerifyOf = 0;
	static int sweepVerifies = 0;
	// A different block size is a different question too: the same patch that
	// overran every count at 128 frames ran clean at 256.
	if (rackdroid::audioPortEpoch() != portEpoch || rackdroid::audioBlockSize() != tunedBlock) {
		bool portChanged = rackdroid::audioPortEpoch() != portEpoch;
		portEpoch = rackdroid::audioPortEpoch();
		tunedBlock = rackdroid::audioBlockSize();
		for (int i = 0; i <= MAX_TRACKED_THREADS; i++) {
			scores[i] = -1;
			scoreAt[i] = 0.0;
			loads[i] = 0;
			provenClean[i] = false;
		}
		settledAt = -1;
		resetLate = true;
		g_engineOverloaded = false;
		g_threadTunerExhausted = false; // or the ladder acts on the last patch's verdict
		g_lastCleanLoad = 0;
		if (portChanged) {
			// A different patch: what the block sizes cost the last one is gone.
			for (int i = 0; i < 16; i++)
				g_blockLoad[i] = 0;
		}
		// And for a new patch the count is chosen before it is heard: see the
		// sweep below. Not for a new block size alone: the counts keep their
		// order across block sizes closely enough, and measuring them all
		// again cost up to ten seconds of silence after every step of the
		// block ladder on an SM-S901E.
		if (portChanged) {
			sweepWanted = true;
			sweepCount = 0;
			sweepConfirming = false;
			sweepVerifyOf = 0;
			sweepVerifies = 0;
			rackdroid::audioWarmupHold(true);
		}
		windowStartedAt = now;
		windowStartCount = total;
		windowTouched = false;
		// Half a second for the load itself -- modules being created with the
		// engine held off -- and then the measuring starts. Not the five
		// seconds a first launch used to wait: the audio is silent until this
		// is over, and nobody should have to listen to that.
		warmupUntil = now + 0.5;
	}

	// Starting up is not a measurement. The patch is still loading and the
	// Oboe stream reopens several times while the Audio module settles, which
	// on an 8T cost fifteen underruns in the first five seconds and was enough
	// to walk the engine straight off a perfectly good count.
	if (now < warmupUntil) {
		windowStartedAt = now;
		windowStartCount = total;
		windowTouched = false;
		windowLoadPeak = 0;
		windowLoadSum = 0;
		windowLoadSamples = 0;
		rackdroid::audioEngineLoadTake(NULL, NULL); // discard; measured startup
		sweepStepAt = now;
		sweepSum = 0;
		sweepCallbacks = 0;
		sweepWorst = 0;
		sweepSlices = 0;
		sweepSliceAt = now;
		return;
	}

	// While another app has the audio focus, and just after: nothing is judged
	// and nothing is counted. See audioFocusDisturbed().
	if (rackdroid::audioFocusDisturbed() && !sweepWanted) {
		windowStartedAt = now;
		windowStartCount = total;
		windowTouched = false;
		windowLoadPeak = 0;
		windowLoadSum = 0;
		windowLoadSamples = 0;
		rackdroid::audioEngineLoadTake(NULL, NULL);
		return;
	}

	// Choosing the count for a new patch used to mean playing it at each one
	// in turn and listening for the underruns: on an SM-S901E a 133-module
	// patch walked 2, 3, 4, 5, 6, 7 and back to 4 over ten seconds, six of
	// them audible and every one a thousand underruns. None of that needs to
	// be heard. How much of its deadline the engine uses at a count is known
	// half a second after switching to it, the audio is being held silent
	// for the load anyway, and so the walk is done there: measure the count
	// in use, and unless it is comfortably inside its deadline measure the
	// others too, then start the patch on the best of them.
	if (sweepWanted) {
		static const double SWEEP_SETTLE_SEC = 0.2; // Workers relaunching
		// Three slices of a fifth of a second, and the worst of them is the
		// reading. A patch does not cost the same from one moment to the next
		// -- a sequencer steps, envelopes open -- and one 0.3 s mean read 68%
		// for a 177-module patch on one thread of a Nothing A024 that then
		// ran at 109%. The engine has to fit the patch's busy moments.
		static const double SWEEP_SLICE_SEC = 0.2;
		static const double SWEEP_MEASURE_SEC = 0.6;
		// Under this, twice running, the count in use is left alone and the
		// others are not measured. It was 70 on one reading, and one reading
		// taken half a second after a 177-module patch loaded on a Nothing A024
		// came in under it: the patch started on the five threads remembered
		// from the day before, at 105-126% of its deadline, and crackled for
		// twenty seconds until something else made the tuner measure -- when
		// it found three threads at 64%.
		static const int32_t SWEEP_ENOUGH_PERCENT = 60;
		int32_t meanNow = 0, callbacksNow = 0;
		// The mean with stalls clipped: see g_loadSumClipped.
		rackdroid::audioEngineLoadTake(NULL, NULL, &callbacksNow, &meanNow);
		if (now - sweepStepAt < SWEEP_SETTLE_SEC)
			sweepSliceAt = now;
		else if (callbacksNow > 0) {
			sweepSum += (int64_t) meanNow * callbacksNow;
			sweepCallbacks += callbacksNow;
		}
		if (now - sweepSliceAt >= SWEEP_SLICE_SEC && sweepCallbacks >= 4) {
			int32_t slice = (int32_t) (sweepSum / sweepCallbacks);
			if (slice > sweepWorst)
				sweepWorst = slice;
			sweepSlices++;
			sweepSum = 0;
			sweepCallbacks = 0;
			sweepSliceAt = now;
		}
		// A step that never gets its callbacks (no stream, a stalled one) must
		// not hold the patch silent: two seconds and the sweep is abandoned.
		bool stuck = now - sweepStepAt > 2.0;
		if (!stuck && (now - sweepStepAt < SWEEP_SETTLE_SEC + SWEEP_MEASURE_SEC
				|| sweepSlices < 2))
			return;
		int measured = settings::threadCount;
		int32_t load = sweepWorst;
		if (!stuck && measured >= 1 && measured <= MAX_TRACKED_THREADS) {
			// A second look at a count keeps the worse of the two.
			if (!(sweepVerifyOf == measured && loads[measured] > load))
				loads[measured] = load;
			scoreAt[measured] = now;
		}
		// The next count nobody has measured yet, nearest first.
		int next = -1;
		bool light = load > 0 && load < SWEEP_ENOUGH_PERCENT;
		// A count is comfortable under this share of its deadline, and the
		// fewest comfortable threads are what is wanted -- not the lowest load,
		// which more Workers can always buy a few points of.
		// Eighty, not sixty: this is measured with nobody touching the screen,
		// and what a count costs once they do grows with the Workers the callback
		// has to wait for. A Nothing A024 read 1:72% 2:94% 3:65% 4:61% 5:51%,
		// took five as the only one under sixty, and a pinch then held callbacks
		// of 2 ms of work for 5 to 15 ms -- 400 underruns in a window -- where
		// the build before, on two threads at 84%, had got through the same
		// zooming with four short bursts. Kept under the 85 that counts as close
		// to the limit below, or the choice would be argued with at once.
		static const int32_t COMFORT_PERCENT = 80;
		int comfy = 0;
		for (int c = floorCount; c <= ceiling && c <= MAX_TRACKED_THREADS && comfy == 0; c++)
			if (loads[c] > 0 && loads[c] < COMFORT_PERCENT)
				comfy = c;
		// One point must not decide it: 4:81% 5:79% on an SM-S901E took five.
		for (int c = floorCount; c < comfy; c++) {
			if (loads[c] > 0 && loads[c] <= loads[comfy] + 5) {
				comfy = c;
				break;
			}
		}
		bool sweepOn = !stuck && (sweepCount > 0 || !light);
		if (!stuck && comfy > 0) {
			// Something fits: then the question is how few threads will do,
			// asked from the bottom -- one first, which is where a light patch
			// belongs and is then settled in two readings instead of a walk
			// down through every count above it (six seconds of silence for
			// a 45-module patch coming after a heavy one, on an SM-S901E).
			sweepOn = false;
			for (int c = floorCount; c < comfy && next < 0; c++)
				if (loads[c] == 0)
					next = c;
		}
		if (sweepOn) {
			// Outwards from the lightest count found so far, and not past a
			// count that is already much worse than it: the curve has one
			// bottom, and seven Workers on a tablet that is happiest with
			// four measured 872% of the deadline for the half second it took
			// to find that out.
			int low = 0;
			for (int c = floorCount; c <= ceiling && c <= MAX_TRACKED_THREADS; c++)
				if (loads[c] > 0 && (low == 0 || loads[c] < loads[low]))
					low = c;
			for (int dir : {-1, 1}) {
				for (int c = low + dir; c >= floorCount && c <= ceiling
						&& c <= MAX_TRACKED_THREADS; c += dir) {
					if (loads[c] == 0) {
						if (next < 0)
							next = c;
						break;
					}
					if (loads[c] > loads[low] + 25)
						break; // far side of the bottom: stop looking this way
				}
				if (next > 0)
					break;
			}
		}
		sweepStepAt = now;
		sweepSum = 0;
		sweepCallbacks = 0;
		sweepWorst = 0;
		sweepSlices = 0;
		sweepSliceAt = now;
		if (next > 0) {
			sweepCount++;
			settings::threadCount = next;
			return;
		}
		// Done. The fewest threads the patch is comfortable on; where it is
		// comfortable on none, the lightest count, and of two within a few
		// points the smaller.
		int best = measured;
		int32_t bestLoad = load > 0 ? load : 1000;
		if (comfy > 0) {
			best = comfy;
			bestLoad = loads[comfy];
		}
		else {
			best = 0;
			bestLoad = 1000000;
			for (int c = floorCount; c <= ceiling && c <= MAX_TRACKED_THREADS; c++) {
				if (loads[c] > 0 && loads[c] < bestLoad) {
					best = c;
					bestLoad = loads[c];
				}
			}
			// And of two within a few points, the smaller, as above.
			for (int c = floorCount; c < best; c++) {
				if (loads[c] > 0 && loads[c] <= bestLoad + 5) {
					best = c;
					bestLoad = loads[c];
					break;
				}
			}
			if (best == 0) {
				best = measured;
				bestLoad = load;
			}
		}
		// Believe the winner only once it has been measured twice: the first
		// reading may have caught the patch in a quiet moment. If the second
		// is worse it is kept, and the choice is made again.
		// A count well inside its deadline needs no second look: its reading is
		// already the worst of three slices and it has forty points to spare.
		if (!stuck && (comfy == 0 || loads[comfy] >= SWEEP_ENOUGH_PERCENT) && sweepCount > 0
				&& sweepVerifyOf != best && sweepVerifies < 3) {
			sweepVerifyOf = best;
			sweepVerifies++;
			settings::threadCount = best;
			return;
		}
		if (sweepCount > 0 || best != measured) {
			std::string seen;
			for (int c = floorCount; c <= ceiling && c <= MAX_TRACKED_THREADS; c++)
				if (loads[c] > 0)
					seen += string::f(" %d:%d%%", c, loads[c]);
			LOGI("Engine: measured this patch in silence at each thread count "
				"(%s ); starting it at %d", seen.c_str(), best);
			// Known, and known worse than the one chosen: the search below is
			// not to go trying them out loud.
			for (int c = floorCount; c <= ceiling && c <= MAX_TRACKED_THREADS; c++) {
				if (loads[c] > 0) {
					// Worse than any number of underruns the chosen count can
					// have: a small figure here was compared with an underrun
					// count and won, and the search went off to try six
					// threads out loud two seconds after the sweep had measured
					// them as no better.
					scores[c] = (c == best) ? -1 : 1000000;
					scoreAt[c] = now;
				}
			}
		}
		sweepWanted = false;
		sweepConfirming = false;
		sweepVerifyOf = 0;
		sweepVerifies = 0;
		// Only a real measurement of the ladder counts as one: this is what
		// stops another from being asked for inside a minute, and a patch
		// waved through on its first reading has had none.
		if (sweepCount > 0) {
			sweptAt = now;
			g_sweptAt = now;
		}
		settings::threadCount = best;
		rackdroid::audioWarmupHold(false);
		windowStartedAt = now;
		windowStartCount = total;
		windowTouched = true;
		windowLoadPeak = 0;
		windowLoadSum = 0;
		windowLoadSamples = 0;
		return;
	}

	// Anything that disturbs the audio while a window is open makes that
	// window worthless: it measures the disturbance, not the thread count.
	// A touch was the first case found; two more showed up on the S22 while
	// backgrounding, returning and rotating the phone in a loop -- every
	// surface change and every stream reopen costs a burst of underruns, and
	// the tuner charged them to whatever count happened to be current. It
	// wandered 2 -> 1 -> 2 -> 5 -> 3 -> 4 over twenty-five seconds before
	// settling back where it started.
	if (rackdroid::windowSecondsSinceInteraction() < INTERACTION_SETTLE_SEC)
		windowTouched = true;
	if (rackdroid::windowSecondsSinceSurfaceChange() < WINDOW_SEC)
		windowTouched = true;
	if (rackdroid::audioSecondsSinceStreamOpen() < WINDOW_SEC)
		windowTouched = true;
	// The fourth disturbance is the tuner itself. Changing the count makes
	// Engine::stepBlock relaunch its workers, and that costs underruns of its
	// own -- so the window right after a move carries the price of the move.
	// A settled engine never notices; a moving one measures nothing else, and
	// feeds on it: it moves, the move underruns, the underruns move it again.
	// Measured on an S22 with the ladder thrashing, the same rung scored 10 in
	// one window and 451 in the next. So each move marks the window that
	// follows it (the flag is set beside every write to threadCount below),
	// which costs one window of patience per rung and buys a number that means
	// something.

	// How full the callback's deadline got during this window. Polled every
	// frame and kept here rather than read once at the end, because reading it
	// resets the peak: skipping a poll would throw away the spike it saw.
	// Read every time regardless, so the peak never carries across windows --
	// but only believe it once the window has had a moment to settle. The
	// first callbacks of a window are the ones Engine::stepBlock spends
	// relaunching its workers after the move that opened it, and one such
	// callback reads well over its deadline: measured at 139% here, on a rung
	// that was about to run clean at 20%. A peak remembers a single spike
	// forever, so the spike has to be kept out rather than argued with.
	int32_t peakNow = 0, meanNow = 0, callbacksNow = 0;
	rackdroid::audioEngineLoadTake(&peakNow, &meanNow, &callbacksNow);
	if (now - windowStartedAt >= MIN_WINDOW_SEC
			&& rackdroid::audioSecondsSinceStreamOpen() >= STREAM_SETTLE_SEC) {
		if (peakNow > windowLoadPeak)
			windowLoadPeak = peakNow;
		// Weighted by the callbacks each reading covers, and counted in
		// callbacks. Counting readings instead meant counting frames: a
		// TB-X306X drawing a heavy patch at a few frames a second never
		// reached four of them in a window, so its load was "unknown" in
		// exactly the situation the load exists to recognise, and the block
		// ladder climbed to 1024 on underruns alone.
		if (meanNow > 0 && callbacksNow > 0) {
			windowLoadSum += (int64_t) meanNow * callbacksNow;
			windowLoadSamples += callbacksNow;
		}
	}
	int32_t windowLoadMean = (windowLoadSamples > 0)
		? (int32_t) (windowLoadSum / windowLoadSamples) : 0;

	// Five seconds is the length of a window that has to prove a count is
	// GOOD: underruns are rare events and a short quiet stretch proves
	// nothing. Ruling a count OUT is a much easier question, and while the
	// search is still looking for its first clean rung that is the only
	// question being asked -- so ask it in a fraction of the time. This is
	// what turns the A024's forty-five second walk into a few seconds.
	double window = (settledAt < 0) ? SEARCH_WINDOW_SEC : WINDOW_SEC;

	// And a count that cannot meet its deadline need not be waited out at all.
	// Over 100% means the callback is already spending more time producing
	// frames than the frames last: no quiet stretch is coming, whatever the
	// underrun counter has managed to notice so far. One glance at this is
	// worth a window of counting.
	// Only ever used to rule a count OUT, never to declare one good, and that
	// asymmetry is deliberate: the reading can come out too low (a stream that
	// has just reopened barely calls back at all, and a peak of nothing was
	// logged in exactly that state), and a too-low reading costs nothing --
	// the window simply runs its full length and is judged on underruns, which
	// is what every window did before this existed.
	// The mean has to rest on enough readings to be a mean at all. Each poll
	// contributes the average of the callbacks since the last one, so a window
	// in which the stream barely called back contributes one or two -- and the
	// mean of one sample IS the peak, which is the very thing this rule was
	// rewritten to stop trusting. A Nothing A024 logged "6 underruns in 0.9s
	// at 5 threads (9002% of the deadline, peak 9002%)": identical numbers, one
	// sample, and the tuner then thrashed 3 -> 4 -> 5 -> 6 -> 7 -> 3 -> 5 -> 7
	// in ten seconds on the strength of it. Below the bar, fall back to
	// counting underruns: slower, and right.
	static const int32_t LOAD_MIN_SAMPLES = 8; // callbacks
	bool hopeless = settledAt < 0 && windowLoadMean > LOAD_HOPELESS_PERCENT
		&& windowLoadSamples >= LOAD_MIN_SAMPLES
		&& now - windowStartedAt >= MIN_WINDOW_SEC;
	if (!hopeless && now - windowStartedAt < window)
		return;

	double windowLen = now - windowStartedAt;
	int32_t loadPeak = windowLoadPeak;
	int32_t loadMean = windowLoadMean;
	int32_t loadSamples = windowLoadSamples;
	windowLoadPeak = 0;
	windowLoadSum = 0;
	windowLoadSamples = 0;
	int32_t rawUnderruns = total - windowStartCount;
	// Scores from windows of different lengths are not comparable, and the
	// search below does compare them. Carry them all in the same unit: what
	// this window's rate would come to over a full-length one.
	int32_t underruns = (windowLen > 0.01)
		? (int32_t) (rawUnderruns * WINDOW_SEC / windowLen) : rawUnderruns;
	// A window closed early on load alone can be genuinely free of underruns
	// so far -- it was cut short precisely so nobody has to hear the ones that
	// were coming. Score it on what the deadline said instead, or the rung
	// records a clean sheet and the search comes straight back to it.
	if (hopeless && underruns == 0) {
		LOGW("Engine: %d threads averaged %d%% of the audio deadline (peak %d%%) "
			"-- it cannot keep up; not waiting for the clicks to prove it",
			settings::threadCount, loadMean, loadPeak);
		// Score it as plainly bad, not as the raw percentage. A blocked
		// callback's wall time is unbounded -- 35153% was measured here, with
		// eight busy loops fighting it for the cores -- and scores[] is
		// compared rung against rung, so letting one rung carry a five-digit
		// number while another carries a two-digit underrun count would make
		// the comparison meaningless. All that needs preserving is the order:
		// worse than any window that actually survived.
		static const int32_t HOPELESS_SCORE = 1000;
		underruns = HOPELESS_SCORE;
	}
	int current = settings::threadCount;
	bool touched = windowTouched;
	windowStartedAt = now;
	windowStartCount = total;
	windowTouched = false;
	// A disturbance excuses a handful of underruns, not a flood. Touching the
	// screen, rotating, reopening the stream -- each of those costs a few, and
	// that is what this guard was built for. It was written as an absolute,
	// though, so on a phone whose owner was working through the menus every
	// window carried a touch and every window was thrown away: a Nothing A024
	// went six windows and hundreds of underruns without the tuner taking a
	// single decision, crackling the whole time. A gesture cannot account for
	// twenty-seven underruns in five seconds, so past that the window counts
	// whatever else happened during it.
	// A load verdict is never excused, however disturbed the window was. A
	// touch, a rotation or the tuner's own last move all cost underruns; none
	// of them makes the engine take longer to produce a block of frames than
	// those frames last, for a second and a half without pause. And every
	// candidate's first window carries the move that created it, so without
	// this exemption the fast rejection would be thrown away exactly when it
	// is most useful.
	// Underruns in a window where no callback ran past its deadline are not
	// this thread count's doing, whatever their number. The xrun counter is
	// read in the callback and lags the stall that caused it: on a Nothing
	// A024 a one-second stall during a pinch at 2 threads was counted as
	// "137 underruns in 1.5s at 3 threads (10% of the deadline, peak 97%)",
	// and the search climbed on the strength of it to 4, 5, 6 and 7 -- where
	// the barrier traffic made the engine genuinely late, 1360% and 2099% of
	// the deadline, for clicks nobody would have heard at 3. The peak is only
	// collected once the window has settled, which is exactly what keeps the
	// stall that produced these underruns out of it; so trust it only when
	// there are enough readings to have seen the window at all.
	if (!hopeless && rawUnderruns > 0 && loadPeak < 100
			&& loadSamples >= LOAD_MIN_SAMPLES) {
		LOGI("Engine: %d underruns in %.1fs at %d threads, but no callback ran "
			"late in that window (%d%% of the deadline, peak %d%%); not the "
			"thread count, not counting it", rawUnderruns, windowLen, current,
			loadMean, loadPeak);
		return;
	}

	static const int32_t DISTURBANCE_EXCUSES = 10;
	// A disturbed window that came through with no underrun at all is still a
	// clean window -- cleaner, if anything. Throwing those away too meant that
	// while someone kept their hands on the screen the tuner never learned
	// that the count was holding: four minutes without a verdict on that A024,
	// so nothing was settled and nothing was remembered about the block size
	// it was running. Its load is not believed, though: drawing was competing.
	bool touchedClean = touched && underruns == 0 && !hopeless;
	if (touched && !hopeless && !touchedClean && underruns <= DISTURBANCE_EXCUSES) {
		// Say so: a discarded window looks exactly like a tuner doing nothing,
		// and telling those apart from a log file is otherwise guesswork.
		if (underruns > 0)
			LOGI("Engine: %d underruns in %.1fs at %d threads, but the window was "
				"disturbed; not counting it", rawUnderruns, windowLen, current);
		return; // measured the disturbance, not the patch
	}
	// Not when the verdict came from the deadline: the line above has already
	// said why this window counts, and "0 underruns -- too many to blame on
	// the disturbance" is a sentence that explains nothing to anybody.
	if (touched && !hopeless && !touchedClean)
		LOGW("Engine: %d underruns in %.1fs at %d threads -- too many to blame on "
			"the disturbance in that window; counting it", rawUnderruns, windowLen,
			current);

	// A few underruns in five seconds are as likely to be the phone as the
	// patch -- a notification, another app waking up, the governor moving a
	// core. Acting on them means leaving a rung that was doing its job, and
	// the rung it moves to can be far worse: measured here, a rung that had
	// just been declared clean was abandoned over two underruns and the walk
	// down reached 1 thread, which produced seventy-eight in one window.
	// So a rung that has proved itself gets the benefit of the doubt, and an
	// unproven one still gets a little. Neither gets it forever.
	static int toleratedRuns = 0;
	static const int TOLERATED_RUNS_MAX = 3;
	bool proven = current >= 1 && current <= MAX_TRACKED_THREADS && provenClean[current];
	int32_t tolerance = proven ? 4 : 1;
	if (underruns > 0 && underruns <= tolerance && toleratedRuns < TOLERATED_RUNS_MAX) {
		toleratedRuns++;
		LOGI("Engine: %d underruns in %.1fs at %d threads; ignoring (%d of %d, %s)",
			rawUnderruns, windowLen, current, toleratedRuns, TOLERATED_RUNS_MAX,
			proven ? "this count has run clean before" : "too few to act on");
		return; // and do not record it as this rung's score
	}

	// Once parked on an overload it takes dropping back under the line the
	// search itself calls hopeless to leave it: an 8T sitting at 97-128% of its
	// deadline crossed a single threshold every few seconds, and each crossing
	// set the ladder walking again.
	// Past the deadline on average is not keeping up, by definition. This was
	// 110, to leave the band below it to the older rule that compares counts
	// by underruns -- and in that band a TB-X306X at 102% re-walked all six
	// counts every minute, seven Workers included, for as long as the patch
	// stayed loaded. The bigger block that band was being kept for is tried
	// from the parked state now.
	static const int32_t OVERLOAD_PERCENT = 100;
	static const int32_t OVERLOAD_LEAVE_PERCENT = 90;
	bool overNow = loadSamples >= LOAD_MIN_SAMPLES
		&& loadMean > (g_engineOverloaded ? OVERLOAD_LEAVE_PERCENT : OVERLOAD_PERCENT);
	if (current >= 1 && current <= MAX_TRACKED_THREADS) {
		// The lowest reading a rung has given while its score is fresh, not the
		// latest. These windows are half a second long and whatever disturbs one
		// only ever makes it read higher: the same rung on an S22 read 116% and
		// then 177%, and comparing rungs on the latest sent the engine round
		// eight of them in forty seconds.
		// Only ever against another overloaded reading: a low number left
		// from the patch that was loaded before this one says nothing, and
		// kept as the minimum it sent all three test devices to two threads.
		bool fresh = overNow && scores[current] >= 0
			&& loads[current] > LOAD_HOPELESS_PERCENT
			&& now - scoreAt[current] < 300.0;
		if (loadSamples >= LOAD_MIN_SAMPLES)
			loads[current] = (fresh && loads[current] < loadMean) ? loads[current] : loadMean;
		else if (!fresh)
			loads[current] = 0;
		scores[current] = underruns;
		scoreAt[current] = now;
	}
	if (!overNow)
		g_engineOverloaded = false;

	// A count that had proved itself and then went over its deadline with the
	// patch unchanged was pushed there by something else -- another process, a
	// hot phone -- and the counts measured again in the middle of that are
	// measured under it. A TB-X306X five minutes clean on two threads at 69%
	// read 138% while seventy shell commands were being started beside it, was
	// measured there and then ( 1:132% 2:138% 3:118% 4:107% 5:104% ), and sat
	// on five threads at 95%, underrunning, long after they had gone. So the
	// count it came from is remembered and gone back to, half a minute later
	// and, if whatever it was is still there and sends it over again, after two
	// minutes and after eight. A try that fails costs a second or two: the
	// count just left is by then known to fit better and is gone back to
	// without another measurement.
	static int priorCount = -1;
	static double priorTryAt = 0.0;
	static double priorWait = 30.0;
	if (resetLate)
		priorCount = -1;
	if (priorCount > 0 && now >= priorTryAt) {
		if (priorCount == current) {
			if (!overNow)
				priorCount = -1; // back, and holding
		}
		else if (priorWait > 500.0 || priorCount > ceiling)
			priorCount = -1;
		else {
			LOGI("Engine: back to the %d threads this patch was running clean on "
				"before it was pushed over its deadline (%d now, at %d%%)",
				priorCount, current, loadMean);
			priorWait *= 4.0;
			priorTryAt = now + priorWait;
			settings::threadCount = priorCount;
			windowTouched = true;
			settledAt = priorCount; // settled, not searching: judged on a full window
			return;
		}
	}

	// A score is only evidence while the conditions that produced it still
	// hold. Past this, treat the rung as never measured and let the search go
	// and look again -- which is what breaks the deadlock described above.
	// Parked on an overload, the ladder is only walked again every five
	// minutes: each walk passes through the ceiling, and nothing but the phone
	// cooling down can have changed the answer.
	// Five minutes, not one. What a stale score does is send the search to
	// "an unmeasured neighbour" at the next underrun, out loud; the counts are
	// measured in silence when the patch loads now, and that knowledge should
	// not be thrown away every minute to be relearned by ear.
	static const double SCORE_TTL_SEC = 300.0;
	static const double OVERLOAD_TTL_SEC = 300.0;
	double scoreTtl = g_engineOverloaded ? OVERLOAD_TTL_SEC : SCORE_TTL_SEC;
	auto known = [&](int i) {
		return i >= 1 && i <= MAX_TRACKED_THREADS && scores[i] >= 0
			&& now - scoreAt[i] < scoreTtl;
	};

	// Clean at a count that is more than it needs is not a happy ending. The
	// ladder only ever moves when it underruns, so once something transient --
	// a heavy moment, another app, a thermal dip -- has pushed the count up,
	// nothing brings it back down again. Measured here: an S22 driven to seven
	// threads by an artificial load stayed at seven when the load went away,
	// burning 712% of 800% on a patch that had run clean at four. That is
	// battery and heat spent on nothing, which is the very thing this whole
	// mechanism exists to avoid.
	//
	// So when a count has held clean for a while, spend one window asking
	// whether a smaller one would do. Only downward, only towards a rung that
	// is unmeasured or was clean itself, and with the interval doubling after
	// a probe that fails, so a device that genuinely needs its cores is not
	// poked at forever.
	static double probeAfter = 60.0;
	static const double PROBE_AFTER_MIN = 60.0;
	static const double PROBE_AFTER_MAX = 600.0;
	static double cleanSince = 0.0;
	static int probedFrom = -1;

	auto measureAgain = [&](const char* why) {
		LOGW("Engine: %d underruns in %.1fs at %d threads (%d%% of the deadline, "
			"peak %d%%), %s; measuring the thread counts again, in silence",
			rawUnderruns, windowLen, current, loadMean, loadPeak, why);
		for (int i = 0; i <= MAX_TRACKED_THREADS; i++) {
			scores[i] = -1;
			loads[i] = 0;
		}
		g_engineOverloaded = false;
		g_threadTunerExhausted = false;
		sweepWanted = true;
		sweepCount = 1; // the whole ladder, whatever the first reading says
		sweepConfirming = false;
		sweepVerifyOf = 0;
		sweepVerifies = 0;
		sweepStepAt = now;
		sweepSum = 0;
		sweepCallbacks = 0;
		sweepWorst = 0;
		sweepSlices = 0;
		sweepSliceAt = now;
		settledAt = -1;
		rackdroid::audioWarmupBegin();
	};
	int measuredCounts = 0;
	for (int i = floorCount; i <= ceiling && i <= MAX_TRACKED_THREADS; i++)
		if (loads[i] > 0 && now - scoreAt[i] < 600.0)
			measuredCounts++;
	// Never into a recording: the silence would be on the tape.
	bool mayMeasure = now - sweptAt >= 60.0 && !rackdroid::audioIsRecording();
	if (resetLate) {
		// The rest of what a new patch forgets; these live further down.
		resetLate = false;
		probedFrom = -1;
		probeAfter = PROBE_AFTER_MIN;
		cleanSince = 0.0;
	}

	if (underruns == 0) {
		toleratedRuns = 0;
		if (current >= 1 && current <= MAX_TRACKED_THREADS)
			provenClean[current] = true;
		g_threadTunerSettled = true;
		if (settledAt != current) {
			LOGI("Engine: %d threads is running clean (%d%% of the audio deadline "
				"on average, peak %d%%)", current, loadMean, loadPeak);
			settledAt = current;
			cleanSince = now;
		}
		rememberThreadCount(current);

		// Everything above this point waits for an underrun before it moves,
		// and an underrun is a click somebody has already heard. The share of
		// its deadline the callback is using says the same thing earlier: a
		// count running clean at 87% is one notification away from not being.
		// So a window that is clean but close counts against the count it was
		// measured at, and two in a row are acted on.
		static const int32_t NEAR_LIMIT_PERCENT = 85;
		static int nearLimitWindows = 0;
		static int raisedFrom = -1;
		static int32_t raisedFromLoad = 0;
		static double noRaiseUntil = 0.0;
		bool loadKnown = loadSamples >= LOAD_MIN_SAMPLES && !touched;
		if (loadKnown) {
			nearLimitWindows = (loadMean >= NEAR_LIMIT_PERCENT) ? nearLimitWindows + 1 : 0;
			g_lastCleanLoad = loadMean;
			noteBlockLoad(loadMean);
		}
		if (probedFrom >= 0 && loadKnown && loadMean >= NEAR_LIMIT_PERCENT - 25) {
			// Fewer threads did not underrun in this window, and would have in
			// the next busy one. Not good enough to stay.
			LOGI("Engine: %d threads ran clean but at %d%% of the audio deadline; "
				"%d it is, then", current, loadMean, probedFrom);
			settings::threadCount = probedFrom;
			windowTouched = true;
			// Settled there, not searching: a search rules a count out at 90%
			// of the deadline without waiting for an underrun, which is right
			// while looking for somewhere to stand and wrong for a count that
			// has been standing at 91% for minutes. Going back "to search" sent
			// an SM-S901E from a clean four threads down to two and 200
			// underruns.
			settledAt = probedFrom;
			probedFrom = -1;
			probeAfter = (probeAfter * 2.0 > PROBE_AFTER_MAX) ? PROBE_AFTER_MAX : probeAfter * 2.0;
			cleanSince = now;
			nearLimitWindows = 0;
			return;
		}
		if (raisedFrom >= 0) {
			// The first clean window after going up ahead of trouble: it has to
			// have bought something, or the extra Worker is heat for nothing.
			if (!loadKnown)
				return; // the move itself marks its first window; wait for a real one
			if (loadMean + 5 > raisedFromLoad) {
				LOGI("Engine: %d threads is no easier than %d was (%d%% against %d%%); "
					"going back", current, raisedFrom, loadMean, raisedFromLoad);
				settings::threadCount = raisedFrom;
				windowTouched = true;
				noRaiseUntil = now + 600.0;
				settledAt = raisedFrom; // settled, not searching: see above
				raisedFrom = -1;
				cleanSince = now;
				nearLimitWindows = 0;
				return;
			}
			raisedFrom = -1;
		}
		if (nearLimitWindows >= 2) {
			nearLimitWindows = 0;
			// Only to a count that is known to be easier, from the silent
			// measurement when the patch loaded. Going up to find out was
			// tried: 4, 5, 6 and then 7 threads on an SM-S901E, the last of
			// them 2186 underruns in five seconds. Finding out is a crackle.
			int upper = current + 1;
			bool upperKnownEasier = upper <= MAX_TRACKED_THREADS && loads[upper] > 0
				&& now - scoreAt[upper] < 600.0 && loads[upper] + 5 < loadMean;
			if (upper <= ceiling && upperKnownEasier && now >= noRaiseUntil) {
				LOGW("Engine: clean at %d threads but at %d%% of the audio deadline; "
					"trying %d before it is heard", current, loadMean, upper);
				raisedFrom = current;
				raisedFromLoad = loadMean;
				settledAt = upper; // judged on a full window, like any settled count
				cleanSince = 0.0;
				settings::threadCount = upper;
				windowTouched = true;
				return;
			}
			// Close to the limit on a count that was never compared with the
			// others -- a patch waved through at launch, when the phone was
			// cool and boosted, and now running at 87% on one thread on a
			// OnePlus 8T. Compare them now, in silence, rather than wait for
			// the underruns to ask.
			if (measuredCounts < 2 && mayMeasure) {
				measureAgain("clean but close to the limit, and the other counts unmeasured");
				return;
			}
			// No count known to be easier. A bigger block was tried here as the
			// next resort, ahead of any underrun, and on the one phone it could
			// be measured on it bought nothing three times out of three (85%
			// to 84, 83, 84) at four seconds of silence a try. What is left is
			// to say that the patch is close, before the crackle says it.
			static double warnedAt = -1e9;
			if (now - warnedAt >= 600.0) {
				warnedAt = now;
				LOGW("Engine: clean but at %d%% of the audio deadline at %d threads, "
					"with no better count known; telling the user it is close",
					loadMean, current);
				rackdroid::showEngineNotice(3);
			}
		}

		if (probedFrom >= 0) {
			// The probe held: the smaller count is doing the job.
			LOGI("Engine: %d threads is enough after all; staying here instead "
				"of %d", current, probedFrom);
			probedFrom = -1;
			probeAfter = PROBE_AFTER_MIN;
		}
		int lower = current - 1;
		// Same staleness rule as the search: a rung is worth probing if it was
		// never measured, measured clean, or measured so long ago that the
		// conditions have moved on. Reading scores[] raw here instead cost a
		// real bug -- a rung scored badly while another app was hogging the
		// phone stayed "known bad" for the rest of the session, and the engine
		// sat a rung higher than it needed to, for ever.
		bool worthProbing = !known(lower) || scores[lower] == 0;
		// And only where one thread fewer would plausibly still fit. The same
		// work on one thread less costs about current/lower as much of the
		// deadline; if that lands near the limit the probe is not a question,
		// it is a crackle with a foregone answer -- four threads at 90% were
		// probed down to three at 94% and two at 110% on an SM-S901E.
		// Where the count below was measured in silence when the patch loaded,
		// that measurement decides, and no estimate is needed.
		bool lowerMeasured = lower >= 1 && lower <= MAX_TRACKED_THREADS
			&& loads[lower] > 0 && now - scoreAt[lower] < 600.0;
		bool roomToProbe = loadKnown && lower >= 1
			&& (lowerMeasured ? loads[lower] < NEAR_LIMIT_PERCENT - 25
				: loadMean * current / lower < NEAR_LIMIT_PERCENT - 30);
		if (cleanSince > 0.0 && now - cleanSince >= probeAfter
				&& lower >= floorCount && worthProbing && roomToProbe) {
			LOGI("Engine: clean at %d threads for %.0fs; trying %d to see if "
				"fewer will do", current, now - cleanSince, lower);
			probedFrom = current;
			cleanSince = 0.0;
			settledAt = -1;
			settings::threadCount = lower;
			windowTouched = true;
		}
		return;
	}
	settledAt = -1;
	cleanSince = 0.0;
	if (probedFrom >= 0) {
		// The probe cost us a window. Go straight back rather than letting the
		// ladder wander, and wait longer before asking again.
		LOGW("Engine: %d underruns in %.1fs at %d threads; %d it is, then",
			rawUnderruns, windowLen, current, probedFrom);
		if (current >= 1 && current <= MAX_TRACKED_THREADS) {
			scores[current] = underruns;
			scoreAt[current] = now;
		}
		settings::threadCount = probedFrom;
		windowTouched = true;
		probedFrom = -1;
		probeAfter = (probeAfter * 2.0 > PROBE_AFTER_MAX) ? PROBE_AFTER_MAX : probeAfter * 2.0;
		return;
	}
	toleratedRuns = 0;

	// No count is tried out loud any more. This used to be where the search
	// went to "an unmeasured neighbour" and listened: after one volume key on a
	// Nothing A024 it walked 3, 2, 4, 5, 6, 7, 4, 5 over ten seconds of a patch
	// that had been playing clean on four. When the counts have to be looked
	// at again they are looked at the way a new patch is -- in silence, by
	// load -- and not more than once a minute.
	int candidate = -1;
	if (!overNow) {
		// Underrunning, but inside its deadline on average: jitter, which the
		// buffer is for, or something outside this app. Nothing is done about
		// it here. For one build two such windows in a row brought on a silent
		// re-measurement, and a drag across the rack on an SM-S901E was enough
		// to earn six seconds of silence in the middle of a patch that was
		// playing. A few clicks are a smaller thing than that.
	}
	else if (measuredCounts < 2 && mayMeasure) {
		// Over its deadline at a count chosen without measuring the others
		// (a patch that started comfortably never had them measured).
		measureAgain("and the other counts unmeasured");
		return;
	}
	static int overWindows = 0;
	if (overNow) {
		// Over its deadline. One such window is not a verdict -- a stall
		// somewhere in the system puts every count over for half a second,
		// and acting on it sent a Nothing A024 through 3, 4, 6 and 2 threads
		// in four seconds, each of them heard. Two in a row are. And then the
		// answer is not to hop to whichever count last read lower, out loud:
		// it is to measure them again in silence where that has not just been
		// done, and otherwise to stay on what the measurement chose and say
		// that the patch is too much.
		overWindows++;
		// Past twice the deadline there is no audio left to protect -- nothing
		// coming out is recognisable at any count -- and what the engine can
		// still do for the user is get out of the way. Parked at five Workers
		// on a 353-module patch (1500% of the deadline), a TB-X306X drew its own
		// status bar once every ten seconds and took eight seconds to act on a
		// tap in File > Open. So there the fewest threads win, whatever they
		// measure.
		static const int32_t NOTHING_LEFT_PERCENT = 200;
		int32_t lightest = loadMean;
		for (int i = floorCount; i <= ceiling && i <= MAX_TRACKED_THREADS; i++)
			if (loads[i] > 0 && now - scoreAt[i] < 600.0 && loads[i] < lightest)
				lightest = loads[i];
		bool nothingLeft = overWindows >= 2 && lightest > NOTHING_LEFT_PERCENT;
		// A count the silent measurement found inside the deadline, where this
		// one has turned out not to be: that is known, not a guess, so it is
		// gone to. The fewest such, and clearly better than here.
		int fits = -1;
		for (int i = floorCount; overWindows >= 2 && fits < 0 && i <= ceiling
				&& i <= MAX_TRACKED_THREADS; i++)
			if (i != current && loads[i] > 0 && now - scoreAt[i] < 600.0
					&& loads[i] < OVERLOAD_PERCENT && loads[i] + 10 < loadMean)
				fits = i;
		// The measurement stops at the first count that fits, so it may have
		// seen two of them; before calling the patch too heavy, see the rest.
		int countsThere = ceiling - floorCount + 1;
		if (nothingLeft && current != floorCount)
			candidate = floorCount;
		else if (fits > 0) {
			candidate = fits;
			overWindows = 0;
		}
		else if (overWindows >= 2 && mayMeasure
				&& (measuredCounts < (countsThere < 3 ? countsThere : 3)
					|| now - sweptAt > 300.0)) {
			overWindows = 0;
			if (proven) {
				priorCount = current;
				priorWait = 30.0;
				priorTryAt = now + 40.0; // the measurement itself takes a few seconds
			}
			measureAgain("and over its deadline");
			return;
		}
		else if (overWindows >= 2 && !g_engineOverloaded) {
			g_overloadPercent = loadMean;
			noteBlockLoad(loadMean);
			LOGW("Engine: no thread count keeps up with this patch; staying at %d "
				"(%d%% of the audio deadline), %s", current, loadMean,
				nothingLeft ? "the fewest threads, to leave the device usable"
					: "the best that was measured");
			g_engineOverloaded = true;
		}
	}
	else
		overWindows = 0;
	if (candidate < 0) {
		g_threadTunerExhausted = true;
		return; // nothing known to be better; stay where we are
	}

	LOGW("Engine: %d underruns in %.1fs at %d threads (%d%% of the deadline, "
		"peak %d%%); trying %d", rawUnderruns, windowLen, current, loadMean,
		loadPeak, candidate);
	g_threadTunerExhausted = false;
	settings::threadCount = candidate;
	windowTouched = true; // see below
	// Setting it is all that is needed: Engine::stepBlock relaunches its
	// workers from settings::threadCount on every block (Engine.cpp:572).
}

/** Set once checkBlockSizeOverload() below has fully spent its lever --
reached the cap for the live stream, whether or not it actually changed
anything. Read by checkMaxedOutOverload() so its "nothing left to raise"
diagnosis waits for the block-size lever too before calling the situation
hopeless. */
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
much weaker reason. So: escalate only on each fresh ceiling underrun, up to
the cap, then stop there for good; never revert. A user who wants lower
latency back can pick a smaller block size by hand in the Audio module, same
as always.

Measured on hardware (see audio_oboe.cpp's DEFAULT_BLOCK_SIZE comment):
doubling block size roughly halves underruns. The ceiling is NOT a taste
judgement about how much latency is bearable -- it is audioMaxUsefulBlockSize(),
the biggest block the live stream's own buffer can actually hold. Past that
the trade stops being latency-for-headroom and becomes a callback that cannot
possibly be on time, which an earlier version of this function got wrong: it
climbed to 4096 frames (85 ms) on a OnePlus 8T whose whole buffer capacity is
1536 frames (32 ms), and measured 225 underruns in 30 s for the trouble. A
Shared stream does not change that arithmetic, so it no longer gets a taller
ladder -- being denied the low-latency path is a reason the headroom is
needed, not a reason the buffer got bigger.

Because that earlier version shipped, a device can come back with a persisted
block size ABOVE what its buffer can hold, which nothing would otherwise undo
-- so this walks that back down, the one case where it lowers rather than
raises. */
/** The size Auto was at before it first raised the block in this session, or 0
if it has not. checkBlockSizeStepDown() works its way back to it. */
static int g_autoRaisedFrom = 0;

static void checkBlockSizeOverload() {
	static int32_t lastCeilingCount = 0;
	int32_t ceilingCount = rackdroid::audioCeilingUnderrunCount();
	bool freshCeilingUnderrun = ceilingCount != lastCeilingCount;
	lastCeilingCount = ceilingCount;

	int current = rackdroid::audioBlockSize();
	int cap = rackdroid::audioMaxUsefulBlockSize();
	if (current <= 0 || cap <= 0)
		return; // no device open yet -- wait for one rather than trying nothing

	// Repair first, and without waiting for an underrun to prove it: a block
	// the buffer cannot hold is wrong on its own terms, not a judgement call.
	if (current > cap) {
		LOGW("Engine: block size %d is bigger than this stream can deliver on "
			"time; returning to %d, the largest its buffer actually holds",
			current, cap);
		rackdroid::audioSetBlockSize(cap);
		g_blockSizeTried = true;
		return;
	}

	// Not on what happens while another app has the audio focus, nor on what
	// was left over from measuring a patch.
	if (rackdroid::audioFocusDisturbed() || system::getTime() - g_sweptAt < 15.0)
		return;
	// The next size up is not tried again while it is remembered as no easier
	// than this one.
	int32_t hereLoad = knownBlockLoad(current);
	int32_t upLoad = knownBlockLoad(current * 2);
	bool upKnownNoBetter = upLoad > 0 && hereLoad > 0 && upLoad + 5 >= hereLoad;
	// Nor where it would reach the device as the very same callback: on a
	// Lenovo TB-X306X every block from 128 to 1024 arrives as one 960-frame
	// burst, and a patch measured 103-106% of its deadline at all four while
	// the ladder spent three reopens finding that out.
	if (rackdroid::audioAlignedCallbackFrames(current * 2) > 0
			&& rackdroid::audioAlignedCallbackFrames(current * 2)
				== rackdroid::audioAlignedCallbackFrames(current))
		upKnownNoBetter = true;

	// The spent ladder only silences Auto; a fixed size still gets its notice.
	if ((g_blockSizeTried && rackdroid::audioBlockChoice() == 0) || !freshCeilingUnderrun)
		return;
	if (!startupSettled())
		return; // the patch is still loading; those underruns are not the workload
	// Last resort, and only once the thread tuner has none left. Doubling the
	// block size buys headroom with latency and is never undone -- the user is
	// told to put it back by hand -- so it must not be spent on the underruns
	// the tuner makes on purpose while it walks down from the core ceiling. It
	// was: on an S22 the ladder was already spent six seconds after launch, on
	// a patch that played cleanly at four threads a moment later.
	// Not a thread-COUNT gate: checkThreadCount() picks whatever count
	// measures best rather than climbing to the ceiling, so "wait until we are
	// at the ceiling" would wait forever on a Shared path.
	if (!g_threadTunerExhausted)
		return;
	// A bigger block buys time for a callback that is sometimes late. It buys
	// nothing for an engine that needs more time than the audio lasts, at any
	// block: three devices each went 128 -> 1024 in twenty seconds on a patch
	// too heavy for them, three reopens and three toasts, and underran exactly
	// as before. The same goes for telling someone their fixed size is too small.
	// ...at any block the ladder can reach, that is. Within half again of the
	// deadline a bigger block has been seen to close the gap, so it is tried.
	if (g_engineOverloaded && g_overloadPercent > 150)
		return;
	// And it is only tried for an engine that is over its deadline. For one
	// that is inside it and underruns now and then -- a stall when the screen
	// is pinched, a volume key -- a bigger block is latency and nothing else:
	// on a Nothing A024 a patch clean at 74% went to 256 frames after one
	// underrun and to 512 after three more, and measured 79% and 80% there.
	// Those underruns are the buffer's business, which is sized ahead of them.
	if (!g_engineOverloaded && rackdroid::audioBlockChoice() == 0)
		return;
	// And never during a recording. Changing the block size reopens the
	// stream, which takes the callback away for the best part of a second --
	// in a WAV that is a silent hole with nothing in the file to mark it, and
	// the take is the one thing here the user cannot simply redo. Headroom can
	// wait until they have stopped.
	if (rackdroid::audioIsRecording())
		return;
	int next = current * 2;
	if (next > cap)
		next = cap;
	// A size the user fixed is theirs. Say that it is not holding and what
	// would help, and leave the decision where it was made -- at most every
	// five minutes, which is also how long a dismissed notice stays dismissed.
	if (rackdroid::audioBlockChoice() > 0) {
		static double noticedAt = -1e9;
		double now = system::getTime();
		if (now - noticedAt >= 300.0) {
			noticedAt = now;
			LOGW("Engine: underrunning at the %d-frame block the user fixed; "
				"suggesting %s", current, next > current ? "the next size up" : "Auto");
			rackdroid::nativeAudioNotice(2, current, next > current ? next : 0);
		}
		return;
	}
	if (current >= cap || upKnownNoBetter) {
		g_blockSizeTried = true; // ladder fully spent, or its next rung known useless
		return;
	}
	// One step, then time to see what it did. It used to take the next step
	// on the very next ceiling underrun, and a reopened stream produces some
	// of its own: a Nothing A024 went 128 -> 256 -> 512 in 1.4 s, the second
	// step answering the first one's reopen.
	if (rackdroid::audioSecondsSinceStreamOpen() < 10.0)
		return;
	LOGW("Engine: still underrunning at this device's thread ceiling; trying "
		"block size %d instead of %d (it is tried lower again after a clean "
		"minute; Engine > Audio block fixes a size)", next, current);
	if (g_autoRaisedFrom <= 0)
		g_autoRaisedFrom = current;
	rackdroid::audioSetBlockSize(next, true);
	rackdroid::nativeAudioNotice(0, current, next);
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
/** How much of their time the engine's worker threads spend ready to run but
not running, 0..1, or -1 while there is not yet enough history to say.

This exists to tell two very different situations apart. If the engine cannot
keep up and nothing is holding it back, the patch really is heavier than the
device. If it cannot keep up because its threads are being kept off the CPU --
a game, a video call, a download -- then the patch is fine, and telling the
user to simplify it is wrong and costly: it asks them to throw away work to fix
someone else's problem.

/proc/stat, the obvious source, is denied to apps -- it reads back empty, which
cost a debugging round trip here. The runqueue wait in /proc/self/task/N/
schedstat is ours to read and is the better signal anyway: it says directly
that our threads were ready and something else had the core. Measured on an
S22: 0.19% idle against 29% with seven competing processes.

Workers only, not the audio callback thread: that one carries URGENT_AUDIO
priority and wins its core against ordinary work, so it shows nothing. It is
the workers arriving late at the barrier that produce the underruns. Threads
are matched by id between samples, because the tuner recreates them whenever it
changes the count and a fresh thread's counters start at zero. */
static float workerRunqueueWait() {
	static const int MAX_TRACKED = MAX_TRACKED_THREADS;
	struct Sample { int tid; unsigned long long run, wait; };
	static Sample prev[MAX_TRACKED];
	static int prevCount = 0;
	static double sampledAt = 0.0;
	static float ratio = -1.f;

	double now = system::getTime();
	if (sampledAt > 0.0 && now - sampledAt < 2.0)
		return ratio;

	Sample cur[MAX_TRACKED];
	int count = 0;
	if (DIR* dir = opendir("/proc/self/task")) {
		while (dirent* e = readdir(dir)) {
			if (count >= MAX_TRACKED)
				break;
			int tid = atoi(e->d_name);
			if (tid <= 0)
				continue;
			char path[80];
			std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
			FILE* f = std::fopen(path, "r");
			if (!f)
				continue;
			char name[32] = {0};
			bool worker = std::fgets(name, sizeof(name), f)
				&& std::strncmp(name, "Worker ", 7) == 0;
			std::fclose(f);
			if (!worker)
				continue;
			std::snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat", tid);
			f = std::fopen(path, "r");
			if (!f)
				continue;
			unsigned long long run = 0, wait = 0;
			// "<ns on cpu> <ns waiting on the runqueue> <timeslices>"
			bool ok = std::fscanf(f, "%llu %llu", &run, &wait) == 2;
			std::fclose(f);
			if (ok)
				cur[count++] = {tid, run, wait};
		}
		closedir(dir);
	}

	unsigned long long dRun = 0, dWait = 0;
	for (int i = 0; i < count; i++) {
		for (int j = 0; j < prevCount; j++) {
			if (prev[j].tid != cur[i].tid)
				continue;
			if (cur[i].run >= prev[j].run && cur[i].wait >= prev[j].wait) {
				dRun += cur[i].run - prev[j].run;
				dWait += cur[i].wait - prev[j].wait;
			}
			break;
		}
	}
	if (dRun + dWait > 0)
		ratio = (float) ((double) dWait / (double) (dRun + dWait));
	else if (prevCount > 0 && count == 0)
		ratio = -1.f; // no workers at all: nothing to say

	for (int i = 0; i < count; i++)
		prev[i] = cur[i];
	prevCount = count;
	sampledAt = now;
	return ratio;
}


/** The other half of the block-size ladder: the way back down.

checkBlockSizeOverload() doubles the block when nothing else is left, buying
headroom with latency, and until now nothing ever undid it. So a block raised
once -- possibly to compensate for a thread count we have since learned was
wrong -- stayed raised for the life of the patch, because Rack saves the value
into the .vcv. Both test phones were found sitting at 1024 for that reason.

What it costs is not small. On an S22, same patch, same minute: at 1024 the
stream measured 38.5 ms of latency and the block added 21.3; at 256, 6.4 and
5.3. Sixty milliseconds down to twelve, for no measurable CPU. On a OnePlus 8T,
where the fast audio path is denied outright, 84 ms down to 50 -- more than the
sharing mode itself is worth there.

The catch is that a block size is a stream construction parameter, so changing
it closes and reopens the stream: roughly a third of a second of silence on an
8T, two thirds on an S22. There is no gapless way to do it.

So it happens ONCE, a few seconds after launch, and never again while the
instrument is being played. A gap is worth spending to repair audio that is
already broken -- that is what the upward ladder is for -- and never worth
spending to chase latency under someone's hands. A size that turns out not to
hold is written down, so the next launch does not buy the same answer again. */
static void checkBlockSizeStepDown() {
	// Rack's own smallest offered size. There is no need to guess a safer
	// floor than that: the step below is measured and reverted if it does not
	// hold, which is the whole point. And the cost of a small block turned out
	// not to be CPU -- an S22 measured 211% at 64 frames against 215% at 256,
	// because what dominates is the per-sample barrier, not the per-block
	// work. It buys 7 ms of total latency there, against 60 at 1024.
	static const int BLOCK_FLOOR = 64;
	static bool done = false;
	static int probedFrom = 0;
	static double probedAt = 0.0;
	// Undoing what the upward ladder did in this session: asked again after a
	// clean minute, and after a failure twice as long as the time before.
	static bool probeIsUndo = false;
	static double undoAt = 0.0;
	static double undoBackoff = 120.0;

	int current = rackdroid::audioBlockSize();
	if (current <= 0)
		return;
	double now = system::getTime();
	// The user fixed the size: nothing here moves it, in either direction.
	if (rackdroid::audioBlockChoice() > 0) {
		probedFrom = 0;
		g_autoRaisedFrom = 0;
		return;
	}

	if (probedFrom > 0) {
		// Never while a recording is running. Reverting reopens the stream,
		// which costs roughly half a second of callbacks -- in a WAV that is a
		// silent hole with nothing to mark it. The step-down below already
		// refuses to start during a recording; the revert did not, and on a
		// Nothing A024 it fired 0.5 s after the user pressed record. Waiting
		// costs only that this verdict is reached a little later.
		if (rackdroid::audioIsRecording())
			return;
		// Give the reopen a few seconds -- it costs underruns of its own, and
		// judging the new size on those judges it on the act of trying it --
		// then a short spell to show whether it holds.
		if (rackdroid::audioSecondsSinceStreamOpen() < 5.0)
			return;
		if (rackdroid::audioUnderrunsRecently()) {
			LOGW("Engine: a %d-frame block does not hold here; back to %d and "
				"noted, so the next launch does not try it again",
				current, probedFrom);
			rackdroid::audioNoteBlockTooSmall(current);
			rackdroid::audioSetBlockSize(probedFrom);
			if (probeIsUndo) {
				undoAt = now + undoBackoff;
				undoBackoff = std::min(undoBackoff * 2.0, 960.0);
			}
			probedFrom = 0;
			return;
		}
		if (now - probedAt < 20.0)
			return;
		LOGI("Engine: a %d-frame block holds; keeping the lower latency", current);
		if (probeIsUndo)
			rackdroid::nativeAudioNotice(1, probedFrom, current);
		if (current <= g_autoRaisedFrom)
			g_autoRaisedFrom = 0; // back where the session started
		probedFrom = 0;
		return;
	}

	// The ladder went up in this session and things have been quiet since: one
	// step back down. This is the half that was missing -- a block raised once
	// stayed raised, and the patch then saved it.
	// Not while the patch is clean only just. The bigger block is part of why
	// it is clean, and "quiet for a minute" at 85% of the deadline is not
	// spare capacity: an SM-S901E stepped 256 -> 128 after exactly such a
	// minute and took 380 underruns in the next two seconds.
	// Low enough that the smaller block does not land straight back in the
	// band where it gets raised again: the step has been worth about a third.
	static const int32_t ROOM_TO_STEP_DOWN_PERCENT = 55;
	bool room = g_lastCleanLoad > 0 && g_lastCleanLoad < ROOM_TO_STEP_DOWN_PERCENT;
	bool undo = done && room && g_autoRaisedFrom > 0 && current > g_autoRaisedFrom
		&& now >= undoAt && rackdroid::audioSecondsSinceUnderrun() >= 60.0
		&& rackdroid::audioSecondsSinceStreamOpen() >= 60.0;
	if (done && !undo)
		return;
	// Early, but after the patch has loaded and applied its own value, and
	// after the first seconds of underruns that mean nothing.
	if (!startupSettled() || rackdroid::audioSecondsSinceStreamOpen() < 5.0)
		return;
	// And not while the thread search is still moving: see g_threadTunerSettled.
	if (!g_threadTunerSettled)
		return;
	// The launch-time probe asks the same question and gets the same answer.
	if (!undo && !room) {
		if (g_lastCleanLoad >= ROOM_TO_STEP_DOWN_PERCENT) {
			done = true;
			LOGI("Engine: at %d%% of the audio deadline there is no room to try "
				"a smaller block; staying at %d frames", g_lastCleanLoad, current);
		}
		return;
	}
	done = true;
	probeIsUndo = undo;
	// Whatever stops an undo below, it is asked again in a minute rather than
	// on the next frame; a probe that starts and fails sets its own, longer wait.
	if (undo)
		undoAt = now + 60.0;

	int want = current / 2;
	if (want < BLOCK_FLOOR)
		return;
	// Not towards a size this patch is remembered as being too much at.
	if (knownBlockLoad(want) >= 85)
		return;
	// A smaller block only buys latency if it actually reaches the device as a
	// smaller callback. Where the burst is larger than the block, it does not:
	// on a Lenovo TB-X306X (burst 960) a step from 512 to 256 produced the same
	// 960-frame callback, cost a reopen and an underrun, measured 9 ms WORSE
	// than before, and then announced "a 256-frame block holds; keeping the
	// lower latency". Wasted work is forgivable; claiming an improvement that
	// did not happen is not.
	int alignedNow = rackdroid::audioAlignedCallbackFrames(current);
	int alignedWant = rackdroid::audioAlignedCallbackFrames(want);
	if (alignedNow > 0 && alignedWant == alignedNow) {
		g_autoRaisedFrom = 0; // nothing lower to be had: stop asking
		LOGI("Engine: a %d-frame block would reach this device as the same "
			"%d-frame callback as %d does -- its burst is %d, so there is no "
			"lower latency to be had here", want, alignedWant, current,
			alignedNow);
		return;
	}
	if (!undo && want == rackdroid::audioKnownTooSmallBlock()) {
		int waiting = rackdroid::audioTooSmallLaunchesLeft();
		if (waiting > 0) {
			LOGI("Engine: %d frames is where this device settled; a %d-frame "
				"block did not hold when it was last tried, and is worth "
				"asking about again in %d %s", current, want, waiting,
				waiting == 1 ? "launch" : "launches");
			return;
		}
		// The verdict has served its time. What condemned the size may have
		// been a passing disturbance rather than the device, and the only way
		// to find out is to spend one reopen asking.
		LOGI("Engine: %d frames did not hold the last time it was tried, but "
			"that was several launches ago; asking again", want);
	}
	if (rackdroid::audioIsRecording())
		return;
	LOGI("Engine: trying a %d-frame block instead of %d, for lower latency",
		want, current);
	probedFrom = current;
	probedAt = now;
	rackdroid::audioSetBlockSize(want);
}


/** Goes back to a block size that is remembered as clearly easier than the one
in use, once the tuner has a verdict on this one and that verdict is poor.
Either direction: this is what brings the engine back from a step up that made
things worse. */
static void checkBlockByLoad() {
	if (rackdroid::audioBlockChoice() > 0 || rackdroid::audioIsRecording())
		return;
	// Two minutes between one reopen and the next it decides on: each is a
	// gap, and a verdict needs that long to be worth another.
	if (rackdroid::audioSecondsSinceStreamOpen() < 120.0)
		return;
	int current = rackdroid::audioBlockSize();
	if (current <= 0)
		return;
	int slot = blockSlot(current);
	// A verdict on this size, reached since the stream last opened at it.
	if (g_blockLoad[slot] <= 0
			|| system::getTime() - g_blockLoadAt[slot] > rackdroid::audioSecondsSinceStreamOpen())
		return;
	int32_t here = g_blockLoad[slot];
	int best = 0;
	int32_t bestLoad = here;
	int cap = rackdroid::audioMaxUsefulBlockSize();
	// A clearly easier size is only worth a reopen for an engine that is
	// short of room; a smaller one that is no worse always is, below.
	for (int b = 64; b <= 4096 && here >= 85; b *= 2) {
		int32_t l = knownBlockLoad(b);
		if (b != current && (cap <= 0 || b <= cap) && l > 0 && l + 10 < bestLoad) {
			bestLoad = l;
			best = b;
		}
	}
	if (best <= 0) {
		// Nothing clearly easier. Then at least not a bigger block than one
		// that does the same job: the smallest size remembered as no worse
		// than this one, which is how a raise that bought nothing is undone.
		for (int b = 64; b < current; b *= 2) {
			int32_t l = knownBlockLoad(b);
			if (l > 0 && l <= here + 5) {
				best = b;
				bestLoad = l;
				break;
			}
		}
		if (best <= 0)
			return;
		LOGW("Engine: a %d-frame block bought nothing over %d (%d%% of the "
			"deadline against %d%%); going back to it", current, best, here, bestLoad);
	}
	else
		LOGW("Engine: this patch was easier at a %d-frame block (%d%% of the deadline "
			"against %d%% at %d); going back to it", best, bestLoad, here, current);
	if (best < current && g_autoRaisedFrom >= best)
		g_autoRaisedFrom = 0;
	rackdroid::audioSetBlockSize(best, best > current);
	rackdroid::nativeAudioNotice(best > current ? 3 : 1, current, best);
}

static void checkMaxedOutOverload() {
	// Sample first and unconditionally: the share is a delta between two
	// readings a couple of seconds apart, so it has to be taken while nothing
	// is wrong. Asking for it only at the moment the notice fires gets the
	// first reading ever and therefore no answer at all -- which is exactly
	// what happened the first time this was tested: "-1% of the phone's busy
	// CPU". Cheap enough to leave running: two small reads every two seconds.
	float waiting = workerRunqueueWait();

	static bool shown = false;
	static int32_t lastCeilingCount = 0;

	int32_t ceilingCount = rackdroid::audioCeilingUnderrunCount();
	bool freshCeilingUnderrun = ceilingCount != lastCeilingCount;
	lastCeilingCount = ceilingCount;

	// Said once per overload, not once per launch: a quiet minute means that
	// one is over, and the next heavy patch deserves its own explanation.
	if (shown && rackdroid::audioSecondsSinceUnderrun() >= 60.0)
		shown = false;
	// An engine that cannot keep up at any thread count has no use for the
	// block-size ladder, so it does not wait for it.
	if (shown || !freshCeilingUnderrun || !(g_blockSizeTried || g_engineOverloaded))
		return;
	// Not while the thread tuner is still working. It opens at the ceiling and
	// walks down, so the first seconds of a heavy patch underrun by design: an
	// S22 showed "this patch needs more CPU than your device can give" six
	// seconds after launch and was playing the same patch cleanly at three
	// threads fifteen seconds later. Bad advice, and the user acts on it.
	if (!g_threadTunerExhausted)
		return;
	int ceiling = engineThreadCeiling();
	if (ceiling <= 1)
		return;
	shown = true;

	int thermal = rackdroid::thermalStatus();
	// PowerManager.THERMAL_STATUS_SEVERE = 3.
	bool throttled = thermal >= 3;
	// Less than half of the phone's busy time being ours, while we cannot keep
	// up, means the shortage is not the patch's doing. Half is a deliberately
	// loose line: the engine is by far the heaviest thing on a phone when it
	// runs at all, so anything near half already means real company.
	// Measured on an S22: 0.19% with the phone to itself, 29% against seven
	// competing processes. Ten per cent sits far from both.
	bool contended = !throttled && waiting >= 0.10f;
	LOGW("Engine: underrunning at %d threads with nothing left to raise "
		"(ceiling %d of %d cores); thermal status %d (%s); workers spent %.1f%% "
		"of their time waiting for a core (%s)",
		settings::threadCount, ceiling, ceiling + RESERVED_CORES,
		thermal, throttled ? "throttled" : "not throttled",
		waiting >= 0.f ? waiting * 100.f : -1.f,
		contended ? "something else is competing" : "the engine is the main load");
	rackdroid::showEngineNotice(throttled ? 1 : (contended ? 2 : 0));
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
		// via eglSwapBuffers) while a surface exists; 100 ms once it is gone,
		// because the engine keeps playing in the background and its
		// maintenance -- above all the thread-count tuner -- has to keep
		// running with it. Blocking forever here froze the tuner at whatever
		// count was right when the app was last on screen, and a phone that
		// throttles while backgrounded then underruns with nothing awake to
		// correct it. Only with no engine at all is waiting indefinitely free.
		int timeout = !rd.rackStarted ? -1
			: rackdroid::windowHasSurface() ? 0 : 100;
		int ident = ALooper_pollOnce(timeout, NULL, &events, (void**) &source);
		if (ident >= 0 && source)
			source->process(app, source);

		if (app->destroyRequested) {
			rd.stopRack();
			return;
		}

		if (rd.rackStarted) {
			try {
				// Engine and audio outlive the surface, so what tunes them
				// runs whether or not anything is on screen. None of these
				// touch the window or the scene.
				rackdroid::windowSetPhase(rackdroid::RENDER_TUNE);
				checkWorkerPriority();
				if (rackdroid::audioReportSlowCallbacks() > 0)
					reportEngineThreadCores();
				rackdroid::audioReleaseIdleDevice();
				rackdroid::audioReportUnderruns();
				rackdroid::audioReportLatency();
				// Only once the engine has stopped thrashing about: the
				// buffer means nothing while the patch is still loading.
				if (startupSettled())
					rackdroid::audioTrimBuffer();
				checkAdpfTarget();
				rackdroid::windowSetAudioStressed(rackdroid::audioUnderrunsRecently());
				checkThreadCount();
				rackdroid::audioApplyBlockChoice();
				checkBlockSizeOverload();
				checkBlockSizeStepDown();
				checkBlockByLoad();

				// The rest needs a surface: input, the scene, a dialog to
				// show, or a restart the user would not see coming.
				if (APP->window && rackdroid::windowHasSurface()) {
					rackdroid::touchStep();
					rackdroid::processTourDemo();
					checkZoomCeiling();
					checkMaxedOutOverload();
					checkLanguageChanged();
					APP->window->step();
				}
				rackdroid::windowSetPhase(rackdroid::RENDER_IDLE);
			}
			catch (std::exception& e) {
				LOGE("FATAL in frame step: %s", e.what());
				throw;
			}
		}
	}
}
