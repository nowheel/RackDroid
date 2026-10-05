/* Rack's engine mutex, with a way for the thread that drives the engine to
 * stand aside for a writer. Used in place of SharedMutex inside the patched
 * Engine.cpp only (see native/CMakeLists.txt).
 *
 * Engine::stepBlock holds the lock shared for a whole block; everything that
 * changes the patch -- adding a module, loading a file -- needs it exclusive.
 * A pthread rwlock lets readers in ahead of a waiting writer, which is fine
 * while the engine has time to spare between blocks. When a patch is too heavy
 * for the device it has none: the next block starts the instant the last one
 * ends, the lock is never free for long enough, and the writer waits for ever.
 * Measured on a Lenovo TB-X306X with a 133-module patch: File > Open sat for
 * more than a minute with "Engine fallback" and a Worker at 100% and the
 * interface thread asleep on the lock; on an SM-S901E Android declared the app
 * not responding. And since the autosave reopens the same patch, the user
 * could not get out by restarting either.
 *
 * Making the lock prefer writers would be the textbook answer and is not
 * safe here: modules call back into the engine from process(), taking the
 * shared lock again on a thread that already holds it, and a writer-preferring
 * rwlock deadlocks on exactly that. So the readers inside a block are left
 * alone, and only the top of stepBlock -- which holds nothing yet -- waits.
 */
#pragma once

#include <atomic>
#include <time.h>
#include <unistd.h>

#include <mutex.hpp>


namespace rackdroid {


struct EngineMutex : rack::SharedMutex {
	/** Writers waiting for the lock or holding it. */
	std::atomic<int> writers{0};

	void lock() {
		writers.fetch_add(1, std::memory_order_relaxed);
		rack::SharedMutex::lock();
	}

	bool try_lock() {
		if (!rack::SharedMutex::try_lock())
			return false;
		writers.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	void unlock() {
		rack::SharedMutex::unlock();
		writers.fetch_sub(1, std::memory_order_relaxed);
	}

	/** Called by Engine::stepBlock before it takes the lock. Bounded: a
	writer that never leaves must not stop the audio for good. A tenth of a
	second is far longer than any block and short enough to pass for the gap a
	patch change makes anyway.

	It does not return the moment the writer is gone, but once none has shown
	up for a while -- how long depends on what is going on. One change, a cable
	plugged in, gets a millisecond: the audio is playing and must not be made
	to click for it. A run of them is a patch being cleared or loaded, which
	takes the lock once per cable and per module; there the audio is already
	gone, and every block let through between two of them is time the user
	spends looking at a frozen rack. With the short wait alone, leaving a
	133-module patch on an overloaded TB-X306X took nine seconds, seven of
	them one block at a time. */
	void letWritersIn() {
		if (writers.load(std::memory_order_relaxed) == 0)
			return;
		int64_t now = nowMs();
		int64_t last = lastBusyMs.load(std::memory_order_relaxed);
		int run = (now - last < RUN_GAP_MS) ? busyRun.load(std::memory_order_relaxed) + 1 : 1;
		int quietNeeded = (run >= RUN_LENGTH) ? 80 : 4; // x 250 us
		int quiet = 0;
		for (int i = 0; i < 400 && quiet < quietNeeded; i++) {
			usleep(250);
			quiet = writers.load(std::memory_order_relaxed) > 0 ? 0 : quiet + 1;
		}
		busyRun.store(run, std::memory_order_relaxed);
		lastBusyMs.store(nowMs(), std::memory_order_relaxed);
	}

private:
	/** Blocks in a row, no further apart than this, that each found a writer:
	that many is a run. */
	static const int RUN_LENGTH = 3;
	static const int64_t RUN_GAP_MS = 50;
	std::atomic<int> busyRun{0};
	std::atomic<int64_t> lastBusyMs{0};

	static int64_t nowMs() {
		timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	}
};


} // namespace rackdroid
