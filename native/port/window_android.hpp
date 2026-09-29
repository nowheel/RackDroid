#pragma once

struct ANativeWindow;

namespace rackdroid {

/** Hands the NativeActivity surface to the next Window creation.
`density` is DisplayMetrics density (e.g. 2.75 for 440 dpi). */
void windowSetPendingSurface(ANativeWindow* win, float density);

/** Swaps the EGL surface under a live Window (rotation, background/foreground).
The EGL context and all GL resources survive. */
void windowSurfaceChanged(ANativeWindow* win);
void windowSurfaceLost();
bool windowHasSurface();

bool windowShouldClose();

/** Sets the modifier state reported by Window::getMods(). Used by touch_input
to emulate Ctrl+scroll (pinch zoom). */
void windowSetMods(int mods);

/** Tells the renderer the audio callback is struggling, so it halves the frame
rate until the sound recovers. Rendering and the callback compete for the same
cores and some views cost far more to draw than others -- zoomed in close,
every module is rasterised into a much larger framebuffer, which was reported
breaking up the sound on an 8T with nothing about the patch changed. Half rate
is a visible cost, but only while something is already audibly wrong, and no
thread count can buy back time the renderer is taking. */
void windowSetAudioStressed(bool stressed);

/** Marks user interaction: keeps the frame rate at full speed (it drops to
half after a few idle seconds to save battery). */
void windowNoteInteraction();

/** Seconds since the last touch, or a large number if there has never been
one. The engine's underrun machinery uses it to keep out of the way while the
rack is being handled: a pinch-zoom or a drag makes the render thread crowd
the audio callback for as long as it lasts, and those underruns say nothing
about the patch or the thread count. */
double windowSecondsSinceInteraction();

/** Seconds since the EGL surface last appeared, went away or was disturbed
(backgrounding, returning, rotation), or a large number if it never has. */
double windowSecondsSinceSurfaceChange();

/** Records a disturbance that costs a burst of frames without destroying the
surface. The activity declares configChanges="orientation|screenSize|...", so
a rotation never reaches APP_CMD_TERM_WINDOW -- it arrives as a resize, and
without this the underruns it causes look to the engine like the patch's
fault. */
void windowNoteSurfaceChange();

/** What the render thread is doing right now, coarsely. Written by the render
thread at each stage of its loop, read by the audio callback when a block runs
late, so a stall in the log says what the other side was busy with at the time.
A diagnostic only: nothing decides anything on it. */
enum RenderPhase {
	RENDER_IDLE = 0, // waiting in the looper
	RENDER_INPUT,    // dispatching a touch
	RENDER_TUNE,     // engine/audio maintenance in the frame loop
	RENDER_STEP,     // Scene::step (includes Rack's periodic autosave)
	RENDER_DRAW,     // Scene::draw, framebuffer rebuilds included
	RENDER_FLUSH,    // nvgEndFrame: the GL submission
	RENDER_SWAP,     // eglSwapBuffers
	RENDER_PHASES
};
void windowSetPhase(int phase);

/** Places timing markers between the RackWidget's children, so a slow frame's
log line can say which part of the rack took the time. Call once, after the
label overlay has been added (it must be the last child). Diagnostic only. */
void windowInstallDrawMarkers();
int windowPhase();
const char* windowPhaseName(int phase);

} // namespace rackdroid
