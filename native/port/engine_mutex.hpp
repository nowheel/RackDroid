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
#include <unistd.h>

#include <mutex.hpp>


namespace rackdroid {


struct EngineMutex : rack::SharedMutex {
	std::atomic<int> writersWaiting{0};

	void lock() {
		writersWaiting.fetch_add(1, std::memory_order_relaxed);
		rack::SharedMutex::lock();
		writersWaiting.fetch_sub(1, std::memory_order_relaxed);
	}

	/** Called by Engine::stepBlock before it takes the lock. Bounded: a
	writer that never arrives must not stop the audio for good. A tenth of a
	second is far longer than any block and short enough to pass for the gap a
	patch change makes anyway. */
	void letWritersIn() {
		for (int i = 0; i < 400 && writersWaiting.load(std::memory_order_relaxed) > 0; i++)
			usleep(250);
	}
};


} // namespace rackdroid
