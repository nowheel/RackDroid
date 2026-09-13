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

/** engineThreadCeiling() (below) gives the system a core back by asking for
one fewer thread than there are cores -- a soft guarantee: it assumes the
scheduler leaves the excluded core alone, and a user overriding our count
through the Threads menu erases it entirely, since the affinity mask below
does not exist yet at that point. This is the hard version of the same
guarantee, and it does not depend on our thread count advice being followed:
it excludes Worker threads from one specific logical CPU by mask, however
many of them end up existing, so Android's compositor/system_server/touch
pipeline always has a core no real-time Worker will ever be scheduled onto,
full stop.

Unlike applyWorkerPriority() above, this needs no JNI round-trip:
sched_setaffinity() on a thread of one's own process is a plain, unprivileged
syscall (governed by cpuset/cgroup membership, not a capability like
CAP_SYS_NICE), so asking for a subset of the cores this cpuset already
grants just works. */
static void applyWorkerAffinity() {
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

	DIR* dir = opendir("/proc/self/task");
	if (!dir)
		return;
	int pinned = 0;
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
		if (worker && sched_setaffinity(tid, sizeof(mask), &mask) == 0)
			pinned++;
	}
	closedir(dir);
	if (pinned > 0)
		LOGI("Engine: pinned %d worker threads off cpu%d, reserved for the system",
			pinned, reservedCpu);
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
		applyWorkerPriority();
		applyWorkerAffinity();
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
	return cores <= 1 ? cores : cores - RESERVED_CORES;
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
static const int MAX_TRACKED_THREADS = 64;

static void checkThreadCount() {
	static const double WINDOW_SEC = 5.0;
	static int scores[MAX_TRACKED_THREADS + 1];
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
	if (rackdroid::audioBlockSize() <= 0)
		return; // no audio device open yet: nothing to measure with

	double now = system::getTime();
	int32_t total = rackdroid::audioUnderrunCount();

	if (!initialised) {
		for (int i = 0; i <= MAX_TRACKED_THREADS; i++)
			scores[i] = -1;
		bool shared = rackdroid::audioIsSharedMode();
		int want = shared ? 2 : ceiling;
		if (want > ceiling)
			want = ceiling;
		LOGI("Engine: starting at %d threads (%s audio path, %d-thread ceiling)",
			want, shared ? "Shared" : "Exclusive", ceiling);
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

	if (rackdroid::windowSecondsSinceInteraction() < INTERACTION_SETTLE_SEC)
		windowTouched = true;

	if (now - windowStartedAt < WINDOW_SEC)
		return;

	int32_t underruns = total - windowStartCount;
	int current = settings::threadCount;
	bool touched = windowTouched;
	windowStartedAt = now;
	windowStartCount = total;
	windowTouched = false;
	if (touched)
		return; // measured the gesture, not the patch: discard and try again

	if (current >= 1 && current <= MAX_TRACKED_THREADS)
		scores[current] = underruns;

	if (underruns == 0) {
		if (settledAt != current) {
			LOGI("Engine: %d threads is running clean", current);
			settledAt = current;
		}
		return;
	}
	settledAt = -1;

	// An unmeasured neighbour first -- nearest, and downward before upward,
	// since the barrier cost is the commoner problem on a phone. Exploration
	// terminates because each count is only unmeasured once.
	int candidate = -1;
	if (current - 1 >= 1 && scores[current - 1] < 0)
		candidate = current - 1;
	else if (current + 1 <= ceiling && scores[current + 1] < 0)
		candidate = current + 1;
	else {
		// Everything nearby is known: go to the best of it, but only if it is
		// clearly better. Without that margin two counts that both underrun a
		// little would swap places every window forever.
		int best = current;
		int32_t bestScore = underruns;
		for (int i = 1; i <= ceiling && i <= MAX_TRACKED_THREADS; i++) {
			if (scores[i] >= 0 && scores[i] < bestScore) {
				bestScore = scores[i];
				best = i;
			}
		}
		if (best != current && bestScore * 2 < underruns)
			candidate = best;
	}
	if (candidate < 0)
		return; // nothing known to be better; stay where we are

	LOGW("Engine: %d underruns in %.0fs at %d threads; trying %d",
		underruns, WINDOW_SEC, current, candidate);
	settings::threadCount = candidate;
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
	// No thread-count gate: checkThreadCount() picks whatever count measures
	// best rather than climbing to the ceiling, so "wait until we are at the
	// ceiling" would mean waiting forever on a Shared path.
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
static void checkMaxedOutOverload() {
	static bool shown = false;
	static int32_t lastCeilingCount = 0;

	int32_t ceilingCount = rackdroid::audioCeilingUnderrunCount();
	bool freshCeilingUnderrun = ceilingCount != lastCeilingCount;
	lastCeilingCount = ceilingCount;

	if (shown || !freshCeilingUnderrun || !g_blockSizeTried)
		return;
	int ceiling = engineThreadCeiling();
	if (ceiling <= 1)
		return;
	shown = true;

	int thermal = rackdroid::thermalStatus();
	// PowerManager.THERMAL_STATUS_SEVERE = 3.
	bool throttled = thermal >= 3;
	LOGW("Engine: underrunning at %d threads with nothing left to raise "
		"(ceiling %d of %d cores); thermal status %d (%s)",
		settings::threadCount, ceiling, ceiling + RESERVED_CORES,
		thermal, throttled ? "throttled" : "not throttled");
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
			rackdroid::audioReleaseIdleDevice();
			checkZoomCeiling();
			rackdroid::windowSetAudioStressed(rackdroid::audioUnderrunsRecently());
			checkThreadCount();
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
