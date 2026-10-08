/* The barrier Rack's engine threads meet at, twice per sample -- in place of
 * upstream's SpinBarrier and HybridBarrier (see the Engine.cpp patch in
 * native/CMakeLists.txt).
 *
 * Upstream's spin for as long as it takes. On a desktop the threads are peers
 * and that is the fastest thing to do. On Android the audio callback runs
 * SCHED_FIFO and the Workers cannot: when the two land on one core, the
 * callback spins waiting for the very Worker it is keeping off the CPU, and
 * nothing ends it but the kernel's real-time throttle. Measured: single
 * callbacks of 0.3 to 4 s on a Nothing A024, 1.6 s on an earlier build.
 * Pinning the callback to a core of its own (main_android.cpp) avoids the
 * meeting where the SoC allows it; it failed outright on that same A024, whose
 * prime core was halted at the time. This removes the cause instead of the
 * meeting: a thread spins for as long as a wait can honestly last, then
 * SLEEPS, which is the one thing that lets a lower-priority thread have the
 * core. It depends on no core numbering, cpuset or vendor scheduler.
 *
 * What it deliberately does NOT change is the healthy case. Between blocks
 * the Workers still spin for the next callback, as upstream's do, and only
 * sleep once the audio has plainly stopped. Letting them sleep after 100 us
 * was measured (75 s, same patch, 2 threads): the app's CPU fell from 250% to
 * 180% on a Lenovo TB-X306X and from 146% to 111% on an SM-S901E, but every
 * block then began by waking them, and the callback's load rose from 8% to
 * 16% of its deadline on the Lenovo and from 6% to 26% (peak 136%) on the
 * Samsung, whose callback is 2 ms long. Headroom is what this app is short of.
 *
 * Measured again on a heavy patch, where the Workers have little time between
 * blocks to sleep in (133 modules, 4 threads at 81% of the deadline, the same
 * SM-S901E, switched twice): the three Workers fell from 99.6% of a core each
 * to 87%, the whole app from 417% to 381%, and the callback's own CPU rose
 * from 82.7% to 84.3%. Nine per cent of the heat for a point and a half of
 * headroom; still not taken. The other 87% is the Workers spinning for each
 * other INSIDE the block, twice a sample, and no sleep fits in 21 us.
 */
#pragma once

#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <ctime>

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>


namespace rackdroid {


struct EngineBarrier {
	/** How long a wait may spin before it sleeps. One sample is 21 us at
	48 kHz and the whole engine has to fit in it, so no healthy wait gets near
	this; one that does is waiting for a thread that is not running. */
	static constexpr int64_t SPIN_NS = 100000;
	/** The same for a Worker waiting for the next block to begin: longer than
	the gap between two callbacks of the largest block (1024 frames, 23 ms), so
	it only sleeps when no callback is coming. Lower it to trade callback
	headroom for CPU and heat -- the numbers are at the top of this file. */
	static constexpr int64_t IDLE_SPIN_NS = 50000000;
	/** Loop iterations between looks at the clock (a vDSO read, no syscall). */
	static constexpr unsigned CLOCK_EVERY = 128;

	/** True for the barrier a block begins at -- the one Workers idle on. */
	const bool blockStart;
	explicit EngineBarrier(bool blockStart = false) : blockStart(blockStart) {}

	/** Set by each engine Worker for itself; the thread driving the engine
	(the audio callback) never is one. */
	static bool& isWorker() {
		static thread_local bool worker = false;
		return worker;
	}

	size_t threads = 0;
	std::atomic<uint32_t> count{0};
	std::atomic<uint32_t> phase{0};
	std::atomic<uint32_t> sleepers{0};

	void setThreads(size_t threads) {
		this->threads = threads;
	}

	/** Upstream's hint that the threads are about to idle. Nothing to do: every
	wait already sleeps once it has spun its budget. */
	void yield() {}

	void wait() {
		uint32_t current = phase.load();

		// Last to arrive: advance the phase, which releases the spinners, and
		// wake whoever went to sleep.
		if (count.fetch_add(1) + 1 >= threads) {
			count.store(0);
			phase.fetch_add(1);
			if (sleepers.load() > 0)
				futex(FUTEX_WAKE_PRIVATE, INT_MAX);
			return;
		}

		const int64_t budget = (blockStart && isWorker()) ? IDLE_SPIN_NS : SPIN_NS;
		int64_t start = 0;
		for (unsigned i = 1;; i++) {
			if (phase.load(std::memory_order_acquire) != current)
				return;
			pause();
			if (i % CLOCK_EVERY)
				continue;
			timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			int64_t now = (int64_t) ts.tv_sec * 1000000000LL + ts.tv_nsec;
			if (start == 0)
				start = now;
			else if (now - start > budget)
				break;
		}

		// Announce, then sleep only if the phase is still the one waited on:
		// FUTEX_WAIT compares in the kernel, so an advance that slips in
		// between is never slept through, and the last thread reads `sleepers`
		// only after advancing.
		sleepers.fetch_add(1);
		while (phase.load() == current)
			futex(FUTEX_WAIT_PRIVATE, (int) current);
		sleepers.fetch_sub(1);
	}

private:
	void futex(int op, int value) {
		syscall(SYS_futex, (uint32_t*) &phase, op, value, NULL, NULL, 0);
	}

	static void pause() {
#if defined(__aarch64__)
		__asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
		__builtin_ia32_pause();
#endif
	}
};


/** Run by each engine Worker as it starts: marks it as one and moves it off
the core it inherited. See engine_barrier.cpp. */
void engineWorkerStarted();

/** Which cores the Workers may use, one bit per core. Applied to Workers as
they start; the ones already running are moved by whoever calls this. */
void engineSetWorkerCpus(uint64_t mask);

/** engine_islands.inc: step independent parts of a patch a block at a time
(on by default), and how many parts the patch in the engine has -- 1 means
Rack's own per-sample path is in use. */
extern std::atomic<bool> engineIslandsOn;
extern std::atomic<int> engineIslandCount;
/** 1: the islands were timed against Rack's loop and are in use; -1: timed
and not faster here; 0: one island, or still being timed. */
extern std::atomic<int> engineIslandsUsed;
/** Islands stepped by a Worker rather than by the thread driving the engine,
ever. For the test: Workers that take none are Workers lost. */
extern std::atomic<uint32_t> engineIslandsByWorkers;


} // namespace rackdroid
