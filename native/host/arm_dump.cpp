/* rack_ui_smoke --dump on the phone's own processor: the same runs
 * (stim_patch.hpp) from the arm64 librack_engine.so and
 * libplugin_fundamental.so an APK ships, as a plain executable run over adb.
 * Not built by CMake; scripts/arm_dump.sh builds, pushes and runs it. */
#include <dlfcn.h>
#include <unistd.h>

#include <asset.hpp>
#include <logger.hpp>
#include <system.hpp>
#include <history.hpp>
#include <patch.hpp>

#include "stim_patch.hpp"
#include "../port/engine_barrier.hpp"


int main(int argc, char* argv[]) {
	if (argc < 3)
		return 2;
	std::string dir = argv[1];
	settings::devMode = true;
	settings::headless = true;
	asset::systemDir = dir;
	asset::userDir = dir + "/user";
	system::createDirectories(asset::userDir);
	system::init();
	system::resetFpuFlags();
	asset::init();
	logger::init();
	random::init();
	settings::init();
	contextSet(new Context);
	APP->engine = new engine::Engine;
	APP->history = new history::State;
	APP->patch = new patch::Manager;
	rackdroid::engineIslandsOn = std::getenv("RACKDROID_DUMP_ISLANDS") != NULL;

	void* handle = dlopen("libplugin_fundamental.so", RTLD_NOW | RTLD_GLOBAL);
	void (*init)(plugin::Plugin*) = handle ? (void (*)(plugin::Plugin*)) dlsym(handle, "init") : NULL;
	json_error_t error;
	json_t* rootJ = json_load_file((dir + "/plugin.json").c_str(), 0, &error);
	if (!init || !rootJ) {
		std::printf("cannot load the plugin: %s\n", handle ? "no init() or no plugin.json" : dlerror());
		return 1;
	}
	plugin::Plugin* p = new plugin::Plugin;
	p->path = dir;
	p->handle = handle;
	p->fromJson(rootJ);
	init(p);
	plugin::plugins.push_back(p);

	dumpRuns(argv[2], argc > 3 ? argv[3] : NULL);
	std::printf("== dumped to %s\n", argv[2]);
	std::fflush(stdout);
	_exit(0);
}
