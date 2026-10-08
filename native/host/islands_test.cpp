/* Host check for port/engine_islands.inc:
 *   cmake -S native -B build-host-engine && cmake --build build-host-engine --target rack_islands_test -j8 && build-host-engine/rack_islands_test
 * Builds patches out of a small deterministic module, steps each of them with
 * Rack's own per-sample loop and with the islands, on one to four threads, and
 * compares every output voltage of every module at the end: they must be the
 * same bit for bit, with the thread count changed twice on the way. Then times
 * both on a patch of twelve islands. */
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <common.hpp>
#include <system.hpp>
#include <asset.hpp>
#include <logger.hpp>
#include <random.hpp>
#include <settings.hpp>
#include <context.hpp>
#include <engine/Engine.hpp>
#include <engine/Module.hpp>
#include <engine/Cable.hpp>

#include "../port/engine_barrier.hpp"

#include <window/Window.hpp>

using namespace rack;

// The engine subset built without a UI names this and nothing here calls it.
math::Vec window::Window::getSize() { return math::Vec(); }


/** A phase-modulated oscillator through a one-pole filter, with feedback from
its own inputs: what it puts out depends on every sample it was ever given, so
a cable stepped a frame early or late shows. Talks to its right neighbour
through the expander message, and has a smoothed parameter. */
struct Voice : engine::Module {
	float phase = 0.f;
	float lp = 0.f;
	float fromLeft = 0.f;
	float leftMessages[2] = {};
	int work;

	Voice(float pitch, int work) : work(work) {
		config(1, 2, 2, 0);
		configParam(0, 0.f, 10.f, pitch);
		leftExpander.producerMessage = &leftMessages[0];
		leftExpander.consumerMessage = &leftMessages[1];
	}

	void process(const ProcessArgs& args) override {
		float in = inputs[0].getVoltageSum() + 0.25f * inputs[1].getVoltage();
		if (leftExpander.module)
			fromLeft = *(float*) leftExpander.consumerMessage;
		float x = 0.f;
		// `work` turns of something a compiler cannot fold away.
		for (int i = 0; i < work; i++) {
			phase += (20.f + 30.f * params[0].getValue() + 3.f * in + fromLeft) * args.sampleTime;
			phase -= std::floor(phase);
			x += std::sin(6.2831853f * phase + 0.1f * i);
		}
		lp += 0.05f * (x / work - lp);
		outputs[0].setVoltage(5.f * lp);
		outputs[1].setVoltage(x / work + (args.frame % 7 == 0 ? 0.5f : 0.f));
		if (rightExpander.module && rightExpander.module->leftExpander.producerMessage) {
			*(float*) rightExpander.module->leftExpander.producerMessage = 0.01f * lp;
			rightExpander.module->leftExpander.messageFlipRequested = true;
		}
	}
};


struct Patch {
	engine::Engine* engine;
	std::vector<Voice*> voices;
};


static void cable(Patch& p, int from, int out, int to, int in) {
	engine::Cable* c = new engine::Cable;
	c->outputModule = p.voices[from];
	c->outputId = out;
	c->inputModule = p.voices[to];
	c->inputId = in;
	p.engine->addCable(c);
}


/** `islands` chains of `size` voices. Within a chain: a ring of cables, a
second cable stacked on one input, and two voices side by side as expanders.
`bridge` cables the first chain to the last, making the patch one island less. */
static Patch build(int islands, int size, int work, bool bridge) {
	Patch p;
	p.engine = new engine::Engine;
	APP->engine = p.engine;
	p.engine->setSampleRate(48000.f);
	for (int i = 0; i < islands; i++) {
		int base = p.voices.size();
		for (int j = 0; j < size; j++) {
			Voice* v = new Voice(1.f + 0.37f * i + 0.11f * j, work);
			p.voices.push_back(v);
			p.engine->addModule(v);
		}
		for (int j = 0; j < size; j++)
			cable(p, base + j, j % 2, base + (j + 1) % size, 0);
		if (size > 2) {
			cable(p, base, 1, base + 2, 0);
			cable(p, base + 2, 0, base + 1, 1);
		}
		if (size > 1) {
			p.voices[base]->rightExpander.moduleId = p.voices[base + 1]->id;
			p.voices[base + 1]->leftExpander.moduleId = p.voices[base]->id;
		}
	}
	if (bridge && islands > 1)
		cable(p, 0, 0, p.voices.size() - 1, 1);
	return p;
}


static std::vector<float> run(int islands, int size, bool bridge, bool on, int threads, int blocks, double* ms) {
	rackdroid::engineIslandsOn = on;
	settings::threadCount = threads;
	Patch p = build(islands, size, ms ? 40 : 2, bridge);
	auto start = std::chrono::steady_clock::now();
	for (int b = 0; b < blocks; b++) {
		// A knob turned while it plays, through the engine's smoothing.
		if (b == 3)
			p.engine->setParamSmoothValue(p.voices[p.voices.size() / 2], 0, 7.f);
		// The thread count changed under it, as the tuner does, and back:
		// the Workers have to be handed to Rack's barriers and taken again.
		if (!ms && b == 20)
			settings::threadCount = threads % 4 + 1;
		if (!ms && b == 35)
			settings::threadCount = threads;
		p.engine->stepBlock(96);
	}
	if (ms)
		*ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	std::vector<float> out;
	for (Voice* v : p.voices) {
		out.push_back(v->outputs[0].getVoltage());
		out.push_back(v->outputs[1].getVoltage());
		out.push_back(v->params[0].getValue());
	}
	out.push_back((float) p.engine->getFrame());
	int found = rackdroid::engineIslandCount;
	if (on && found != (bridge && islands > 1 ? islands - 1 : islands)) {
		std::printf("FAIL: %d chains%s read as %d islands\n", islands, bridge ? " bridged" : "", found);
		std::exit(1);
	}
	delete p.engine;
	APP->engine = NULL;
	return out;
}


int main() {
	asset::systemDir = RACKDROID_RACK_DIR;
	asset::userDir = system::getTempDirectory() + "/rackdroid-islands-test";
	system::createDirectories(asset::userDir);
	settings::devMode = true;
	settings::headless = true;
	system::init();
	system::resetFpuFlags();
	asset::init();
	logger::init();
	random::init();
	settings::init();
	contextSet(new Context);

	int checked = 0;
	struct Shape { int islands, size; bool bridge; };
	for (Shape s : {Shape{1, 6, false}, {2, 5, false}, {4, 7, false}, {4, 7, true}, {12, 3, false}, {5, 1, false}}) {
		std::vector<float> want = run(s.islands, s.size, s.bridge, false, 1, 50, NULL);
		for (int threads = 1; threads <= 4; threads++) {
			for (bool on : {false, true}) {
				std::vector<float> got = run(s.islands, s.size, s.bridge, on, threads, 50, NULL);
				if (got.size() != want.size() || std::memcmp(got.data(), want.data(), got.size() * sizeof(float)) != 0) {
					std::printf("FAIL: %d chains of %d%s, %d threads, islands %s: not what one thread of Rack's own loop gives\n",
						s.islands, s.size, s.bridge ? " bridged" : "", threads, on ? "on" : "off");
					return 1;
				}
				checked++;
			}
		}
	}
	std::printf("same output, bit for bit, in %d runs\n", checked);

	// What it is for. 12 islands of 44 voices, a second of audio in 96-frame blocks.
	for (int threads : {1, 2, 4}) {
		double off = 0.0, on = 0.0;
		run(12, 44, false, false, threads, 500, &off);
		uint32_t before = rackdroid::engineIslandsByWorkers;
		run(12, 44, false, true, threads, 500, &on);
		uint32_t byWorkers = rackdroid::engineIslandsByWorkers - before;
		// Same output proves nothing about who did the work: with the Workers
		// lost the caller does it all and the samples are still right.
		if (threads > 1 && byWorkers < 500) {
			std::printf("FAIL: %d threads, and the Workers stepped %u islands of some 5000\n", threads, byWorkers);
			return 1;
		}
		std::printf("12 islands of 44, %d thread%s: Rack's loop %.0f ms, islands %.0f ms for 1000 ms of audio (%.2fx)\n",
			threads, threads > 1 ? "s" : "", off, on, off / on);
	}
	{
		// And after the thread count has changed under it, which is where
		// they were lost.
		rackdroid::engineIslandsOn = true;
		settings::threadCount = 2;
		Patch p = build(12, 20, 40, false);
		for (int b = 0; b < 60; b++)
			p.engine->stepBlock(96);
		settings::threadCount = 4;
		for (int b = 0; b < 60; b++)
			p.engine->stepBlock(96);
		uint32_t before = rackdroid::engineIslandsByWorkers;
		bool used = rackdroid::engineIslandsUsed > 0;
		for (int b = 0; b < 200; b++)
			p.engine->stepBlock(96);
		uint32_t byWorkers = rackdroid::engineIslandsByWorkers - before;
		delete p.engine;
		APP->engine = NULL;
		if (used && byWorkers < 200) {
			std::printf("FAIL: after going from 2 threads to 4 the Workers stepped %u islands of 2400\n", byWorkers);
			return 1;
		}
		std::printf("after a change of thread count the Workers stepped %u islands of 2400%s\n", byWorkers, used ? "" : " (Rack's loop was chosen)");
	}
	std::printf("OK\n");
	return 0;
}
