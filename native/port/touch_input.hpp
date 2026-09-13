#pragma once

struct AInputEvent;

namespace rackdroid {

/** How far the rack may be zoomed in on Android. Upstream allows 4x
(RackScrollWidget.cpp), where every visible module is rasterised into a
framebuffer with sixteen times the pixels AND that framebuffer is reallocated
on each zoom step -- an allocation storm on the render thread, which shares its
cores with the audio callback. A real 8T broke up audibly at maximum zoom with
nothing about the patch changed. Two is still a generous magnification on a
phone, and it costs a quarter of the pixels four does.

Enforced in two places on purpose: touch_input.cpp refuses to ask for more, so
a pinch stops at the wall instead of fighting a clamp every frame, and
checkZoomCeiling() in main_android.cpp catches every other route in -- the View
menu, and a patch file saved on desktop at 4x. */
static const float MAX_RACK_ZOOM = 2.f;

/** Translates an Android input event into Rack UI events.
Returns 1 if the event was consumed. */
int touchHandleEvent(AInputEvent* event);

/** Per-frame housekeeping (long-press detection). Call once per rendered
frame, before Window::step(). */
void touchStep();

} // namespace rackdroid
