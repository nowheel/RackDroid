/* The part of engine_barrier.hpp that needs one home: what an engine Worker
 * does as it starts. Lives in rack_engine, next to the patched Engine.cpp that
 * calls it; main_android.cpp, in another library, feeds it the core mask. */
#include <sched.h>

#include "engine_barrier.hpp"


namespace rackdroid {


/** engine_islands.inc: whether independent parts of a patch are stepped a block
at a time, and how many the patch in the engine has (1: Rack's own path). */
std::atomic<bool> engineIslandsOn{true};
std::atomic<int> engineIslandCount{0};
std::atomic<int> engineIslandsUsed{0};


/** Cores the Workers may run on, one bit each; 0 until someone has decided. */
static std::atomic<uint64_t> g_workerCpus{0};


void engineSetWorkerCpus(uint64_t mask) {
	g_workerCpus.store(mask, std::memory_order_relaxed);
}


/** Called by every engine Worker on its own thread, first thing.

A new thread inherits the affinity of the thread that creates it, and Rack
creates its Workers from inside Engine::stepBlock -- on the audio callback
thread, which is pinned to a single core so that no Worker can share it. So
every Worker launched after that pin was born ON the callback's core, and
stayed there until checkWorkerPriority() got to it half a second later. For
that half second the callback waited for a thread it was itself keeping off
the CPU, twice per sample. Seen on a Nothing A024 the moment the tuner tried
one thread fewer: "workers on cpu7", callbacks of 20.8 ms for 2 ms of audio
with exactly 192 sleeps each (96 samples, two barriers), 22 underruns in half
a second -- which the tuner read as too few threads, and every thread it added
was launched the same way: 2 to 7 threads and block 128 to 512 in eight
seconds, 800 underruns. Each change of thread count carried its own stall. */
void engineWorkerStarted() {
	EngineBarrier::isWorker() = true;
	uint64_t allowed = g_workerCpus.load(std::memory_order_relaxed);
	cpu_set_t set;
	CPU_ZERO(&set);
	long cores = sysconf(_SC_NPROCESSORS_CONF);
	for (long cpu = 0; cpu < cores && cpu < 64; cpu++) {
		// Nothing decided yet: any core at all beats the one inherited.
		if (allowed == 0 || ((allowed >> cpu) & 1))
			CPU_SET(cpu, &set);
	}
	sched_setaffinity(0, sizeof(set), &set);
}


} // namespace rackdroid
