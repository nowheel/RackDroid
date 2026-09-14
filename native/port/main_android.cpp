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
#include <vector>
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


static void applyWorkerAffinity(const std::vector<int>& workers) {
	int cores = system::getLogicalCoreCount();
	if (cores <= 1)
		return;
	int reservedCpu = pickReservedCpu(cores);
	cpu_set_t mask;
	CPU_ZERO(&mask);
	for (int cpu = 0; cpu < cores; cpu++) {
		if (cpu != reservedCpu)
			CPU_SET(cpu, &mask);
	}
	int pinned = 0;
	for (int tid : workers) {
		if (sched_setaffinity(tid, sizeof(mask), &mask) == 0)
			pinned++;
	}
	if (pinned > 0)
		LOGI("Engine: pinned %d worker threads off cpu%d, reserved for the system",
			pinned, reservedCpu);
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
	if (settings::threadCount != lastThreadCount) {
		lastThreadCount = settings::threadCount;
		applyAt = system::getTime() + 0.5;
	}
	if (applyAt > 0.0 && system::getTime() >= applyAt) {
		applyAt = 0.0;
		std::vector<int> workers = collectWorkerThreads();
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
	int block = rackdroid::audioBlockSize();
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
	static int scores[MAX_TRACKED_THREADS + 1];
	// When each score was taken. A measurement describes the conditions it was
	// made under, and those expire: scores collected while another app was
	// eating the phone become lies the moment it stops. Without this the engine
	// could park for good on a rung chosen under conditions that no longer
	// exist -- seen doing exactly that on an S22, stuck at seven threads and
	// thirty underruns a second for four minutes after an artificial load was
	// removed, because every alternative had been scored during it.
	static double scoreAt[MAX_TRACKED_THREADS + 1];
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

	// Never walk down to one. One thread is no parallelism at all, and no
	// measurement taken on any device has ever made it the best rung: on the
	// 8T one and two were both clean, on the S22 one produced seventy-eight
	// underruns in a window where two produced seven. Trying it gains nothing
	// and occasionally costs five seconds of ruined audio, so the ladder
	// stops at two wherever there are two to have.
	int floorCount = (ceiling >= 2) ? 2 : 1;

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
			int remembered = rememberedThreadCount(ceiling);
			want = remembered > 0 ? remembered : ceiling;
			why = remembered > 0 ? "where it settled last time" : "first guess";
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

	// Starting up is not a measurement. The patch is still loading and the
	// Oboe stream reopens several times while the Audio module settles, which
	// on an 8T cost fifteen underruns in the first five seconds and was enough
	// to walk the engine straight off a perfectly good count.
	if (now < warmupUntil) {
		windowStartedAt = now;
		windowStartCount = total;
		windowTouched = false;
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

	if (now - windowStartedAt < WINDOW_SEC)
		return;

	int32_t underruns = total - windowStartCount;
	int current = settings::threadCount;
	bool touched = windowTouched;
	windowStartedAt = now;
	windowStartCount = total;
	windowTouched = false;
	if (touched) {
		// Say so: a discarded window looks exactly like a tuner doing nothing,
		// and telling those apart from a log file is otherwise guesswork.
		if (underruns > 0)
			LOGI("Engine: %d underruns in %.0fs at %d threads, but the window was "
				"disturbed; not counting it", underruns, WINDOW_SEC, current);
		return; // measured the disturbance, not the patch
	}

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
		LOGI("Engine: %d underruns in %.0fs at %d threads; ignoring (%d of %d, %s)",
			underruns, WINDOW_SEC, current, toleratedRuns, TOLERATED_RUNS_MAX,
			proven ? "this count has run clean before" : "too few to act on");
		return; // and do not record it as this rung's score
	}

	if (current >= 1 && current <= MAX_TRACKED_THREADS) {
		scores[current] = underruns;
		scoreAt[current] = now;
	}

	// A score is only evidence while the conditions that produced it still
	// hold. Past this, treat the rung as never measured and let the search go
	// and look again -- which is what breaks the deadlock described above.
	static const double SCORE_TTL_SEC = 60.0;
	auto known = [&](int i) {
		return i >= 1 && i <= MAX_TRACKED_THREADS && scores[i] >= 0
			&& now - scoreAt[i] < SCORE_TTL_SEC;
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

	if (underruns == 0) {
		toleratedRuns = 0;
		if (current >= 1 && current <= MAX_TRACKED_THREADS)
			provenClean[current] = true;
		if (settledAt != current) {
			LOGI("Engine: %d threads is running clean", current);
			settledAt = current;
			cleanSince = now;
		}
		rememberThreadCount(current);
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
		if (cleanSince > 0.0 && now - cleanSince >= probeAfter
				&& lower >= floorCount && worthProbing) {
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
		LOGW("Engine: %d underruns in %.0fs at %d threads; %d it is, then",
			underruns, WINDOW_SEC, current, probedFrom);
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

	// An unmeasured neighbour first -- nearest, and downward before upward,
	// since the barrier cost is the commoner problem on a phone. Exploration
	// terminates because each count is only unmeasured once, until its score
	// goes stale.
	int candidate = -1;
	if (current - 1 >= floorCount && !known(current - 1))
		candidate = current - 1;
	else if (current + 1 <= ceiling && !known(current + 1))
		candidate = current + 1;
	else {
		// Everything nearby is known: go to the best of it, but only if it is
		// clearly better. Without that margin two counts that both underrun a
		// little would swap places every window forever.
		int best = current;
		int32_t bestScore = underruns;
		for (int i = floorCount; i <= ceiling && i <= MAX_TRACKED_THREADS; i++) {
			if (known(i) && scores[i] < bestScore) {
				bestScore = scores[i];
				best = i;
			}
		}
		if (best != current && bestScore * 2 < underruns)
			candidate = best;
	}
	if (candidate < 0) {
		g_threadTunerExhausted = true;
		return; // nothing known to be better; stay where we are
	}

	LOGW("Engine: %d underruns in %.0fs at %d threads; trying %d",
		underruns, WINDOW_SEC, current, candidate);
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

	if (g_blockSizeTried || !freshCeilingUnderrun)
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
	// And never during a recording. Changing the block size reopens the
	// stream, which takes the callback away for the best part of a second --
	// in a WAV that is a silent hole with nothing in the file to mark it, and
	// the take is the one thing here the user cannot simply redo. Headroom can
	// wait until they have stopped.
	if (rackdroid::audioIsRecording())
		return;
	if (current >= cap) {
		g_blockSizeTried = true; // ladder fully spent
		return;
	}
	int next = current * 2;
	if (next > cap)
		next = cap;
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
into the .vcv. Both test phones were found sitting at 1024 frames for exactly
that reason.

What it costs is not small. Measured on an S22 at 1024 against 256, same patch,
same moment: 38.5 ms of stream latency against 6.4, plus 21.3 ms of engine
block against 5.3. Sixty milliseconds down to twelve, for no measurable CPU
(223% against 215%) and three minutes without a single underrun. That is more
latency than the fast audio path itself is worth on the devices that deny it.

Unlike a thread change this is not free to try: changing the block closes and
reopens the stream, roughly three quarters of a second of silence. So it is
attempted only after a long quiet spell, never during a recording, and the wait
doubles after each attempt that fails, so a device that genuinely needs a big
block is asked at most a few times a session. */
static void checkBlockSizeUnderload() {
	static const int BLOCK_FLOOR = 128;
	static double quietSince = 0.0;
	static int32_t lastUnderruns = -1;
	static double waitFor = 120.0;
	static const double WAIT_MAX = 1800.0;
	static int probedFrom = 0;

	int current = rackdroid::audioBlockSize();
	if (current <= 0)
		return;
	double now = system::getTime();
	int32_t underruns = rackdroid::audioUnderrunCount();

	// "Quiet" means not one underrun, not a tolerable few: this is the wrong
	// thing to gamble a stream reopen on.
	// Changing the block reopens the stream, and a reopen always costs a few
	// underruns of its own. Judging the new size on those is judging it on the
	// act of trying it -- the same mistake the thread tuner made with its own
	// moves. Give the stream a few seconds to settle before believing anything
	// it says. Without this the step down was condemned four seconds in, every
	// time, and could never succeed.
	if (rackdroid::audioSecondsSinceStreamOpen() < 5.0) {
		lastUnderruns = underruns;
		quietSince = now;
		return;
	}

	if (underruns != lastUnderruns || !startupSettled()) {
		lastUnderruns = underruns;
		quietSince = now;
		if (probedFrom > 0) {
			// The step down did not hold. Go back, and ask less often.
			LOGW("Engine: %d-frame block underran; back to %d", current, probedFrom);
			rackdroid::audioSetBlockSize(probedFrom);
			probedFrom = 0;
			waitFor = (waitFor * 2.0 > WAIT_MAX) ? WAIT_MAX : waitFor * 2.0;
			quietSince = 0.0;
		}
		return;
	}

	if (quietSince <= 0.0) {
		quietSince = now;
		return;
	}
	if (now - quietSince < waitFor)
		return;

	if (probedFrom > 0) {
		// It held through a whole quiet spell. Keep it, and allow the next
		// step down to be considered at the normal interval again.
		LOGI("Engine: %d-frame block is holding; keeping the lower latency",
			current);
		probedFrom = 0;
		waitFor = 120.0;
		quietSince = now;
		return;
	}

	if (current / 2 < BLOCK_FLOOR)
		return; // as low as this is willing to go
	if (rackdroid::audioIsRecording())
		return; // a reopen is a hole in the take; latency can wait
	if (!g_threadTunerExhausted && settings::threadCount > 0) {
		// Only while the thread side is settled, so two mechanisms are not
		// moving at once and neither can read the other's cost as its own.
		if (rackdroid::audioUnderrunsRecently())
			return;
	}
	LOGI("Engine: quiet for %.0fs at a %d-frame block; trying %d for lower "
		"latency", now - quietSince, current, current / 2);
	probedFrom = current;
	rackdroid::audioSetBlockSize(current / 2);
	quietSince = 0.0;
	lastUnderruns = rackdroid::audioUnderrunCount();
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

	if (shown || !freshCeilingUnderrun || !g_blockSizeTried)
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
				checkWorkerPriority();
				rackdroid::audioReleaseIdleDevice();
				rackdroid::audioReportUnderruns();
				rackdroid::audioReportLatency();
				checkAdpfTarget();
				rackdroid::windowSetAudioStressed(rackdroid::audioUnderrunsRecently());
				checkThreadCount();
				checkBlockSizeOverload();
				checkBlockSizeUnderload();

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
			}
			catch (std::exception& e) {
				LOGE("FATAL in frame step: %s", e.what());
				throw;
			}
		}
	}
}
