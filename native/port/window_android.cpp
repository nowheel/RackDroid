/* Android implementation of rack::window (replaces src/window/Window.cpp).
 *
 * Upstream Window owns a GLFW window + desktop GL context. Here it owns an
 * EGL context on the ANativeWindow provided by NativeActivity, with nanovg on
 * GLES3. Differences from upstream, by design:
 *  - no run loop: android_main drives step() from the Looper loop
 *  - the EGL surface can disappear (APP_CMD_TERM_WINDOW) and come back while
 *    the EGL context — and thus all nanovg textures — stays alive
 *  - pixelRatio comes from the display density instead of the monitor scale
 *  - mods/cursor/fullscreen concepts are stubs or driven by touch_input
 *
 * Font/Image/cache logic is reproduced from the upstream file (GPLv3).
 */
#include <map>

#if defined(__ANDROID__)
	#include <android/native_window.h>
#else
	// Host reproduction build (rack_ui_smoke): EGL surfaceless + pbuffer.
	struct ANativeWindow;
#endif
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <nanovg.h>
#define NANOVG_GLES3
#include <nanovg_gl.h>
#define NANOVG_FBO_VALID
#include <nanovg_gl_utils.h>

#include <window/Window.hpp>
#include <asset.hpp>
#include <widget/Widget.hpp>
#include <app/Scene.hpp>
#include <app/RackWidget.hpp>
#include <context.hpp>
#include <patch.hpp>
#include <settings.hpp>
#include <system.hpp>

#include <blendish.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "window_android.hpp"
#include "touch_input.hpp"
#include "menu_touch.hpp"

/** Set from the frame loop (main_android.cpp), which is the layer that can see
both the audio driver and the window: this file lives in the engine library and
cannot call into the app's. Read by Window::step, written by
rackdroid::windowSetAudioStressed at the bottom of this file. */
static std::atomic<bool> g_audioStressed{false};

/** How long after the fingers last moved apart or together framebuffers are
still left alone (see pinchFreezesFramebuffers()). Long enough to bridge the gap between two touch samples, short
enough that fingers resting mid-gesture already get a sharp picture. */
static const double PINCH_FB_HOLD_SEC = 0.1;

/** While a pinch is moving the zoom, no framebuffer is rebuilt at all: every
one is drawn scaled from what it last held, and they sharpen once the fingers
stop. Rebuilding one at a new size costs far more than drawing it -- measured
40-250 ms for the rail's single tile alone on a Nothing A024 and a OnePlus 8T,
and 50-100 ms for one module panel, growing with the framebuffer's pixel size
and not with the SVG's complexity (a rail cut from 667 shapes to 51 cost the
same). That is the texture being deleted and reallocated at every zoom step,
and it is already out of date by the next step anyway.

Two levers, because FramebufferWidget::draw() has two reasons to rebuild: time
left in the frame (getFrameDurationRemaining(), made negative here), and being
the first dirty framebuffer of the frame, which it rebuilds regardless. The
second is judged by Window::fbCount(), which Window::step() therefore starts at
1 instead of 0 during a pinch -- "one has already been rebuilt this frame". So
the rail, always first in the rack, stops paying for the whole gesture.

The price: a module that has never been drawn before (scrolled into view for
the first time during the pinch) stays empty until the fingers stop. */
static bool pinchFreezesFramebuffers() {
	return rackdroid::touchSecondsSincePinch() < PINCH_FB_HOLD_SEC;
}

/** See rackdroid::RenderPhase. */
static std::atomic<int> g_renderPhase{0};

/** Timestamps inside Scene::draw, taken by marker widgets placed between the
RackWidget's own children (see windowInstallDrawMarkers()). Reset every frame;
zero means the marker was not reached. */
enum DrawMark {
	MARK_START,        // before the rail
	MARK_RAIL,         // after the rail
	MARK_MODULES,      // after every module's panel and widgets
	MARK_CONTAINERS,   // after the plug and cable containers' base layer
	MARK_LABELS,       // after RackDroid's control labels
	MARK_LAYERS_START, // lights and halos begin
	MARK_LAYERS_END,   // cables done
	DRAW_MARKS
};
static double g_drawMark[DRAW_MARKS];

struct DrawMarker : rack::widget::Widget {
	int drawMark = -1;
	int layerMark = -1;
	int markLayer = 0;
	void draw(const DrawArgs& args) override {
		if (drawMark >= 0)
			g_drawMark[drawMark] = rack::system::getTime();
	}
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layerMark >= 0 && layer == markLayer)
			g_drawMark[layerMark] = rack::system::getTime();
	}
};

static double markSpan(int from, int to) {
	if (g_drawMark[from] <= 0.0 || g_drawMark[to] <= 0.0 || g_drawMark[to] < g_drawMark[from])
		return -1.0;
	return g_drawMark[to] - g_drawMark[from];
}

/** Where a slow frame spent its time, one entry per RenderPhase. Only the
stages inside Window::step are timed here; a frame that is slow before it gets
there (input, maintenance) shows up as a long gap between frames instead. */
static void reportSlowFrame(double gap, const double* spent, int fbDirty, bool pinchFrozen) {
	// A frame longer than this has already been seen by anyone looking at the
	// screen. Most frames are ~8-17 ms; a stall is what is being looked for.
	static const double SLOW_FRAME_SEC = 0.050;
	double total = 0.0;
	for (int i = 0; i < rackdroid::RENDER_PHASES; i++)
		total += spent[i];
	if (total < SLOW_FRAME_SEC && gap < SLOW_FRAME_SEC * 2)
		return;
	// A stall produces a run of these; a few a second are enough to read it,
	// and the rest would only push older evidence out of a capped log file.
	static double budgetFrom = 0.0;
	static int budget = 0;
	double now = rack::system::getTime();
	if (now - budgetFrom >= 1.0) {
		budgetFrom = now;
		budget = 5;
	}
	if (budget <= 0)
		return;
	budget--;
	WARN("Render: slow frame %.0f ms (since previous %.0f ms): step %.0f, draw %.0f, "
		"flush %.0f, swap %.0f", total * 1e3, gap * 1e3,
		spent[rackdroid::RENDER_STEP] * 1e3, spent[rackdroid::RENDER_DRAW] * 1e3,
		spent[rackdroid::RENDER_FLUSH] * 1e3, spent[rackdroid::RENDER_SWAP] * 1e3);
	// And inside draw: which part of the rack, how many framebuffers asked to
	// be rebuilt, and whether the pinch was holding them back. -1 is a
	// span whose markers were not both reached (nothing of the rack drawn).
	WARN("Render:   draw = rail %.1f, modules %.1f, containers %.1f, labels %.1f, "
		"lights/plugs/cables %.1f ms; %d framebuffers dirty, pinch freeze %s, zoom %.2f",
		markSpan(MARK_START, MARK_RAIL) * 1e3, markSpan(MARK_RAIL, MARK_MODULES) * 1e3,
		markSpan(MARK_MODULES, MARK_CONTAINERS) * 1e3,
		markSpan(MARK_CONTAINERS, MARK_LABELS) * 1e3,
		markSpan(MARK_LAYERS_START, MARK_LAYERS_END) * 1e3,
		fbDirty, pinchFrozen ? "on" : "off",
		(APP->scene && APP->scene->rackScroll) ? APP->scene->rackScroll->getZoom() : 0.f);
}
#if defined(__ANDROID__)
	#include "menu_native.hpp"
	#include "browser_native.hpp"
#endif


namespace rack {
namespace window {


Font::~Font() {
	// There is no NanoVG deleteFont() function yet, so do nothing
}


void Font::loadFile(const std::string& filename, NVGcontext* vg) {
	this->vg = vg;
	std::string name = system::getStem(filename);
	size_t size;
	// Transfer ownership of font data to font object
	uint8_t* data = system::readFile(filename, &size);
	handle = nvgCreateFontMem(vg, name.c_str(), data, size, 0);
	if (handle < 0) {
		std::free(data);
		throw Exception("Failed to load font %s", filename.c_str());
	}
	INFO("Loaded font %s", filename.c_str());
}


std::shared_ptr<Font> Font::load(const std::string& filename) {
	return APP->window->loadFont(filename);
}


Image::~Image() {
	if (handle >= 0)
		nvgDeleteImage(vg, handle);
}


void Image::loadFile(const std::string& filename, NVGcontext* vg) {
	this->vg = vg;
	std::vector<uint8_t> data = system::readFile(filename);
	handle = nvgCreateImageMem(vg, NVG_IMAGE_REPEATX | NVG_IMAGE_REPEATY, data.data(), data.size());
	if (handle <= 0)
		throw Exception("Failed to load image %s", filename.c_str());
	INFO("Loaded image %s", filename.c_str());
}


std::shared_ptr<Image> Image::load(const std::string& filename) {
	return APP->window->loadImage(filename);
}


// Surface handed over by android_main before Window is (re)created.
static ANativeWindow* pendingNativeWindow = NULL;
static float displayDensity = 2.f;

/** UI scale: display density, giving desktop-equivalent physical sizes
(finger-friendly knobs, readable text). Fixed-size overlays wider than the
resulting scene (e.g. the 550-unit tips window on portrait phones) are dealt
with individually rather than by shrinking the whole UI. Users can override
with settings::pixelRatio. */
static float effectivePixelRatio(int fbWidth, int fbHeight) {
	(void) fbWidth;
	(void) fbHeight;
	if (settings::pixelRatio > 0.f)
		return settings::pixelRatio;
	return std::fmax(1.f, displayDensity);
}


struct Window::Internal {
	ANativeWindow* nativeWindow = NULL;
	EGLDisplay display = EGL_NO_DISPLAY;
	EGLConfig config = NULL;
	EGLSurface surface = EGL_NO_SURFACE;
	EGLContext context = EGL_NO_CONTEXT;

	int fbWidth = 0;
	int fbHeight = 0;

	int frame = 0;
	double monitorRefreshRate = 60.0;
	double frameTime = NAN;
	double lastFrameDuration = NAN;

	int mods = 0;
	bool shouldClose = false;

	double lastInteraction = 0.0;
	int swapInterval = 1;

	std::map<std::string, std::shared_ptr<Font>> fontCache;
	std::map<std::string, std::shared_ptr<Image>> imageCache;

	bool fbDirtyOnSubpixelChange = true;
	int fbCount = 0;

	void createSurface(ANativeWindow* win) {
		nativeWindow = win;
		if (win) {
#if defined(__ANDROID__)
			surface = eglCreateWindowSurface(display, config, win, NULL);
#endif
		}
		else {
			// No native window (host smoke test): render into a pbuffer. Wide
			// enough to fit a single module (up to ~150HP at 2x pixelRatio)
			// uncropped for --export-thumbnails, which renders one module at
			// scene position (0,0) and reads back a tight crop of it.
			const EGLint pbAttribs[] = {EGL_WIDTH, 4800, EGL_HEIGHT, 900, EGL_NONE};
			surface = eglCreatePbufferSurface(display, config, pbAttribs);
		}
		if (surface == EGL_NO_SURFACE)
			throw Exception("eglCreate*Surface failed: 0x%x", eglGetError());
		if (!eglMakeCurrent(display, surface, surface, context))
			throw Exception("eglMakeCurrent failed: 0x%x", eglGetError());
		eglSwapInterval(display, 1);
		eglQuerySurface(display, surface, EGL_WIDTH, &fbWidth);
		eglQuerySurface(display, surface, EGL_HEIGHT, &fbHeight);
	}

	void destroySurface() {
		if (display == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE)
			return;
		// Keep the context (and all GL resources) alive without a surface.
		eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
		eglDestroySurface(display, surface);
		surface = EGL_NO_SURFACE;
		nativeWindow = NULL;
	}
};


Window::Window() {
	internal = new Internal;

	internal->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!eglInitialize(internal->display, NULL, NULL))
		throw Exception("eglInitialize failed");

	// Pbuffer configs (host smoke test) often lack WINDOW_BIT and vice versa:
	// request only the surface type actually needed.
	const EGLint surfaceType = pendingNativeWindow ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT;
	const EGLint configAttribs[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_SURFACE_TYPE, surfaceType,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_DEPTH_SIZE, 0,
		EGL_STENCIL_SIZE, 8, // nanovg requires a stencil buffer
		EGL_NONE
	};
	EGLint numConfigs = 0;
	if (!eglChooseConfig(internal->display, configAttribs, &internal->config, 1, &numConfigs) || numConfigs < 1)
		throw Exception("No GLES3 EGL config with stencil buffer");

	const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
	internal->context = eglCreateContext(internal->display, internal->config, EGL_NO_CONTEXT, contextAttribs);
	if (internal->context == EGL_NO_CONTEXT)
		throw Exception("eglCreateContext (ES3) failed: 0x%x", eglGetError());

	internal->createSurface(pendingNativeWindow);
	pendingNativeWindow = NULL;

	INFO("Renderer: %s %s", glGetString(GL_VENDOR), glGetString(GL_RENDERER));
	INFO("OpenGL: %s", glGetString(GL_VERSION));

	// Set up NanoVG
	int nvgFlags = NVG_ANTIALIAS;
	vg = nvgCreateGLES3(nvgFlags);
	fbVg = nvgCreateSharedGLES3(vg, nvgFlags);
	if (!vg)
		throw Exception("Could not initialize NanoVG (GLES3)");

	pixelRatio = effectivePixelRatio(internal->fbWidth, internal->fbHeight);
	windowRatio = 1.f;

	// Load UI fonts. Geomini (OFL, ships in graphics/system-res/fonts) is
	// the app typeface; it is Latin-only, so fallbacks are added in
	// coverage order -- DejaVu FIRST for punctuation/symbols (the Noto CJK
	// faces also carry Latin glyphs and would win with loadFont()'s
	// default fallback order), then the CJK + emoji chain.
	uiFont = loadFontWithoutFallbacks(asset::system("res/fonts/Geomini.ttf"));
	if (uiFont) {
		for (const char* fb : {"res/fonts/DejaVuSans.ttf", "res/fonts/NotoSansJP-Medium.otf",
				"res/fonts/NotoSansSC-Medium.otf", "res/fonts/NotoEmoji-Medium.ttf"}) {
			std::shared_ptr<Font> f = loadFontWithoutFallbacks(asset::system(fb));
			if (f)
				nvgAddFallbackFontId(vg, uiFont->handle, f->handle);
		}
	}
	else {
		uiFont = loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
	}
	if (uiFont)
		bndSetFont(uiFont->handle);

	if (APP->scene) {
		widget::Widget::ContextCreateEvent e;
		e.vg = vg;
		APP->scene->onContextCreate(e);
	}
}


Window::~Window() {
	if (APP->scene) {
		widget::Widget::ContextDestroyEvent e;
		e.vg = vg;
		APP->scene->onContextDestroy(e);
	}

	// Fonts and Images in the cache must be deleted before the NanoVG context
	internal->fontCache.clear();
	internal->imageCache.clear();

	if (internal->display != EGL_NO_DISPLAY) {
		eglMakeCurrent(internal->display, EGL_NO_SURFACE, EGL_NO_SURFACE, internal->context);
		nvgDeleteGLES3(fbVg);
		nvgDeleteGLES3(vg);
		internal->destroySurface();
		eglMakeCurrent(internal->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroyContext(internal->display, internal->context);
		eglTerminate(internal->display);
	}
	delete internal;
}


math::Vec Window::getSize() {
	return math::Vec(internal->fbWidth, internal->fbHeight);
}


void Window::setSize(math::Vec size) {
	// Window size is dictated by the OS on Android.
	(void) size;
}


void Window::run() {
	// Not used on Android: android_main() drives step() from the Looper loop.
}


void Window::step() {
	// No surface (app in background): skip rendering entirely.
	if (internal->surface == EGL_NO_SURFACE)
		return;

	double frameTime = system::getTime();
	double frameGap = 0.0;
	if (std::isfinite(internal->frameTime)) {
		internal->lastFrameDuration = frameTime - internal->frameTime;
		frameGap = internal->lastFrameDuration;
	}
	double spent[rackdroid::RENDER_PHASES] = {};
	for (int i = 0; i < DRAW_MARKS; i++)
		g_drawMark[i] = 0.0;
	double stageAt = frameTime;
	int stage = rackdroid::RENDER_STEP;
	auto enterStage = [&](int next) {
		double t = system::getTime();
		spent[stage] += t - stageAt;
		stageAt = t;
		stage = next;
		rackdroid::windowSetPhase(next);
	};
	rackdroid::windowSetPhase(rackdroid::RENDER_STEP);
	internal->frameTime = frameTime;
	internal->fbCount = pinchFreezesFramebuffers() ? 1 : 0;

	// Make event handlers and step() have a clean NanoVG context
	nvgReset(vg);

	if (uiFont)
		bndSetFont(uiFont->handle);

	// Surface may have been resized (rotation, split screen)
	eglQuerySurface(internal->display, internal->surface, EGL_WIDTH, &internal->fbWidth);
	eglQuerySurface(internal->display, internal->surface, EGL_HEIGHT, &internal->fbHeight);
	int fbWidth = internal->fbWidth;
	int fbHeight = internal->fbHeight;

	// Get desired pixel ratio
	float newPixelRatio = effectivePixelRatio(fbWidth, fbHeight);
	if (newPixelRatio != pixelRatio) {
		pixelRatio = newPixelRatio;
		APP->event->handleDirty();
	}
	windowRatio = 1.f;

	if (APP->scene) {
		// Resize scene
		APP->scene->box.size = math::Vec(fbWidth, fbHeight).div(pixelRatio);

		// Step scene
		APP->scene->step();

		// Mobile: keep floating overlays (tips window, future dialogs) on
		// screen even when they center themselves off a small scene.
		for (widget::Widget* child : APP->scene->children)
			child->box = child->box.nudge(APP->scene->box.zeroPos());

		// Prefer native Android bottom-sheet menus; whatever they don't
		// capture (menus with non-list widgets) is resized for touch.
#if defined(__ANDROID__)
		rackdroid::processNativeMenus();
		rackdroid::processNativeBrowser();
#endif
		rackdroid::fixupMenus();

		// Render scene
		enterStage(rackdroid::RENDER_DRAW);
		nvgBeginFrame(vg, fbWidth, fbHeight, pixelRatio);
		nvgScale(vg, pixelRatio, pixelRatio);

		widget::Widget::DrawArgs args;
		args.vg = vg;
		args.clipBox = APP->scene->box.zeroPos();
		APP->scene->draw(args);

		enterStage(rackdroid::RENDER_FLUSH);
		glViewport(0, 0, fbWidth, fbHeight);
		glClearColor(0.0, 0.0, 0.0, 1.0);
		glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		nvgEndFrame(vg);
	}

	// Battery: halve the frame rate (vsync/2) after a few seconds without
	// touch interaction; back to full rate on the next touch.
	int wantedInterval = (frameTime - internal->lastInteraction > 5.0) ? 2 : 1;
	// Audio first. Rendering and the audio callback compete for the same
	// cores, and some views cost far more to draw than others -- zoomed in
	// close, every module is rasterised into a much larger framebuffer, which
	// was reported breaking up the sound on an 8T while nothing about the
	// patch had changed. Underruns in the last couple of seconds mean the
	// callback is losing that competition, so give it room: half rate is a
	// visible cost, but only while something is already audibly wrong, and no
	// thread count can buy back time the renderer is taking.
	if (g_audioStressed.load(std::memory_order_relaxed))
		wantedInterval = 2;
	if (wantedInterval != internal->swapInterval) {
		internal->swapInterval = wantedInterval;
		eglSwapInterval(internal->display, wantedInterval);
	}

	enterStage(rackdroid::RENDER_SWAP);
	eglSwapBuffers(internal->display, internal->surface);
	enterStage(rackdroid::RENDER_IDLE);
	internal->frame++;
	reportSlowFrame(frameGap, spent, internal->fbCount,
		pinchFreezesFramebuffers());

	// Half rate (above) means nothing to a frame that takes five vsyncs. A
	// 133-module rack at zoom 0.25 costs 80 ms a frame on an SM-S901E; left
	// playing like that with nobody touching it, the render thread never
	// pauses, and the phone went from thermal status 1 to 3 in three minutes
	// and to 4 -- its CPU held to 55% -- in forty, where the patch that had
	// been running clean at 82% of its deadline underran 470 times a second.
	// With the phone cool the same view cost the audio nothing at all: the
	// drawing does its damage as heat. So once the screen has been left alone
	// for five seconds, an expensive frame is followed by twice as long a
	// rest. The next touch may wait for the end of one; hence the cap.
	double work = 0.0;
	for (int i = rackdroid::RENDER_STEP; i <= rackdroid::RENDER_SWAP; i++)
		work += spent[i];
	if (frameTime - internal->lastInteraction > 5.0 && work > 0.016)
		std::this_thread::sleep_for(std::chrono::duration<double>(work < 0.075 ? 2.0 * work : 0.15));
}


void Window::screenshot(const std::string& screenshotPath) {
	INFO("Screenshots not supported on Android (%s)", screenshotPath.c_str());
}


void Window::screenshotModules(const std::string& screenshotsDir, float zoom) {
	(void) zoom;
	INFO("Module screenshots not supported on Android (%s)", screenshotsDir.c_str());
}


void Window::close() {
	internal->shouldClose = true;
}


void Window::cursorLock() {}
void Window::cursorUnlock() {}
bool Window::isCursorLocked() {
	return false;
}


int Window::getMods() {
	return internal->mods;
}


void Window::setFullScreen(bool fullScreen) {
	(void) fullScreen;
}
bool Window::isFullScreen() {
	// Android apps are effectively always fullscreen.
	return true;
}


double Window::getMonitorRefreshRate() {
	return internal->monitorRefreshRate;
}


double Window::getFrameTime() {
	return internal->frameTime;
}


double Window::getLastFrameDuration() {
	return internal->lastFrameDuration;
}


double Window::getFrameDurationRemaining() {
	double elapsed = system::getTime() - internal->frameTime;
	// The only reader is FramebufferWidget::draw(), which re-renders a dirty
	// framebuffer while this is above -1/60 and otherwise draws the one it
	// has, scaled. See pinchFreezesFramebuffers() for why none is re-rendered
	// while a pinch is moving the zoom.
	if (pinchFreezesFramebuffers())
		return -1.0;
	double frameDuration = 1.f / settings::frameRateLimit;
	return frameDuration - elapsed;
}


std::shared_ptr<Font> Window::loadFont(const std::string& filename) {
	const auto& it = internal->fontCache.find(filename);
	if (it != internal->fontCache.end())
		return it->second;

	std::shared_ptr<Font> font = loadFontWithoutFallbacks(filename);
	if (!font)
		return NULL;

	std::shared_ptr<Font> jpFont = loadFontWithoutFallbacks(asset::system("res/fonts/NotoSansJP-Medium.otf"));
	if (jpFont)
		nvgAddFallbackFontId(vg, font->handle, jpFont->handle);
	std::shared_ptr<Font> scFont = loadFontWithoutFallbacks(asset::system("res/fonts/NotoSansSC-Medium.otf"));
	if (scFont)
		nvgAddFallbackFontId(vg, font->handle, scFont->handle);
	std::shared_ptr<Font> emojiFont = loadFontWithoutFallbacks(asset::system("res/fonts/NotoEmoji-Medium.ttf"));
	if (emojiFont)
		nvgAddFallbackFontId(vg, font->handle, emojiFont->handle);

	return font;
}


std::shared_ptr<Font> Window::loadFontWithoutFallbacks(const std::string& filename) {
	const auto& it = internal->fontCache.find(filename);
	if (it != internal->fontCache.end())
		return it->second;

	std::shared_ptr<Font> font = std::make_shared<Font>();
	try {
		font->loadFile(filename, vg);
	}
	catch (Exception& e) {
		WARN("%s", e.what());
		font = NULL;
	}
	internal->fontCache[filename] = font;
	return font;
}


std::shared_ptr<Image> Window::loadImage(const std::string& filename) {
	const auto& it = internal->imageCache.find(filename);
	if (it != internal->imageCache.end())
		return it->second;

	std::shared_ptr<Image> image;
	try {
		image = std::make_shared<Image>();
		image->loadFile(filename, vg);
	}
	catch (Exception& e) {
		WARN("%s", e.what());
		image = NULL;
	}
	internal->imageCache[filename] = image;
	return image;
}


bool& Window::fbDirtyOnSubpixelChange() {
	return internal->fbDirtyOnSubpixelChange;
}


int& Window::fbCount() {
	return internal->fbCount;
}


void init() {
	// Nothing to do: EGL is initialized per-Window.
}


void destroy() {
}


} // namespace window
} // namespace rack


// ---- Android-side control API (used by main_android / touch_input) ----

namespace rackdroid {


void windowSetPendingSurface(ANativeWindow* win, float density) {
	rack::window::pendingNativeWindow = win;
	if (density > 0.f)
		rack::window::displayDensity = density;
}


/** When the EGL surface last appeared or went away. Backgrounding, returning
and every rotation land here, and each one costs a burst of underruns that has
nothing to do with the patch -- so whatever is judging the engine by its
underrun count needs to know not to believe the next few seconds. */
static double g_lastSurfaceChange = 0.0;

double windowSecondsSinceSurfaceChange() {
	if (g_lastSurfaceChange <= 0.0)
		return 1e9;
	return rack::system::getTime() - g_lastSurfaceChange;
}


void windowNoteSurfaceChange() {
	g_lastSurfaceChange = rack::system::getTime();
}


void windowSurfaceChanged(ANativeWindow* win) {
	windowNoteSurfaceChange();
	rack::window::Window* w = APP->window;
	if (!w)
		return;
	w->internal->destroySurface();
	if (win)
		w->internal->createSurface(win);
}


void windowSurfaceLost() {
	windowSurfaceChanged(NULL);
}


bool windowHasSurface() {
	rack::window::Window* w = APP->window;
	return w && w->internal->surface != EGL_NO_SURFACE;
}


bool windowShouldClose() {
	rack::window::Window* w = APP->window;
	return w && w->internal->shouldClose;
}


void windowSetMods(int mods) {
	rack::window::Window* w = APP->window;
	if (w)
		w->internal->mods = mods;
}


void windowInstallDrawMarkers() {
	if (!APP->scene || !APP->scene->rack)
		return;
	rack::app::RackWidget* rack = APP->scene->rack;
	if (rack->children.empty())
		return;
	rack::widget::Widget* rail = rack->children.front();
	rack::widget::Widget* modules = rack->getModuleContainer();
	rack::widget::Widget* cables = rack->getCableContainer();
	auto marker = [&](int drawMark, int layerMark, int layer) {
		DrawMarker* m = new DrawMarker;
		m->box = rack->box.zeroPos(); // never culled
		m->drawMark = drawMark;
		m->layerMark = layerMark;
		m->markLayer = layer;
		return m;
	};
	rack->addChildBottom(marker(MARK_START, MARK_LAYERS_START, 1));
	rack->addChildAbove(marker(MARK_RAIL, -1, 0), rail);
	rack->addChildAbove(marker(MARK_MODULES, -1, 0), modules);
	rack->addChildAbove(marker(MARK_CONTAINERS, -1, 0), cables);
	rack->addChild(marker(MARK_LABELS, MARK_LAYERS_END, 3));
}


void windowSetPhase(int phase) {
	g_renderPhase.store(phase, std::memory_order_relaxed);
}


int windowPhase() {
	return g_renderPhase.load(std::memory_order_relaxed);
}


const char* windowPhaseName(int phase) {
	static const char* const names[RENDER_PHASES] = {
		"idle", "input", "tune", "step", "draw", "flush", "swap",
	};
	return (phase >= 0 && phase < RENDER_PHASES) ? names[phase] : "?";
}


void windowSetAudioStressed(bool stressed) {
	g_audioStressed.store(stressed, std::memory_order_relaxed);
}


void windowNoteInteraction() {
	rack::window::Window* w = APP->window;
	if (w)
		w->internal->lastInteraction = rack::system::getTime();
}


double windowSecondsSinceInteraction() {
	rack::window::Window* w = APP->window;
	if (!w || w->internal->lastInteraction <= 0.0)
		return 1e9;
	return rack::system::getTime() - w->internal->lastInteraction;
}


} // namespace rackdroid
