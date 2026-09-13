/* Android Dynamic Performance Framework hint session. See adpf.hpp for what
this is for and why it is introduced on its own.

Resolved with dlsym rather than called directly. The NDK marks these
__INTRODUCED_IN(__ANDROID_API_T__) (API 33) and minSdk here is 29, so linking
them normally is a compile error, and the weak-symbol build flag that would
allow it changes how the whole module links for the sake of six functions.
Looking them up by name costs one dlopen at startup and says plainly, in the
code, that they are optional. */
#include "adpf.hpp"

#include <atomic>
#include <mutex>
#include <vector>

#include <dlfcn.h>
#include <android/log.h>

#include <logger.hpp>

#define ADPF_LOG(...) do { \
	__android_log_print(ANDROID_LOG_INFO, "rackdroid.adpf", __VA_ARGS__); \
	INFO(__VA_ARGS__); \
} while (0)

namespace rackdroid {

namespace {

// Opaque to us, exactly as they are to the NDK header.
struct AHintManager;
struct AHintSession;

typedef AHintManager* (*GetManagerFn)();
typedef AHintSession* (*CreateSessionFn)(AHintManager*, const int32_t*, size_t, int64_t);
typedef int (*SetThreadsFn)(AHintSession*, const int32_t*, size_t);
typedef int (*UpdateTargetFn)(AHintSession*, int64_t);
typedef int (*ReportActualFn)(AHintSession*, int64_t);
typedef void (*CloseSessionFn)(AHintSession*);

struct Api {
	GetManagerFn getManager = NULL;
	CreateSessionFn createSession = NULL;
	SetThreadsFn setThreads = NULL;     // Android 14+; NULL on 13
	UpdateTargetFn updateTarget = NULL;
	ReportActualFn reportActual = NULL;
	CloseSessionFn closeSession = NULL;
	bool resolved = false;
	bool usable = false;
};

Api g_api;

/** Looks the entry points up once. Everything below is a no-op if they are not
all there, which is the case on Android 12 and earlier. */
const Api& api() {
	if (g_api.resolved)
		return g_api;
	g_api.resolved = true;
	// Already loaded into the process -- the port layer links libandroid --
	// so this is a reference count, not a load.
	void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_NOLOAD);
	if (!lib)
		lib = dlopen("libandroid.so", RTLD_NOW);
	if (!lib) {
		ADPF_LOG("ADPF: libandroid.so not available; running without it");
		return g_api;
	}
	g_api.getManager = (GetManagerFn) dlsym(lib, "APerformanceHint_getManager");
	g_api.createSession = (CreateSessionFn) dlsym(lib, "APerformanceHint_createSession");
	g_api.setThreads = (SetThreadsFn) dlsym(lib, "APerformanceHint_setThreads");
	g_api.updateTarget = (UpdateTargetFn) dlsym(lib, "APerformanceHint_updateTargetWorkDuration");
	g_api.reportActual = (ReportActualFn) dlsym(lib, "APerformanceHint_reportActualWorkDuration");
	g_api.closeSession = (CloseSessionFn) dlsym(lib, "APerformanceHint_closeSession");
	g_api.usable = g_api.getManager && g_api.createSession && g_api.updateTarget
		&& g_api.reportActual && g_api.closeSession;
	if (!g_api.usable)
		ADPF_LOG("ADPF: not offered by this Android version; running without it");
	else if (!g_api.setThreads)
		ADPF_LOG("ADPF: available, but this Android cannot re-thread a session; "
			"it will be rebuilt instead when the thread count changes");
	return g_api;
}

/* The session belongs to the render thread, which is the only one that
creates, re-threads or closes it. The audio callback only reports, and only
ever with try_lock: a late report is worth nothing, and a callback waiting on
a mutex is worth less than nothing. */
std::mutex g_mutex;
AHintSession* g_session = NULL;
int64_t g_targetNanos = 0;
std::vector<int32_t> g_tids;
std::atomic<bool> g_active{false};
bool g_refused = false; // the system said no once; stop asking

/** Caller holds g_mutex. */
void closeLocked() {
	if (!g_session)
		return;
	api().closeSession(g_session);
	g_session = NULL;
	g_active.store(false, std::memory_order_relaxed);
}

/** Caller holds g_mutex. Returns true if a session exists afterwards. */
bool openLocked() {
	if (g_session)
		return true;
	if (g_targetNanos <= 0 || g_tids.empty())
		return false; // nothing to promise yet
	AHintManager* manager = api().getManager();
	if (!manager) {
		// A device may simply not offer the service. Say so once: a tester
		// reporting "no difference" and a session that never existed are
		// different findings, and only the log can tell them apart.
		g_refused = true;
		ADPF_LOG("ADPF: no hint manager on this device; running without it");
		return false;
	}
	g_session = api().createSession(manager, g_tids.data(), g_tids.size(), g_targetNanos);
	if (!g_session) {
		g_refused = true;
		ADPF_LOG("ADPF: the system refused a hint session; running without it");
		return false;
	}
	g_active.store(true, std::memory_order_relaxed);
	return true;
}

} // namespace


void adpfSetThreads(const int* tids, size_t count) {
	if (!api().usable || g_refused || !tids || count == 0)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);

	std::vector<int32_t> wanted(tids, tids + count);
	if (g_session && wanted == g_tids)
		return; // same threads as last time; nothing to say
	g_tids.swap(wanted);

	if (!g_session) {
		if (openLocked())
			ADPF_LOG("ADPF: hint session open for %zu threads, deadline %.2f ms",
				g_tids.size(), g_targetNanos / 1e6);
		return;
	}

	// Re-threading in place needs Android 14. On 13 the only way to change the
	// set is a new session, which is affordable here: the thread count moves
	// when the tuner moves, not per block.
	if (api().setThreads) {
		int err = api().setThreads(g_session, g_tids.data(), g_tids.size());
		if (err == 0) {
			ADPF_LOG("ADPF: session now covers %zu threads", g_tids.size());
			return;
		}
		ADPF_LOG("ADPF: setThreads failed (%d); rebuilding the session", err);
	}
	closeLocked();
	if (openLocked())
		ADPF_LOG("ADPF: session rebuilt for %zu threads", g_tids.size());
}


void adpfSetTargetNanos(int64_t nanos) {
	if (!api().usable || g_refused || nanos <= 0)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (nanos == g_targetNanos)
		return;
	g_targetNanos = nanos;
	if (!g_session) {
		// Remembered; the session is created once threads arrive too.
		openLocked();
		return;
	}
	api().updateTarget(g_session, nanos);
	ADPF_LOG("ADPF: deadline now %.2f ms", nanos / 1e6);
}


void adpfReportNanos(int64_t nanos) {
	if (nanos <= 0 || !g_active.load(std::memory_order_relaxed))
		return;

	// Each report is a round trip to the system, so do not send one per
	// callback when nothing has moved. A tenth of the deadline is the
	// granularity that matters: below that there is nothing for the governor
	// to act on, and at 48 kHz with a 256-frame block a report per callback
	// would be 187 of them a second.
	static int64_t lastReported = 0;
	int64_t delta = nanos > lastReported ? nanos - lastReported : lastReported - nanos;
	if (lastReported > 0 && delta * 10 < g_targetNanos)
		return;

	// try_lock, never lock: the render thread may be rebuilding the session,
	// and this thread must not wait for it. A dropped report costs nothing --
	// the next callback is a few milliseconds away.
	std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
	if (!lock.owns_lock() || !g_session)
		return;
	api().reportActual(g_session, nanos);
	lastReported = nanos;
}


void adpfClose() {
	if (!api().usable)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	closeLocked();
	g_tids.clear();
}


bool adpfActive() {
	return g_active.load(std::memory_order_relaxed);
}

} // namespace rackdroid
