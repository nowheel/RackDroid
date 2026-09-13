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
#include <context.hpp>
#include <patch.hpp>
#include <settings.hpp>
#include <system.hpp>

#include <blendish.h>

#include <atomic>

#include "window_android.hpp"
#include "menu_touch.hpp"

/** Set from the frame loop (main_android.cpp), which is the layer that can see
both the audio driver and the window: this file lives in the engine library and
cannot call into the app's. Read by Window::step, written by
rackdroid::windowSetAudioStressed at the bottom of this file. */
static std::atomic<bool> g_audioStressed{false};
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
	if (std::isfinite(internal->frameTime)) {
		internal->lastFrameDuration = frameTime - internal->frameTime;
	}
	internal->frameTime = frameTime;
	internal->fbCount = 0;

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
		nvgBeginFrame(vg, fbWidth, fbHeight, pixelRatio);
		nvgScale(vg, pixelRatio, pixelRatio);

		widget::Widget::DrawArgs args;
		args.vg = vg;
		args.clipBox = APP->scene->box.zeroPos();
		APP->scene->draw(args);

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

	eglSwapBuffers(internal->display, internal->surface);
	internal->frame++;
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
	double frameDuration = 1.f / settings::frameRateLimit;
	return frameDuration - (system::getTime() - internal->frameTime);
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


void windowSurfaceChanged(ANativeWindow* win) {
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
