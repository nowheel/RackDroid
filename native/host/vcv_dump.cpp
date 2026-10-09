/* The other side of rack_ui_smoke --dump: the same runs (stim_patch.hpp)
 * inside VCV Rack's own desktop binary. Not built by CMake -- it links the
 * libRack.so of a Rack download and is compiled with the Rack SDK's flags:
 *   g++ -std=c++11 -O3 -funsafe-math-optimizations -march=nehalem -fPIC \
 *     -I$SDK/include -I$SDK/dep/include native/host/vcv_dump.cpp \
 *     -o vcv_dump $RACK/libRack.so -Wl,-rpath,$RACK
 *   ./vcv_dump $RACK <user dir with plugins/Fundamental> out.bin [patch.json]
 * The user dir's plugins/Fundamental is what gets compared: the plugin as
 * built with the SDK, or the one unpacked from the download. */
#include <unistd.h>
#include <execinfo.h>
#include <csignal>

#include <asset.hpp>
#include <logger.hpp>
#include <system.hpp>
#include <audio.hpp>
#include <midi.hpp>
#include <history.hpp>
#include <patch.hpp>

#include "stim_patch.hpp"


static void crashed(int) {
	void* frames[40];
	backtrace_symbols_fd(frames, backtrace(frames, 40), 2);
	_exit(3);
}


int main(int argc, char* argv[]) {
	signal(SIGSEGV, crashed);
	// --tables: port/minblep_vcv.inc, the step tables as this binary computes
	// them, every float in hexadecimal so that none is rounded on the way.
	if (argc > 1 && std::string(argv[1]) == "--tables") {
		static const int sizes[][2] = {{16, 16}, {16, 32}};
		for (auto& size : sizes) {
			int n = 2 * size[0] * size[1];
			std::vector<float> t(n);
			dsp::minBlepImpulse(size[0], size[1], t.data());
			std::printf("static const float minBlepVcv_%d_%d[%d] = {\n", size[0], size[1], n);
			for (int i = 0; i < n; i++)
				std::printf("%s%a,%s", i % 6 == 0 ? "\t" : " ", (double) t[i], i % 6 == 5 || i == n - 1 ? "\n" : "");
			std::printf("};\n");
		}
		return 0;
	}
	if (argc < 4)
		return 2;
	settings::devMode = true;
	settings::headless = true;
	asset::systemDir = argv[1];
	asset::userDir = argv[2];
	system::init();
	asset::init();
	logger::init();
	random::init();
	settings::init();
	audio::init();
	midi::init();
	plugin::init();
	contextSet(new Context);
	APP->engine = new engine::Engine;
	// Fundamental's VCO2 asks for the patch's storage directory as it is added.
	APP->history = new history::State;
	APP->patch = new patch::Manager;
	dumpRuns(argv[3], argc > 4 ? argv[4] : NULL);
	std::printf("== dumped to %s\n", argv[3]);
	// No teardown: the process ends here and nothing is saved.
	std::fflush(stdout);
	_exit(0);
}
