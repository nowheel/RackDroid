/* One model played alone, the same way wherever this is compiled: by the
 * host harness (main_ui_host.cpp, --islands and --dump) and by vcv_dump.cpp,
 * which runs it inside VCV Rack's own desktop binary. Rack's API only. */
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <jansson.h>

#include <random.hpp>
#include <settings.hpp>
#include <context.hpp>
#include <engine/Engine.hpp>
#include <engine/Module.hpp>
#include <engine/Cable.hpp>
#include <dsp/minblep.hpp>
#include <dsp/fft.hpp>
#include <plugin.hpp>
#include <plugin/Plugin.hpp>
#include <plugin/Model.hpp>

using namespace rack;


/** What the comparison feeds every input from: audio, a slow curve, gates,
triggers, a ramp and a four-voice chord. Whole-number phases, one conversion
to float and one multiplication each: the same bits on any processor. It was
sines at first, and a phone's sin() is not a PC's -- the inputs already
differed in the last place before the module under test had done anything. */
struct Stim : engine::Module {
	uint32_t phase[6] = {};
	uint32_t step[6];
	Stim(int copy) {
		config(0, 0, 6, 0);
		// 220, 10, 20, 50, 3 and 30 Hz at 48 kHz, an eighth more per copy.
		static const uint32_t base[6] = {19685267, 894785, 1789570, 4473924, 268435, 2684354};
		for (int i = 0; i < 6; i++)
			step[i] = base[i] + base[i] / 8 * copy;
	}
	static float bipolar(uint32_t p) {
		return (float) (int32_t) p * (1.f / 2147483648.f);
	}
	void process(const ProcessArgs& args) override {
		for (int i = 0; i < 6; i++)
			phase[i] += step[i];
		uint32_t p = phase[0];
		uint32_t tri = p < 0x80000000u ? p : ~p;
		outputs[0].setVoltage(5.f * ((float) ((int32_t) tri - 0x40000000) * (1.f / 1073741824.f)));
		outputs[1].setVoltage(5.f * bipolar(phase[1]));
		outputs[2].setVoltage(phase[2] < 0x80000000u ? 10.f : 0.f);
		outputs[3].setVoltage(phase[3] < 0x0CCCCCCCu ? 10.f : 0.f);
		outputs[4].setVoltage((float) (phase[4] >> 8) * (10.f / 16777216.f));
		outputs[5].setChannels(4);
		for (int c = 0; c < 4; c++)
			outputs[5].setVoltage(2.f * bipolar(phase[5] * (uint32_t) (c + 1)), c);
	}
};

/** Where the comparison plugs every output: a module that finds an output
unconnected may not compute it at all. */
struct Sink : engine::Module {
	Sink(int inputs) {
		config(0, inputs, 0, 0);
	}
};

static const int STIM_COPIES = 4;
static const unsigned STIM_SEED = 12345;

/** Four copies of one model, each with a Stim cabled to all its inputs, a Sink
on all its outputs and nothing between the copies, in an engine of their own;
two copies with their knobs where the module puts them and two with every knob
somewhere else. Returns every output voltage and light after each block, for a
quarter of a second. `drew`: whether the thread's random generator, or the C
library's, was drawn from while it was stepped (a module that does gets other
numbers on another thread, in Rack's loop as well; seen only when this thread
stepped it, so ask on one thread). */
static std::vector<float> stimRun(plugin::Model* model, int threads, bool* drew) {
	settings::threadCount = threads;
	random::local().seed(0x52ac6b0dULL, 0x1dd6f00dULL);
	engine::Engine* old = APP->engine;
	engine::Engine* engine = new engine::Engine;
	APP->engine = engine;
	engine->setSampleRate(48000.f);
	std::vector<engine::Module*> modules, all;
	std::vector<engine::Cable*> cables;
	uint32_t lcg = 12345;
	for (int c = 0; c < STIM_COPIES; c++) {
		engine::Module* m = model->createModule();
		engine->addModule(m);
		all.push_back(m);
		modules.push_back(m);
		if (c >= 2) {
			for (int i = 0; i < (int) m->paramQuantities.size(); i++) {
				engine::ParamQuantity* q = m->paramQuantities[i];
				lcg = lcg * 1664525u + 1013904223u;
				if (!q || !std::isfinite(q->getMinValue()) || !std::isfinite(q->getMaxValue()))
					continue;
				float v = q->getMinValue() + (lcg >> 8) / 16777216.f * (q->getMaxValue() - q->getMinValue());
				engine->setParamValue(m, i, q->snapEnabled ? std::round(v) : v);
			}
		}
		if (!m->outputs.empty()) {
			Sink* sink = new Sink(m->outputs.size());
			engine->addModule(sink);
			all.push_back(sink);
			for (int i = 0; i < (int) m->outputs.size(); i++) {
				engine::Cable* cable = new engine::Cable;
				cable->outputModule = m;
				cable->outputId = i;
				cable->inputModule = sink;
				cable->inputId = i;
				engine->addCable(cable);
				cables.push_back(cable);
			}
		}
		if (m->inputs.empty())
			continue;
		Stim* stim = new Stim(c);
		engine->addModule(stim);
		all.push_back(stim);
		for (int i = 0; i < (int) m->inputs.size(); i++) {
			engine::Cable* cable = new engine::Cable;
			cable->outputModule = stim;
			cable->outputId = (i + c) % 6;
			cable->inputModule = m;
			cable->inputId = i;
			engine->addCable(cable);
				cables.push_back(cable);
		}
	}
	std::vector<float> out;
	std::srand(STIM_SEED);
	int firstRand = std::rand();
	// What the generator would give next if nothing draws from it while the
	// patch is stepped.
	random::Xoroshiro128Plus untouched = random::local();
	uint64_t next = untouched();
	// The C library's too, which is one for the whole process (and which
	// FrozenWasteland's ProbablyNote seeds from the clock as it is built).
	std::srand(STIM_SEED);
	// STIM_FRAMES=1 to see a difference sample by sample.
	int frames = std::getenv("STIM_FRAMES") ? std::atoi(std::getenv("STIM_FRAMES")) : 96;
	for (int b = 0; b < 120; b++) {
		engine->stepBlock(frames);
		for (engine::Module* m : modules) {
			for (engine::Output& o : m->outputs)
				out.insert(out.end(), o.voltages, o.voltages + engine::PORT_MAX_CHANNELS);
			for (engine::Light& l : m->lights)
				out.push_back(l.value);
		}
	}
	// Every thread has a generator of its own: a module that draws from it in
	// process() gets other numbers on another thread, in Rack's loop as well.
	// Seen here only when this thread stepped it, hence one thread.
	if (drew)
		*drew = random::local()() != next || std::rand() != firstRand;
	// As the rack does it, not left to the engine: a module that removes its
	// param handles as it dies (MIDI-Map) takes the engine's lock to do it.
	for (engine::Cable* cable : cables) {
		engine->removeCable(cable);
		delete cable;
	}
	for (engine::Module* m : all) {
		engine->removeModule(m);
		delete m;
	}
	delete engine;
	APP->engine = old;
	return out;
}


/** The Fundamental modules of a patch file and the cables between them (its
drums are RackDroid's own and its audio module wants a device), stepped for
two seconds on one thread; every output voltage of every module after each
block. */
static std::vector<float> patchRun(json_t* rootJ) {
	settings::threadCount = 1;
	random::local().seed(0x52ac6b0dULL, 0x1dd6f00dULL);
	std::srand(STIM_SEED);
	engine::Engine* old = APP->engine;
	engine::Engine* engine = new engine::Engine;
	APP->engine = engine;
	engine->setSampleRate(48000.f);
	std::vector<engine::Module*> all;
	std::vector<engine::Cable*> cables;
	size_t i;
	json_t* j;
	json_array_foreach(json_object_get(rootJ, "modules"), i, j) {
		const char* pluginSlug = json_string_value(json_object_get(j, "plugin"));
		const char* modelSlug = json_string_value(json_object_get(j, "model"));
		if (!pluginSlug || !modelSlug || std::string(pluginSlug) != "Fundamental")
			continue;
		plugin::Model* model = plugin::getModel(pluginSlug, modelSlug);
		if (!model)
			continue;
		engine::Module* m = model->createModule();
		m->fromJson(j);
		engine->addModule(m);
		all.push_back(m);
	}
	json_array_foreach(json_object_get(rootJ, "cables"), i, j) {
		engine::Module* from = engine->getModule(json_integer_value(json_object_get(j, "outputModuleId")));
		engine::Module* to = engine->getModule(json_integer_value(json_object_get(j, "inputModuleId")));
		if (!from || !to)
			continue;
		engine::Cable* cable = new engine::Cable;
		cable->outputModule = from;
		cable->outputId = json_integer_value(json_object_get(j, "outputId"));
		cable->inputModule = to;
		cable->inputId = json_integer_value(json_object_get(j, "inputId"));
		engine->addCable(cable);
		cables.push_back(cable);
	}
	std::vector<float> out;
	for (int b = 0; b < 1000; b++) {
		engine->stepBlock(96);
		for (engine::Module* m : all)
			for (engine::Output& o : m->outputs)
				out.insert(out.end(), o.voltages, o.voltages + engine::PORT_MAX_CHANNELS);
	}
	for (engine::Cable* cable : cables) {
		engine->removeCable(cable);
		delete cable;
	}
	for (engine::Module* m : all) {
		engine->removeModule(m);
		delete m;
	}
	delete engine;
	APP->engine = old;
	return out;
}


/** Every Fundamental model through stimRun on one thread, then the patch if
one is given, written as "name count\n" and that many raw floats each. */
static void dumpRuns(const char* path, const char* patchPath) {
	FILE* f = std::fopen(path, "wb");
	if (!f)
		return;
	for (plugin::Plugin* p : plugin::plugins) {
		if (p->slug != "Fundamental")
			continue;
		for (plugin::Model* model : p->models) {
			std::fprintf(stderr, "## dump %s\n", model->slug.c_str());
			std::vector<float> out = stimRun(model, 1, NULL);
			std::fprintf(f, "%s %zu\n", model->slug.c_str(), out.size());
			std::fwrite(out.data(), sizeof(float), out.size(), f);
		}
	}
	{
		// What the library itself computes for the oscillators: the band-
		// limited step every saw and square edge is built from.
		std::vector<float> out(2 * 16 * 16);
		dsp::minBlepImpulse(16, 16, out.data());
		std::fprintf(f, "minBLEP-table %zu\n", out.size());
		std::fwrite(out.data(), sizeof(float), out.size(), f);
	}
	{
		// And its FFT, which the wavetable oscillator builds its band-limited
		// tables with: a transform and back of a whole-number ramp.
		const int n = 1024;
		float* in = (float*) pffft_aligned_malloc(sizeof(float) * n);
		float* freq = (float*) pffft_aligned_malloc(sizeof(float) * 2 * n);
		float* back = (float*) pffft_aligned_malloc(sizeof(float) * n);
		for (int i = 0; i < n; i++)
			in[i] = (float) ((i * 37) % 251 - 125) * (1.f / 128.f);
		dsp::RealFFT fft(n);
		fft.rfft(in, freq);
		fft.irfft(freq, back);
		std::vector<float> out(freq, freq + 2 * n);
		out.insert(out.end(), back, back + n);
		std::fprintf(f, "FFT %zu\n", out.size());
		std::fwrite(out.data(), sizeof(float), out.size(), f);
	}
	if (patchPath) {
		json_error_t error;
		json_t* rootJ = json_load_file(patchPath, 0, &error);
		if (rootJ) {
			std::vector<float> out = patchRun(rootJ);
			std::fprintf(f, "PATCH %zu\n", out.size());
			std::fwrite(out.data(), sizeof(float), out.size(), f);
			json_decref(rootJ);
		}
	}
	std::fclose(f);
}
