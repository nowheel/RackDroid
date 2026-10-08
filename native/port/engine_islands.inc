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
 * island's state stays on the core that ran it last.
 *
 * What a sample sees is unchanged: islands_test.cpp steps the same patches
 * both ways and compares every output voltage.
 *
 * A patch that is one island (everything cabled to one mixer) gains nothing
 * and takes Rack's own path, untouched.
 *
 * What does differ, by at most one block: Engine::getFrame() read from inside
 * process() stays at the block's first frame (ProcessArgs::frame is right),
 * and anything modules of different islands share behind the engine's back --
 * a parameter one writes on another, static storage -- is seen up to a block
 * early or late. Rack's own threads already gave no order within a frame.
 */

template <class TCables>
static void Engine_stepCablesOf(TCables& cablesOf);


struct EngineIsland {
	std::vector<Module*> modules;
	/** In the order of Internal::cables, which Engine_stepCablesOf relies on. */
	std::vector<Cable*> cables;
	/** Seconds a block, smoothed; written by the thread that ran it. */
	double cost = 0.0;
};


struct EngineIslands {
	Engine* owner = NULL;
	uint64_t signature = 0;
	int threads = 0;
	int blocksSinceBalance = 0;
	std::vector<EngineIsland> islands;
	/** Island indices for each thread, [0] being the caller of stepBlock. */
	std::vector<std::vector<int>> groups;
	std::map<Module*, int> islandOf;
	int frames = 0;
	int smoothIsland = -1;
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


/** Heaviest island first, each to the thread with the least so far. By what
the islands were measured to cost once they have run, by size before. */
static std::vector<std::vector<int>> EngineIslands_share(int threads, double* worst) {
	EngineIslands& s = engineIslands;
	auto costOf = [&](int i) {
		const EngineIsland& is = s.islands[i];
		return is.cost > 0.0 ? is.cost : 1e-6 * (double) is.modules.size();
	};
	std::vector<int> order(s.islands.size());
	for (size_t i = 0; i < order.size(); i++)
		order[i] = i;
	std::sort(order.begin(), order.end(), [&](int a, int b) {
		return costOf(a) > costOf(b);
	});
	std::vector<std::vector<int>> groups(threads);
	std::vector<double> load(threads, 0.0);
	for (int i : order) {
		int t = std::min_element(load.begin(), load.end()) - load.begin();
		groups[t].push_back(i);
		load[t] += costOf(i);
	}
	*worst = *std::max_element(load.begin(), load.end());
	return groups;
}


static double EngineIslands_worstNow() {
	EngineIslands& s = engineIslands;
	double worst = 0.0;
	for (const std::vector<int>& group : s.groups) {
		double load = 0.0;
		for (int i : group)
			load += s.islands[i].cost;
		worst = std::max(worst, load);
	}
	return worst;
}


/** One thread's islands for the block. */
static void EngineIslands_run(Engine* that, int threadId) {
	Engine::Internal* internal = that->internal;
	EngineIslands& s = engineIslands;
	if (threadId >= (int) s.groups.size())
		return;

	Module::ProcessArgs processArgs;
	processArgs.sampleRate = internal->sampleRate;
	processArgs.sampleTime = internal->sampleTime;

	for (int index : s.groups[threadId]) {
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
	}
}


/** Steps the whole block island by island; false when the patch is one island
(or this is switched off) and Rack's own loop must do it. Called by
Engine::stepBlock with the locks it holds and the Workers launched. */
static bool EngineIslands_stepBlock(Engine* that, int frames) {
	Engine::Internal* internal = that->internal;
	EngineIslands& s = engineIslands;
	if (!rackdroid::engineIslandsOn.load(std::memory_order_relaxed)) {
		s.owner = NULL;
		return false;
	}
	uint64_t signature = EngineIslands_signature(internal);
	if (s.owner != that || signature != s.signature) {
		s.owner = that;
		s.signature = signature;
		EngineIslands_build(internal);
		s.threads = 0;
	}
	if (s.islands.size() < 2)
		return false;

	int threads = std::max(internal->threadCount, 1);
	double worst = 0.0;
	if (threads != s.threads) {
		s.groups = EngineIslands_share(threads, &worst);
		s.threads = threads;
		s.blocksSinceBalance = 0;
	}
	else if (++s.blocksSinceBalance >= 256) {
		// Islands do not cost what their size says, and what they cost moves.
		// Moving one to another thread costs it its warm cache, so only for a
		// tenth or more off the heaviest thread.
		s.blocksSinceBalance = 0;
		std::vector<std::vector<int>> groups = EngineIslands_share(threads, &worst);
		if (worst < 0.9 * EngineIslands_worstNow())
			s.groups = groups;
	}

	s.frames = frames;
	s.smoothIsland = -1;
	if (internal->smoothModule) {
		auto it = s.islandOf.find(internal->smoothModule);
		if (it != s.islandOf.end())
			s.smoothIsland = it->second;
	}

	s.active.store(true, std::memory_order_release);
	internal->engineBarrier.wait();
	EngineIslands_run(that, 0);
	internal->workerBarrier.wait();
	s.active.store(false, std::memory_order_release);

	internal->frame += frames;
	return true;
}
