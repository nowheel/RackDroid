/* Host reproduction of the phase-2 Android startup (rack_ui_smoke).
 *
 * Runs the exact same bring-up sequence as port/main_android.cpp — full UI
 * stack, Core plugin, patch launch, Window on EGL — but with a Mesa
 * surfaceless/pbuffer context instead of an ANativeWindow, and no Oboe.
 * Renders a number of frames and shuts down. Any exception or crash here is
 * a bug that would also kill the app on device.
 *
 * Run with: EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 ./rack_ui_smoke
 */
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>

#include <GLES3/gl3.h>
#include <stb_image_write.h>

#include <common.hpp>
#include <system.hpp>
#include <asset.hpp>
#include <logger.hpp>
#include <random.hpp>
#include <string.hpp>
#include <settings.hpp>
#include <audio.hpp>
#include <midi.hpp>
#include <midiloopback.hpp>
#include <keyboard.hpp>
#include <plugin.hpp>
#include <library.hpp>
#include <network.hpp>
#include <context.hpp>
#include <engine/Engine.hpp>
#include <history.hpp>
#include <patch.hpp>
#include <widget/event.hpp>
#include <app/Scene.hpp>
#include <app/Browser.hpp>
#include <app/RackWidget.hpp>
#include <app/ModuleWidget.hpp>
#include <plugin/Model.hpp>
#include <ui/common.hpp>
#include <ui/Menu.hpp>
#include <ui/MenuSeparator.hpp>
#include <helpers.hpp>
#include <window/Window.hpp>

#include "stim_patch.hpp"
#include "../port/engine_barrier.hpp"
#include "../port/window_android.hpp"
#include "../port/static_plugins.hpp"
#include "../port/label_overlay.hpp"

using namespace rack;


static bool islandsUsed = true;
static bool islandsDrew = false;
/** Which model and which of its five runs, for the alarm: a module can loop
for ever in process() on knobs it was never meant to have. */
static char islandsNow[256] = "";
static void islandsHung(int sig) {
	const char* said = sig == SIGALRM ? "\nHUNG: " : "\nCRASHED: ";
	(void) !write(1, said, std::strlen(said));
	(void) !write(1, islandsNow, std::strlen(islandsNow));
	(void) !write(1, "\n", 1);
	_exit(3);
}

/** stimRun (stim_patch.hpp) as islands or in Rack's loop. A quarter of a
second: that long the engine steps a new patch as islands whatever it costs
(the warm-up and the first half of its trial), so no switch is needed to
force them. */
static std::vector<float> islandsRun(plugin::Model* model, bool on, int threads) {
	std::snprintf(islandsNow, sizeof(islandsNow), "%s/%s, %s on %d thread%s", model->plugin->slug.c_str(), model->slug.c_str(),
		on ? "islands" : "Rack's loop", threads, threads > 1 ? "s" : "");
	alarm(120);
	rackdroid::engineIslandsOn = on;
	uint32_t before = rackdroid::engineIslandsByWorkers;
	bool drew = false;
	std::vector<float> out = stimRun(model, threads, !on && threads == 1 ? &drew : NULL);
	if (drew)
		islandsDrew = true;
	if (on && (rackdroid::engineIslandCount < STIM_COPIES || (threads > 1 && rackdroid::engineIslandsByWorkers == before)))
		islandsUsed = false;
	return out;
}


int main(int argc, char* argv[]) {
	std::string tmpDir = system::getTempDirectory() + "/rackdroid-ui-smoke";
	// Never inherit an autosave/settings file from a previous smoke run. In
	// particular, --all-modules used to persist every tested module and made
	// the next run load ~1000 stale instances before the test even started.
	system::removeRecursively(tmpDir);
	// RACKDROID_SYSTEM_DIR can point at a copy of what the APK actually ships
	// (assets/system.zip contents) to reproduce the on-device layout.
	const char* sysDirEnv = std::getenv("RACKDROID_SYSTEM_DIR");
	asset::systemDir = sysDirEnv ? sysDirEnv : RACKDROID_RACK_DIR;
	asset::userDir = tmpDir + "/user";
	system::createDirectories(asset::userDir);

	settings::devMode = true; // log to stderr
	settings::headless = false;
	settings::showTipsOnLaunch = false;

	system::init();
	system::resetFpuFlags();
	asset::init();
	logger::init();
	random::init();

	std::printf("== %s %s (phase-2 UI smoke test)\n", APP_NAME.c_str(), APP_VERSION.c_str());

	string::init(); // also required on Android: translations ship in system.zip
	settings::init();
	settings::sampleRate = 48000.f;

	network::init();
	audio::init(); // No drivers registered: like Android with mic denied and no Oboe
	midi::init();
	keyboard::init();
	midiloopback::init();
	plugin::init();
	rackdroid::loadStaticPlugins();
	app::browserInit();
	library::init();
	ui::init();
	window::init();

	contextSet(new Context);
	APP->midiLoopbackContext = new midiloopback::Context;
	APP->engine = new engine::Engine;
	APP->history = new history::State;
	APP->event = new widget::EventState;
	APP->scene = new app::Scene;
	APP->event->rootWidget = APP->scene;
	APP->patch = new patch::Manager;

	std::printf("== runtime up, creating Window (EGL pbuffer)\n");
	rackdroid::windowSetPendingSurface(NULL, 2.f);
	APP->window = new window::Window;

	std::printf("== window up, launching patch\n");
	APP->patch->launch("");
	// --islands steps engines of its own, one at a time.
	bool islandsMode = argc > 2 && (std::string(argv[2]) == "--islands" || std::string(argv[2]) == "--dump");
	if (!islandsMode)
		APP->engine->startFallbackThread();
	rackdroid::installLabelOverlay();
	std::printf("== patch loaded: '%s'\n", APP->patch->path.c_str());

	// --menu: pop a sample context menu to eyeball touch sizing.
	if (argc > 2 && std::string(argv[2]) == "--menu") {
		ui::Menu* menu = createMenu();
		menu->box.pos = math::Vec(120, 60);
		menu->addChild(createMenuLabel("Sample menu"));
		menu->addChild(createMenuItem("Add module", "Enter", [] {}));
		menu->addChild(createMenuItem("Copy", "Ctrl+C", [] {}));
		menu->addChild(createMenuItem("Paste", "Ctrl+V", [] {}));
		menu->addChild(createMenuItem("Delete", "", [] {}));
		menu->addChild(new ui::MenuSeparator);
		menu->addChild(createMenuItem("Randomize", "", [] {}));
		menu->addChild(createMenuItem("Disconnect cables", "", [] {}));
	}

	int frames = (argc > 1) ? std::atoi(argv[1]) : 60;
	for (int i = 0; i < frames; i++) {
		APP->window->step();
	}
	std::printf("== rendered %d frames\n", frames);
	std::fflush(stdout);

	// --probe: let the fallback thread run the engine for a while, then dump
	// every module's output voltages — verifies a patch actually makes
	// signal without needing ears (used to debug the bundled demo patches).
	if (argc > 2 && std::string(argv[2]) == "--probe") {
		for (int pass = 0; pass < 3; pass++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(700));
			std::printf("-- probe pass %d (engine frame %lld)\n", pass, (long long) APP->engine->getFrame());
			for (int64_t moduleId : APP->engine->getModuleIds()) {
				engine::Module* m = APP->engine->getModule(moduleId);
				if (!m || !m->model)
					continue;
				std::printf("   %-24s", m->model->name.c_str());
				for (int o = 0; o < (int) m->outputs.size() && o < 10; o++)
					std::printf(" %6.2f", m->outputs[o].getVoltage());
				std::printf("   in:");
				for (int in = 0; in < (int) m->inputs.size() && in < 8; in++)
					std::printf(" %6.2f", m->inputs[in].getVoltage());
				std::printf("\n");
			}
		}
		std::fflush(stdout);
	}

	// --all-modules: instantiate every registered model like the module
	// browser does (Module + ModuleWidget added to the rack), rendering as
	// we go. Catches per-module crashes off-device.
	if (argc > 2 && std::string(argv[2]) == "--all-modules") {
		int count = 0;
		for (plugin::Plugin* p : plugin::plugins) {
			for (plugin::Model* model : p->models) {
				std::fprintf(stderr, "## adding %s/%s\n", p->slug.c_str(), model->slug.c_str());
				engine::Module* module = model->createModule();
				APP->engine->addModule(module);
				app::ModuleWidget* widget = model->createModuleWidget(module);
				if (!widget)
					throw std::runtime_error("createModuleWidget returned NULL for " +
						p->slug + "/" + model->slug);
				// Same call the module browser makes on selection
				APP->scene->rack->addModuleAtMouse(widget);
				APP->window->step();
				// One live instance is enough to cover constructor and draw paths.
				// Keeping all ~1000 instances made teardown take several minutes;
				// ModuleWidget's destructor removes and deletes its engine module.
				APP->scene->rack->removeModule(widget);
				delete widget;
				count++;
			}
		}
		for (int i = 0; i < 10; i++)
			APP->window->step();
		std::printf("== instantiated and rendered %d modules\n", count);
		std::fflush(stdout);
	}

	// --dump <file> [patch.json]: what vcv_dump.cpp writes from inside VCV
	// Rack's desktop binary, written by this build, to compare the two.
	if (argc > 3 && std::string(argv[2]) == "--dump") {
		rackdroid::engineIslandsOn = std::getenv("RACKDROID_DUMP_ISLANDS") != NULL;
		dumpRuns(argv[3], argc > 4 ? argv[4] : NULL);
		std::printf("== dumped to %s\n", argv[3]);
	}

	// --islands [plugin-slug]: every registered model stepped by Rack's own
	// loop and as islands (port/engine_islands.inc), the outputs compared bit
	// for bit. A model Rack's own loop does not play the same way twice -- on
	// one thread, or on three -- cannot be compared and is listed apart.
	if (argc > 2 && std::string(argv[2]) == "--islands") {
		APP->scene->rack->clear();
		signal(SIGALRM, islandsHung);
		signal(SIGSEGV, islandsHung);
		// Models this cannot be asked of, none of it to do with islands:
		// Venom's Bypass hands work to a thread of its own that walks the
		// engine's cables and outlives an engine taken down a quarter of a
		// second after it was built; RJModules' Gaussian indexes a nine-entry
		// histogram with whatever its generator gives and writes outside it
		// (it hung a run with five gigabytes in hand); FrozenWasteland's
		// ProbablyNoteArabic loops for ever and Bidoo's bordL crashes, both in
		// Rack's own loop on one thread, on the knob positions given here.
		// RACKDROID_ISLANDS_SKIP=Plugin/Model,Plugin/Model adds to them.
		std::string skip = std::string(",Venom/Bypass,RJModules/Gaussian,FrozenWasteland/ProbablyNoteArabic,Bidoo/bordL,") + (std::getenv("RACKDROID_ISLANDS_SKIP") ? std::getenv("RACKDROID_ISLANDS_SKIP") : "") + ",";
		int same = 0, unrepeatable = 0, failed = 0;
		for (plugin::Plugin* p : plugin::plugins) {
			if (argc > 3 && p->slug != argv[3])
				continue;
			for (plugin::Model* model : p->models) {
				if (skip.find("," + p->slug + "/" + model->slug + ",") != std::string::npos)
					continue;
				// RJModules' Gaussian indexes a nine-entry histogram with
				// whatever its own generator gives: it writes outside it, and
				// hung this run with five gigabytes in hand.
				if (p->slug == "RJModules" && model->slug == "Gaussian")
					continue;
				std::fprintf(stderr, "## islands %s/%s\n", p->slug.c_str(), model->slug.c_str());
				// Up to three times: a generator seeded from the clock gives
				// the same numbers to every run that starts within its tick,
				// and one pass of Rack's loop against itself does not show it.
				// A model fails only if Rack's loop agreed with itself -- before
				// the islands and after them -- and the islands did not, each time.
				const char* verdict = "same";
				for (int attempt = 0; attempt < 3; attempt++) {
					islandsDrew = false;
					std::vector<float> want = islandsRun(model, false, 1);
					bool drew = islandsDrew;
					bool repeatable = !drew && want == islandsRun(model, false, 1) && want == islandsRun(model, false, 3);
					islandsUsed = true;
					bool three = want == islandsRun(model, true, 3);
					bool one = want == islandsRun(model, true, 1);
					repeatable = repeatable && want == islandsRun(model, false, 1);
					if (!islandsUsed) {
						verdict = "FAIL: not stepped as islands";
						break;
					}
					if (!repeatable) {
						verdict = drew ? "draws random numbers in process()" : "not repeatable in Rack's loop";
						break;
					}
					if (three && one) {
						verdict = attempt ? "not repeatable in Rack's loop" : "same";
						break;
					}
					verdict = three ? "FAIL: differs on 1 thread" : one ? "FAIL: differs on 3 threads" : "FAIL: differs";
				}
				// Bidoo's lATe times its swing with clock(), the processor
				// time the whole process has used: how many of its own frames
				// fit in a millisecond of that depends on what else is stepped
				// between them.
				if (p->slug == "Bidoo" && model->slug == "lATe" && !std::strncmp(verdict, "FAIL: differs", 13))
					verdict = "reads the processor clock";
				if (!std::strncmp(verdict, "FAIL", 4))
					failed++;
				else if (std::strcmp(verdict, "same") != 0)
					unrepeatable++;
				else
					same++;
				if (std::strcmp(verdict, "same") != 0)
					std::printf("%s/%s: %s\n", p->slug.c_str(), model->slug.c_str(), verdict);
				std::fflush(stdout);
			}
		}
		alarm(0);
		std::printf("== islands: %d models the same bit for bit, %d Rack itself does not repeat, %d FAILED\n", same, unrepeatable, failed);
		std::fflush(stdout);
		if (failed)
			return 1;
	}

	// --export-thumbnails <outdir>: render one PNG per registered model, for
	// the native Android module browser's grid (ModuleThumbnails.kt).
	// Each module is added to the rack via the same RackWidget::addModule
	// used by the real add-module path (not a disconnected preview tree
	// like Browser.cpp's ModelBox) so main_android.cpp's runtime param/port
	// label overlay -- which only labels modules it finds via
	// RackWidget::getModules(), i.e. actually in the rack -- picks it up
	// too; the exported thumbnail then matches what's on screen during
	// play, not just the bare panel. Positioned at (0,0), rendered, and the
	// crop matching its own box size is saved (the pbuffer above is sized
	// wide/tall enough that no module needs the surface itself resized per
	// model). Run once locally; output is committed to
	// graphics/browser-thumbs/ and packaged like graphics/system-res/ (see
	// graphics/regen_graphics.py).
	if (argc > 2 && std::string(argv[2]) == "--export-thumbnails") {
		std::string outDir = (argc > 3) ? argv[3] : "thumbnails";
		system::createDirectories(outDir);
		// The default template patch has its own modules/note (visible in
		// screenshots throughout this port as the "Tutorial patch
		// instructions" box) sitting right at the origin our modules are
		// placed at, and MenuBar is a fixed screen-space overlay drawn on
		// top of everything regardless of scroll -- both would otherwise
		// bleed into every crop.
		APP->scene->rack->clear();
		if (APP->scene->menuBar)
			APP->scene->menuBar->hide();
		float pixelRatio = APP->window->pixelRatio;
		math::Vec fbSize = APP->window->getSize();
		int fbWidth = (int) fbSize.x, fbHeight = (int) fbSize.y;
		int count = 0, failed = 0;
		for (plugin::Plugin* p : plugin::plugins) {
			std::string pluginDir = outDir + "/" + p->slug;
			system::createDirectories(pluginDir);
			for (plugin::Model* model : p->models) {
				app::ModuleWidget* widget = NULL;
				try {
					// A real engine::Module, not createModuleWidget(NULL)'s
					// preview mode: RackWidget::updateExpanders() (run by
					// addModule() below) unconditionally dereferences
					// mw->module for every module in the rack, so a
					// null-module widget segfaults the moment a second
					// module -- or even just this one alone, if the default
					// patch already has any -- gets added alongside it.
					// deleting `widget` below tears both down again
					// (ModuleWidget::~ModuleWidget -> setModule(NULL) calls
					// Engine::removeModule + delete module for us).
					engine::Module* module = model->createModule();
					APP->engine->addModule(module);
					widget = model->createModuleWidget(module);
					if (!widget)
						throw std::runtime_error("createModuleWidget returned NULL");
					// RackWidget itself sits at a large, scroll-dependent
					// absolute offset (it re-centers on the loaded patch's
					// modules), so a widget at LOCAL (0,0) does not land at
					// scene (0,0) -- compensate so its ABSOLUTE position is
					// (0,0), regardless of wherever the view has scrolled to.
					math::Vec rackOrigin = APP->scene->rack->getAbsoluteOffset(math::Vec(0, 0));
					widget->box.pos = math::Vec(0, 0).minus(rackOrigin);
					APP->scene->rack->addModule(widget); // finds getModules(), unlike a bare scene->addChild
					APP->window->step(); // also drives the label overlay's own step
					APP->window->step();

					math::Vec origin = widget->getAbsoluteOffset(math::Vec(0, 0));
					int x0 = (int) std::round(origin.x * pixelRatio);
					int y0 = (int) std::round(origin.y * pixelRatio);
					int w = (int) std::ceil(widget->box.size.x * pixelRatio);
					int h = (int) std::ceil(widget->box.size.y * pixelRatio);
					if (w <= 0 || h <= 0 || x0 < 0 || y0 < 0 || x0 + w > fbWidth || y0 + h > fbHeight) {
						std::fprintf(stderr, "## skip %s/%s: rect %d,%d %dx%d exceeds %dx%d canvas\n",
							p->slug.c_str(), model->slug.c_str(), x0, y0, w, h, fbWidth, fbHeight);
						failed++;
					}
					else {
						std::vector<uint8_t> pixels(fbWidth * fbHeight * 4);
						glReadPixels(0, 0, fbWidth, fbHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
						// Flip vertically (GL origin bottom-left), then crop
						// the w x h region where the widget landed.
						std::vector<uint8_t> crop(w * h * 4);
						for (int y = 0; y < h; y++) {
							int srcY = fbHeight - 1 - (y0 + y);
							std::memcpy(&crop[y * w * 4], &pixels[(srcY * fbWidth + x0) * 4], w * 4);
						}
						std::string path = pluginDir + "/" + model->slug + ".png";
						stbi_write_png(path.c_str(), w, h, 4, crop.data(), w * 4);
						count++;
					}
					APP->scene->rack->removeModule(widget);
					delete widget;
				}
				catch (std::exception& e) {
					std::fprintf(stderr, "## thumbnail failed %s/%s: %s\n",
						p->slug.c_str(), model->slug.c_str(), e.what());
					if (widget) {
						APP->scene->rack->removeModule(widget);
						delete widget;
					}
					failed++;
				}
			}
		}
		std::printf("== exported %d thumbnails (%d failed) to %s\n", count, failed, outDir.c_str());
		std::fflush(stdout);
	}

	// Dump the last frame so rendering can be inspected visually.
	{
		math::Vec size = APP->window->getSize();
		int w = size.x, h = size.y;
		std::vector<uint8_t> pixels(w * h * 4);
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
		// Flip vertically (GL origin is bottom-left)
		std::vector<uint8_t> flipped(w * h * 4);
		for (int y = 0; y < h; y++)
			std::memcpy(&flipped[y * w * 4], &pixels[(h - 1 - y) * w * 4], w * 4);
		stbi_write_png("ui_smoke_frame.png", w, h, 4, flipped.data(), w * 4);
		std::printf("== frame dumped to ui_smoke_frame.png\n");
	}

	// Same teardown as the app
	APP->patch->saveAutosave();
	settings::save();
	// Destructors (Window, Scene) use the APP macro: the context must still
	// be set while they run.
	delete APP;
	contextSet(NULL);
	window::destroy();
	ui::destroy();
	library::destroy();
	plugin::destroy();
	midi::destroy();
	audio::destroy();
	settings::destroy();
	logger::destroy();

	std::printf("== OK\n");
	return 0;
}
