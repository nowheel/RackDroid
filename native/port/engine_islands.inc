/* Independent parts of a patch, each run for a whole block by one thread.
 *
 * Included into the patched copy of Engine.cpp (native/CMakeLists.txt), inside
 * namespace rack::engine, just before Engine_stepWorker.
 *
 * Rack's threads share the modules one at a time, first come first served, and
 * meet at two barriers every sample: 192 meetings for a 96-frame callback, and
 * a module's state on a different core every sample. Measured on a Nothing
 * A024 with a 177-module patch, the same count read 72-112% of the deadline on
 * one thread and 62-68% on four: three more cores bought next to nothing.
 *
 * A cable is the only thing that carries a sample from one module to another,
 * so two groups of modules with no cable between them (and not side by side,
 * where they could be talking as expanders) cannot tell in what order they are
 * run. Such a group -- an island -- is stepped here for the whole block by one
 * thread: its modules, then its cables, frame after frame, with nobody to wait
 * for. The threads meet once per block instead of twice per sample, and an
 * island's state stays on one core for the length of a block.
 *
 * What a sample sees is unchanged: islands_test.cpp steps the same patches
 * both ways and compares every output voltage.
 *
 * A patch that is one island (everything cabled to one mixer) gains nothing
 * and takes Rack's own path, untouched.
 *
 * Nor is it always a gain with several: a 45-module island beside a 1-module
 * one leaves one thread with all the work, where Rack's loop would have shared
 * it (a TB-X306X went from 42% of its deadline to 59%). Since both ways give
 * the same samples, the engine can change between them from one block to the
 * next with nothing to hear, so it times a few dozen blocks of each whenever
 * the patch or the thread count changes and keeps the quicker.
 *
 * What does differ, by at most one block: Engine::getFrame() read from inside
 * process() stays at the block's first frame (ProcessArgs::frame is right),
 * and anything modules of different islands share behind the engine's back --
 * a parameter one writes on another, static storage -- is seen up to a block
 * early or late. Rack's own threads already gave no order within a frame.
 */

template <class TCables>
static void Engine_stepCablesOf(TCables& cablesOf);
static void Engine_stepFrame(Engine* that);


struct EngineIsland {
	std::vector<Module*> modules;
	/** In the order of Internal::cables, which Engine_stepCablesOf relies on. */
	std::vector<Cable*> cables;
	/** Seconds a block, smoothed; written by the thread that ran it. */
	double cost = 0.0;
};


/** The trial is counted in audio, not in blocks: a block is 96 frames on one
phone and 960 on another (a TB-X306X, where a trial of 112 blocks took over
two seconds and was still running, half of it the slow way, while the silent
measurement read the count).

TRIAL_WARM_SEC as islands before anything is timed: Workers just launched have
neither their priority nor their cores yet (main_android.cpp gives them within
a twentieth of a second), and timed in that state the islands lost to Rack's
loop at thread counts where they are twice as fast. Then TRIAL_SEC each way,
four blocks at least, the first TRIAL_SKIP_SEC of each left out. */
static constexpr double TRIAL_WARM_SEC = 0.15;
static constexpr double TRIAL_SEC = 0.1;
static constexpr double TRIAL_SKIP_SEC = 0.02;


struct EngineIslands {
	Engine* owner = NULL;
	uint64_t signature = 0;
	int threads = 0;
	int blocksSinceBalance = 0;
	std::vector<EngineIsland> islands;
	/** The islands, heaviest first, and the next one nobody has taken yet this
	block. Taken, not handed out: see EngineIslands_run. */
	std::vector<int> order;
	std::atomic<int> next{0};
	/** How many of `order` may be taken: 0 between blocks, so that a Worker
	waking late finds the door shut rather than half a block being prepared. */
	std::atomic<int> open{0};
	/** Islands of this block not yet finished; the block ends at 0. */
	std::atomic<uint32_t> pending{0};
	std::atomic<bool> callerSleeps{false};
	/** Goes up once a block, and to send the Workers back to Rack's loop. */
	std::atomic<uint32_t> generation{0};
	std::atomic<uint32_t> sleepers{0};
	std::atomic<bool> leave{false};
	/** Workers that have arrived in EngineIslands_work since they were sent. */
	std::atomic<int> entered{0};
	/** The Workers are in EngineIslands_work, not at Rack's barriers. */
	bool parked = false;
	/** When this block opened, and how long its audio lasts (seconds). */
	double blockStart = 0.0;
	double blockTime = 0.0;
	/** Each core's capacity as the kernel rates it, 0 where it does not say,
	and the largest of them. Read once. */
	int capacity[64] = {};
	int capacityMax = 0;
	bool capacityRead = false;
	std::map<Module*, int> islandOf;
	int frames = 0;
	int smoothIsland = -1;
	/** Which way is faster for this patch on this many threads is measured,
	not guessed: see TRIAL_SEC. In frames of audio: negative while warming,
	[0, trialPhase) islands, [trialPhase, 2 trialPhase) Rack's loop, done. */
	int64_t trialAt = -1;
	int64_t trialPhase = 1 << 30;
	double trialTime[2] = {};
	int64_t trialFrames[2] = {};
	bool useIslands = true;
	std::atomic<bool> active{false};
};
static EngineIslands engineIslands;


/** Changes whenever a module or cable comes or goes or two modules become
neighbours. Read every block, under the engine's shared lock.
ponytail: a walk of every module and cable per block (7 us for 529 modules and
1010 cables); a counter bumped by add/remove would be free but needs four more
anchors in upstream's file. */
static uint64_t EngineIslands_signature(Engine::Internal* internal) {
	uint64_t h = 1469598103934665603ull;
	auto mix = [&](uint64_t v) {
		h ^= v;
		h *= 1099511628211ull;
	};
	for (Module* m : internal->modules) {
		mix((uintptr_t) m);
		mix((uintptr_t) m->leftExpander.module);
		mix((uintptr_t) m->rightExpander.module);
	}
	for (Cable* c : internal->cables) {
		mix((uintptr_t) c);
		mix((uintptr_t) c->inputModule);
		mix((uintptr_t) c->outputModule);
		mix(((uint64_t) (uint32_t) c->inputId << 32) | (uint32_t) c->outputId);
	}
	return h;
}


static void EngineIslands_build(Engine::Internal* internal) {
	EngineIslands& s = engineIslands;
	if (!s.capacityRead) {
		s.capacityRead = true;
		for (int cpu = 0; cpu < 64; cpu++) {
			char path[64];
			std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
			FILE* f = std::fopen(path, "r");
			if (!f)
				break;
			if (std::fscanf(f, "%d", &s.capacity[cpu]) != 1)
				s.capacity[cpu] = 0;
			std::fclose(f);
			s.capacityMax = std::max(s.capacityMax, s.capacity[cpu]);
		}
	}
	s.islands.clear();
	s.islandOf.clear();
	int n = internal->modules.size();
	std::map<Module*, int> index;
	for (int i = 0; i < n; i++)
		index[internal->modules[i]] = i;
	std::vector<int> parent(n);
	for (int i = 0; i < n; i++)
		parent[i] = i;
	auto find = [&](int x) {
		while (parent[x] != x) {
			parent[x] = parent[parent[x]];
			x = parent[x];
		}
		return x;
	};
	auto join = [&](Module* a, Module* b) {
		auto ia = index.find(a);
		auto ib = index.find(b);
		if (ia == index.end() || ib == index.end())
			return;
		int ra = find(ia->second);
		int rb = find(ib->second);
		if (ra != rb)
			parent[ra] = rb;
	};
	for (Cable* c : internal->cables)
		join(c->inputModule, c->outputModule);
	// Neighbours may be expanders of one another, by message or by reaching
	// straight into each other. There is no telling from here, so they stay
	// together.
	for (Module* m : internal->modules) {
		if (m->leftExpander.module)
			join(m, m->leftExpander.module);
		if (m->rightExpander.module)
			join(m, m->rightExpander.module);
	}
	std::map<int, int> islandOfRoot;
	for (int i = 0; i < n; i++) {
		int root = find(i);
		auto it = islandOfRoot.find(root);
		if (it == islandOfRoot.end()) {
			it = islandOfRoot.emplace(root, (int) s.islands.size()).first;
			s.islands.emplace_back();
		}
		s.islands[it->second].modules.push_back(internal->modules[i]);
		s.islandOf[internal->modules[i]] = it->second;
	}
	for (Cable* c : internal->cables) {
		auto it = s.islandOf.find(c->inputModule);
		if (it != s.islandOf.end())
			s.islands[it->second].cables.push_back(c);
	}
	rackdroid::engineIslandCount.store((int) s.islands.size(), std::memory_order_relaxed);
}


/** Heaviest first, by what the islands were measured to cost once they have
run and by size before: the light ones are then the last to be taken, and the
block ends when the last one does. */
static void EngineIslands_sort() {
	EngineIslands& s = engineIslands;
	auto costOf = [&](int i) {
		const EngineIsland& is = s.islands[i];
		return is.cost > 0.0 ? is.cost : 1e-6 * (double) is.modules.size();
	};
	s.order.resize(s.islands.size());
	for (size_t i = 0; i < s.order.size(); i++)
		s.order[i] = i;
	std::sort(s.order.begin(), s.order.end(), [&](int a, int b) {
		return costOf(a) > costOf(b);
	});
}


static void EngineIslands_futex(std::atomic<uint32_t>& word, int op, uint32_t value) {
	syscall(SYS_futex, (uint32_t*) &word, op, value, NULL, NULL, 0);
}


/** What one thread does of the block: the next island nobody has taken, until
there is none.

They were shared out beforehand at first, each thread its own. A Nothing A024
then put two Workers on one core for a minute: each waited a scheduler's turn
(16 ms) for the other, which was only spinning, and the callback waited 16 ms
a block for islands that any idle thread could have run -- 800% of the
deadline, 530 underruns a second. Taken as they come, a thread that is not
running takes nothing and holds nobody up, and a slow core does less. */
static void EngineIslands_run(Engine* that, int threadId) {
	Engine::Internal* internal = that->internal;
	EngineIslands& s = engineIslands;
	Module::ProcessArgs processArgs;
	processArgs.sampleRate = internal->sampleRate;
	processArgs.sampleTime = internal->sampleTime;

	while (true) {
		// A Worker on a small core stands by until half the block's time is
		// gone. The block ends when its last island does, and an island a
		// small core has taken takes three times as long: on a OnePlus 8T
		// four islands on five threads read 76% of the deadline, and on a
		// Galaxy A52s two threads read worse than one. If the big cores can
		// do it all in time the small one is not wanted; if they cannot, it
		// is better than nothing.
		if (threadId != 0 && s.capacityMax > 0) {
			int cpu = sched_getcpu();
			if (cpu >= 0 && cpu < 64 && s.capacity[cpu] > 0 && 2 * s.capacity[cpu] < s.capacityMax) {
				bool wanted = false;
				while (s.next.load(std::memory_order_acquire) < s.open.load(std::memory_order_acquire)) {
					if (system::getTime() - s.blockStart > 0.5 * s.blockTime) {
						wanted = true;
						break;
					}
#if defined(__aarch64__)
					__asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
					__builtin_ia32_pause();
#endif
				}
				if (!wanted)
					break;
			}
		}
		// Never past `open`, and never while it is 0: a thread that slept
		// through the end of its block must not move `next` in the one being
		// prepared.
		int taken = s.next.load(std::memory_order_acquire);
		while (taken < s.open.load(std::memory_order_acquire)
				&& !s.next.compare_exchange_weak(taken, taken + 1, std::memory_order_acq_rel)) {}
		if (taken >= s.open.load(std::memory_order_acquire))
			break;
		int index = s.order[taken];
		EngineIsland& island = s.islands[index];
		double startTime = system::getTime();
		for (int f = 0; f < s.frames; f++) {
			// Param smoothing, as Engine_stepFrame does it, by the thread that
			// has the module.
			if (index == s.smoothIsland) {
				Module* smoothModule = internal->smoothModule;
				if (smoothModule) {
					int smoothParamId = internal->smoothParamId;
					float smoothValue = internal->smoothValue;
					Param* smoothParam = &smoothModule->params[smoothParamId];
					float value = smoothParam->value;
					const float smoothLambda = 60.f;
					float newValue = value + (smoothValue - value) * smoothLambda * internal->sampleTime;
					if (value == newValue) {
						smoothParam->setValue(smoothValue);
						internal->smoothModule = NULL;
						internal->smoothParamId = 0;
					}
					else {
						smoothParam->setValue(newValue);
					}
				}
			}

			processArgs.frame = internal->frame + f;
			for (Module* module : island.modules)
				module->doProcess(processArgs);

			Engine_stepCablesOf(island.cables);

			for (Module* module : island.modules) {
				if (module->leftExpander.messageFlipRequested) {
					std::swap(module->leftExpander.producerMessage, module->leftExpander.consumerMessage);
					module->leftExpander.messageFlipRequested = false;
				}
				if (module->rightExpander.messageFlipRequested) {
					std::swap(module->rightExpander.producerMessage, module->rightExpander.consumerMessage);
					module->rightExpander.messageFlipRequested = false;
				}
			}
		}
		double took = system::getTime() - startTime;
		island.cost = island.cost > 0.0 ? island.cost + 0.1 * (took - island.cost) : took;
		if (threadId != 0)
			rackdroid::engineIslandsByWorkers.fetch_add(1, std::memory_order_relaxed);
		if (s.pending.fetch_sub(1, std::memory_order_acq_rel) == 1
				&& s.callerSleeps.load(std::memory_order_acquire))
			EngineIslands_futex(s.pending, FUTEX_WAKE_PRIVATE, 1);
	}
}


/** Where a Worker lives while the islands are in use: it takes islands when a
block opens and waits for the next, and the thread stepping the engine waits
for islands, never for Workers.

With Rack's two barriers every Worker had to report at the start and the end
of every block whether or not it had done anything, and one that was not
being run -- two of them given one core, or the system's own interface taking
theirs while the app switcher was open -- held the callback for a scheduler's
turn: callbacks of 16 ms with 1.3 ms of work in them, on a Nothing A024, block
after block. Here a Worker that is not running is simply not there.

It spins between blocks as Rack's Workers do at their barrier, for the same
measured reason (engine_barrier.hpp): a core left idle slows down. */
static void EngineIslands_work(Engine* that, int threadId) {
	EngineIslands& s = engineIslands;
	uint32_t seen = s.generation.load(std::memory_order_acquire) - 1;
	s.entered.fetch_add(1, std::memory_order_acq_rel);
	while (true) {
		int64_t start = 0;
		for (unsigned i = 1; s.generation.load(std::memory_order_acquire) == seen; i++) {
#if defined(__aarch64__)
			__asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
			__builtin_ia32_pause();
#endif
			if (i % rackdroid::EngineBarrier::CLOCK_EVERY)
				continue;
			timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			int64_t now = (int64_t) ts.tv_sec * 1000000000LL + ts.tv_nsec;
			if (start == 0)
				start = now;
			else if (now - start > rackdroid::EngineBarrier::IDLE_SPIN_NS) {
				s.sleepers.fetch_add(1);
				while (s.generation.load(std::memory_order_acquire) == seen)
					EngineIslands_futex(s.generation, FUTEX_WAIT_PRIVATE, seen);
				s.sleepers.fetch_sub(1);
			}
		}
		seen = s.generation.load(std::memory_order_acquire);
		if (s.leave.load(std::memory_order_acquire))
			return;
		EngineIslands_run(that, threadId);
	}
}


/** Sends the Workers back to Rack's barriers, where its own loop and
Engine_relaunchWorkers expect to find them. Between blocks only. */
static void EngineIslands_leave(Engine* that) {
	EngineIslands& s = engineIslands;
	if (!s.parked || s.owner != that)
		return;
	s.leave.store(true, std::memory_order_release);
	s.generation.fetch_add(1, std::memory_order_acq_rel);
	EngineIslands_futex(s.generation, FUTEX_WAKE_PRIVATE, INT_MAX);
	// Each returns from Engine_stepWorker into EngineWorker::run, which meets
	// the caller here as it does at the end of any frame.
	that->internal->workerBarrier.wait();
	s.leave.store(false, std::memory_order_release);
	s.parked = false;
}


/** Steps the whole block island by island; false when the patch is one island
(or this is switched off) and Rack's own loop must do it. Called by
Engine::stepBlock with the locks it holds and the Workers launched. */
static bool EngineIslands_stepBlock(Engine* that, int frames) {
	Engine::Internal* internal = that->internal;
	EngineIslands& s = engineIslands;
	if (!rackdroid::engineIslandsOn.load(std::memory_order_relaxed)) {
		EngineIslands_leave(that);
		s.owner = NULL;
		return false;
	}
	uint64_t signature = EngineIslands_signature(internal);
	bool rebuilt = false;
	if (s.owner != that || signature != s.signature) {
		rebuilt = true;
		// Parked Workers touch no island between blocks, so the islands may
		// be rebuilt under them.
		s.owner = that;
		s.signature = signature;
		EngineIslands_build(internal);
		s.threads = 0;
	}
	if (s.islands.size() < 2) {
		EngineIslands_leave(that);
		rackdroid::engineIslandsUsed.store(0, std::memory_order_relaxed);
		return false;
	}

	int threads = std::max(internal->threadCount, 1);
	if (threads != s.threads) {
		// Timed again on another thread count only if Rack's loop won the
		// last time: islands that were the quicker on some count are the
		// quicker on more threads, and each trial is a quarter of a second of
		// the silent measurement, half of it spent the slow way.
		bool keep = s.threads > 0 && !rebuilt && s.trialAt >= 2 * s.trialPhase && s.useIslands;
		s.threads = threads;
		s.blocksSinceBalance = 0;
		if (!keep) {
			s.trialAt = -(int64_t) (TRIAL_WARM_SEC * internal->sampleRate);
			s.trialPhase = std::max((int64_t) (TRIAL_SEC * internal->sampleRate), (int64_t) 4 * frames);
			s.trialTime[0] = s.trialTime[1] = 0.0;
			s.trialFrames[0] = s.trialFrames[1] = 0;
			rackdroid::engineIslandsUsed.store(0, std::memory_order_relaxed);
		}
		EngineIslands_sort();
	}
	else if (++s.blocksSinceBalance >= 256) {
		// Islands do not cost what their size says, and what they cost moves.
		s.blocksSinceBalance = 0;
		EngineIslands_sort();
	}

	bool decided = s.trialAt >= 2 * s.trialPhase;
	bool islandsNow = decided ? s.useIslands : s.trialAt < s.trialPhase;
	double trialStart = decided ? 0.0 : system::getTime();
	if (!islandsNow) {
		EngineIslands_leave(that);
		for (int f = 0; f < frames; f++)
			Engine_stepFrame(that);
	}
	else {
	s.frames = frames;
	s.smoothIsland = -1;
	if (internal->smoothModule) {
		auto it = s.islandOf.find(internal->smoothModule);
		if (it != s.islandOf.end())
			s.smoothIsland = it->second;
	}

	s.blockStart = system::getTime();
	s.blockTime = frames * (double) internal->sampleTime;
	int count = s.order.size();
	s.pending.store(count, std::memory_order_release);
	s.next.store(0, std::memory_order_release);
	s.open.store(count, std::memory_order_release);
	s.generation.fetch_add(1, std::memory_order_acq_rel);
	if (!s.parked) {
		// Out of Rack's loop, once: the Workers are waiting at its barrier,
		// and Engine_stepWorker sends them to EngineIslands_work to stay.
		//
		// `active` stays up until every one of them has read it. Taken down
		// as soon as the barrier opened, a Worker that looked a moment later
		// went into Rack's own step instead and then sat at Rack's second
		// barrier for good: the engine ran on, on one thread, at 200% of the
		// deadline whatever the thread count (TB-X306X, OnePlus 8T).
		s.entered.store(0, std::memory_order_release);
		s.active.store(true, std::memory_order_release);
		internal->engineBarrier.wait();
		while (s.entered.load(std::memory_order_acquire) < internal->threadCount - 1) {
#if defined(__aarch64__)
			__asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
			__builtin_ia32_pause();
#endif
		}
		s.active.store(false, std::memory_order_release);
		s.parked = true;
	}
	else if (s.sleepers.load(std::memory_order_acquire) > 0) {
		EngineIslands_futex(s.generation, FUTEX_WAKE_PRIVATE, INT_MAX);
	}
	EngineIslands_run(that, 0);

	// For the islands a Worker is still in the middle of. Briefly spinning:
	// they are a block's worth of work at most.
	for (unsigned i = 1; s.pending.load(std::memory_order_acquire) != 0; i++) {
		if (i < 20000)
			continue;
		s.callerSleeps.store(true, std::memory_order_release);
		uint32_t left;
		while ((left = s.pending.load(std::memory_order_acquire)) != 0)
			EngineIslands_futex(s.pending, FUTEX_WAIT_PRIVATE, left);
		s.callerSleeps.store(false, std::memory_order_release);
	}
	s.open.store(0, std::memory_order_release);

	internal->frame += frames;
	}
	if (!decided) {
		if (s.trialAt >= 0) {
			int which = s.trialAt >= s.trialPhase;
			if (s.trialAt - which * s.trialPhase >= (int64_t) (TRIAL_SKIP_SEC * internal->sampleRate)) {
				s.trialTime[which] += system::getTime() - trialStart;
				s.trialFrames[which] += frames;
			}
		}
		s.trialAt += frames;
		if (s.trialAt >= 2 * s.trialPhase) {
			// Per frame, since the two halves need not hold the same number
			// of blocks. Rack's own loop on a tie: it is the one everybody
			// else runs.
			double islandsCost = s.trialFrames[0] > 0 ? s.trialTime[0] / s.trialFrames[0] : 0.0;
			double rackCost = s.trialFrames[1] > 0 ? s.trialTime[1] / s.trialFrames[1] : 0.0;
			s.useIslands = islandsCost > 0.0 && rackCost > 0.0 && islandsCost < 0.95 * rackCost;
			rackdroid::engineIslandsUsed.store(s.useIslands ? 1 : -1, std::memory_order_relaxed);
		}
	}
	return true;
}
