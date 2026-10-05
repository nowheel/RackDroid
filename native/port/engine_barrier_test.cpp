/* Stress check for engine_barrier.hpp, host or device:
 *   g++ -O2 -pthread native/port/engine_barrier_test.cpp -o /tmp/barrier_test && /tmp/barrier_test
 * Threads step through phases in lockstep the way Engine::stepFrame does; some
 * of them stall on purpose so the sleeping path and its wake-up are exercised.
 * A lost wake-up hangs it; a thread let through early trips the assert. */
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

#include "engine_barrier.hpp"

int main() {
	const int threads = 4;
	const int phases = 20000;
	rackdroid::EngineBarrier a(true), b;
	a.setThreads(threads);
	b.setThreads(threads);
	std::atomic<int> done{0};
	std::vector<std::thread> pool;
	for (int t = 0; t < threads; t++) {
		pool.emplace_back([&, t] {
			rackdroid::EngineBarrier::isWorker() = t > 0;
			for (int p = 0; p < phases; p++) {
				a.wait();
				// Everyone is in phase p now: nobody has finished it yet.
				assert(done.load() >= p * threads && done.load() < (p + 1) * threads);
				if ((p + t) % 997 == 0)
					std::this_thread::sleep_for(std::chrono::microseconds(400));
				done.fetch_add(1);
				b.wait();
				assert(done.load() >= (p + 1) * threads);
			}
		});
	}
	for (std::thread& th : pool)
		th.join();
	assert(done.load() == threads * phases);
	std::printf("ok: %d threads, %d phases\n", threads, phases);
	return 0;
}
