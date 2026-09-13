/* Touch → Rack event translation.
 *
 * Rack's event system is mouse-centric (EventState::handleButton/Hover/
 * Scroll with GLFW button/mod codes), so this layer emulates a mouse:
 *
 *  - one finger: hover + left button (tap = click, drag = knob/cable drag)
 *  - long-press without movement: right click (context menus)
 *  - two fingers: scroll (pan the rack); pinch: Ctrl+scroll (zoom)
 *
 * Coordinates: the EGL surface is 1:1 with touch coordinates (windowRatio=1),
 * and Rack scene coordinates are framebuffer / pixelRatio — same conversion
 * as upstream's cursorPosCallback.
 */
#include <atomic>
#include <cmath>

#include <android/input.h>
#include <jni.h>

#include <GLFW/glfw3.h>

#include <context.hpp>
#include <widget/event.hpp>
#include <window/Window.hpp>
#include <ui/TextField.hpp>
#include <app/ModuleWidget.hpp>
#include <app/PortWidget.hpp>
#include <app/Scene.hpp>
#include <app/RackScrollWidget.hpp>
#include <system.hpp>

#include "touch_input.hpp"
#include <typeinfo>
#include <android/log.h>
#include "cable_park.hpp"
#include <app/CableWidget.hpp>
#include <app/PortWidget.hpp>
#include <app/Scene.hpp>
#include <app/RackScrollWidget.hpp>
#include <app/RackWidget.hpp>
#include <app/Scene.hpp>
#include "window_android.hpp"
#include "jni_bridge.hpp"
#include "menu_native.hpp"
#include <logger.hpp>


namespace rackdroid {


/* Cable-park diagnostics have to reach BOTH sinks: logcat for whoever has the
phone on a cable, and user/log.txt for everyone else, since a release build is
not debuggable and `run-as` cannot read the private log. Plain INFO() alone
already cost one debugging round trip here — the messages existed and were
invisible. */
#define LOGI(...) do { __android_log_print(ANDROID_LOG_INFO, "rackdroid.cablepark", __VA_ARGS__); INFO(__VA_ARGS__); } while (0)
#define TOUCH_WARN(...) do { __android_log_print(ANDROID_LOG_WARN, "rackdroid.touch", __VA_ARGS__); WARN(__VA_ARGS__); } while (0)


static const double LONG_PRESS_SECONDS = 0.6;
static const float LONG_PRESS_SLOP_PX = 16.f; // in scene units
// How far the finger may drift and still count as "at rest" for the long-press
// timer. Small, so any real drag resets it; a resting finger's jitter does not.
static const float LONG_PRESS_STILL_PX = 5.f;
static const float PINCH_DETECT_RATIO = 0.02f;
static const float PINCH_ZOOM_SPEED = 8.f;
// Inertia (momentum) for one- and two-finger panning
static const float INERTIA_MIN_SPEED = 80.f;   // scene units/s to start coasting
static const float INERTIA_STOP_SPEED = 20.f;  // stop below this
static const float INERTIA_DECAY = 4.f;         // exponential decay per second
// Caps a single velocity sample before it enters the EMA. Scene units track
// dp (scenePos divides by density, same as Window's pixelRatio), so this is
// ~20 screen-widths/s on a typical phone -- nothing a real flick reaches. A
// sample past it is the touch controller misreporting the surviving finger
// for a frame right as the other one lifts (a known capacitive-sensor
// artifact), or a processing stall making dt tiny while delta is a real
// on-screen distance -- either way, dividing by that dt manufactures a
// velocity no finger produced, and it used to ride the EMA straight into
// startInertia() with nothing to stop it: the "strange inertia" that
// suddenly accelerates and flings the rack off screen (issue #4).
static const float MAX_PAN_SPEED = 8000.f;     // scene units/s


static rack::math::Vec clampPanVelocity(rack::math::Vec v) {
	float n = v.norm();
	if (n > MAX_PAN_SPEED) {
		TOUCH_WARN("Touch: pan velocity sample %.0f/s exceeds cap, clamping to %.0f/s "
			"(lift-off glitch or a processing stall, not a real flick)", n, MAX_PAN_SPEED);
		return v.mult(MAX_PAN_SPEED / n);
	}
	return v;
}

// Same family of glitch, same fix, on the pinch-zoom path: ratio = dist/
// lastDist - 1 blows up exactly like a velocity sample does if either
// finger's reported position glitches for one sample (a lift-off jitter, or
// lastDist itself near zero) -- and it feeds RackScrollWidget::setZoom as
// pow(2, 2*ratio) with nothing to stop it. Issue #3's log showed a ~25000x7500
// framebuffer allocation failing right after a burst of clamped pan samples
// on the same gesture; this is the same touch glitch reaching zoom instead of
// pan. Capped to ±1 -- a 4x zoom change in a single touch sample is already
// far past anything a real pinch produces, so nothing legitimate is lost.
static const float MAX_PINCH_RATIO = 1.f;

static float clampPinchRatio(float ratio) {
	if (std::fabs(ratio) > MAX_PINCH_RATIO) {
		TOUCH_WARN("Touch: pinch ratio %.2f exceeds cap, clamping to %.2f "
			"(lift-off glitch or a processing stall, not a real pinch)",
			ratio, std::copysign(MAX_PINCH_RATIO, ratio));
		return std::copysign(MAX_PINCH_RATIO, ratio);
	}
	return ratio;
}

// clampPinchRatio bounds a single MOVE callback's zoom change, which stops a
// single glitched sample -- but not a BURST of them: a render/input thread
// stalled by the engine underrunning (the far more common trigger here, per
// issue #3's field logs) does not drop the touch events queued behind it, it
// delivers them all at once when it recovers. Each one clamped is still each
// one applied, and pow(2, 2*ratio) compounds across the burst regardless of
// how small any single step was told to be -- confirmed on hardware: the
// per-event clamp alone cut the framebuffer-allocation failures from 8 to 3
// in one session, not to zero.
//
// The fix is the same idea clampPanVelocity already applies: rate-limit
// against real elapsed time, not per callback. Bounding how fast the raw
// finger SEPARATION may change, in scene units per second, means the total
// change over any stretch of wall-clock time is bounded by what a real pinch
// could have produced in that same stretch -- no matter how many stale
// samples a stall lets through to cover it. 12000/s is 1.5x MAX_PAN_SPEED:
// two fingers spreading is two flicks' worth of motion, at most.
static const float MAX_PINCH_DIST_SPEED = 12000.f; // scene units/s

static float clampPinchDistRate(float distDelta, float dt) {
	float rate = distDelta / dt;
	if (std::fabs(rate) > MAX_PINCH_DIST_SPEED) {
		TOUCH_WARN("Touch: pinch distance rate %.0f/s exceeds cap, clamping to %.0f/s "
			"(lift-off glitch or a processing stall, not a real pinch)",
			rate, std::copysign(MAX_PINCH_DIST_SPEED, rate));
		rate = std::copysign(MAX_PINCH_DIST_SPEED, rate);
	}
	return rate * dt;
}

struct TouchState {
	bool down = false;
	bool leftSent = false;
	bool longPressFired = false;
	double downTime = 0.0;
	rack::math::Vec downPos;
	rack::math::Vec lastPos;
	// When the finger last came to rest, and where. The long-press (context
	// menu) is timed from here, not from the down: a slow drag keeps nudging
	// this forward so it never fires -- moving a module must not pop its menu.
	double stillTime = 0.0;
	rack::math::Vec stillPos;

	// Multitouch gesture state
	bool gesture = false; // two-finger mode active
	rack::math::Vec lastCentroid;
	float lastDist = 0.f;
	double lastMoveTime = 0.0;
	rack::math::Vec panVelocity; // scene units/s, smoothed

	// One-finger drag on empty rack pans the view (instead of the selection
	// marquee, which now needs the toolbar's multi-select mode).
	bool panSingle = false;

	// Multi-select mode only: the module this press landed on. Rack never sees
	// the press, so the release can toggle the module's selection and a hold can
	// turn it into a move instead of a context menu.
	rack::app::ModuleWidget* selectTarget = NULL;

	// Pan inertia after the fingers lift
	bool inertiaActive = false;
	rack::math::Vec inertiaVel;
	rack::math::Vec inertiaCentroid;
	double inertiaTime = 0.0;
};

static TouchState st;

/** Patch lock (toolbar padlocks). 0 = off. 1 = layout lock: module drags
 * and port/cable touches are swallowed, params stay live. 2 = full lock:
 * every single-finger press on the canvas is swallowed -- the patch is
 * immutable and only two-finger pan/zoom (and the toolbar, a separate
 * window) still works. Session-only by design. */
static std::atomic<int> lockMode{0};

/** Multi-select mode (toolbar toggle). Off by default: one finger on empty rack
 * pans the view. On: one finger on empty rack draws Rack's selection marquee. */
static std::atomic<bool> multiSelect{false};

/** The module owning the freshly-hovered widget, or NULL on empty rack. Knobs
 * and ports are children of their ModuleWidget, so this walks up to it. */
static rack::app::ModuleWidget* hoveredModule() {
	for (rack::widget::Widget* w = APP->event->hoveredWidget; w; w = w->parent) {
		if (auto* mw = dynamic_cast<rack::app::ModuleWidget*>(w))
			return mw;
	}
	return NULL;
}

/** True when the freshly-hovered widget is something a single finger should act
 * on -- a module (its body, knobs and ports are children) or a cable plug. When
 * it is NOT, the press is on empty rack, where a one-finger drag pans instead of
 * starting the selection box. */
static bool overInteractive() {
	if (hoveredModule())
		return true;
	for (rack::widget::Widget* w = APP->event->hoveredWidget; w; w = w->parent) {
		if (dynamic_cast<rack::app::PlugWidget*>(w))
			return true;
	}
	return false;
}

/** Add or remove one module from Rack's selection. Render-thread only, which is
 * where every touch event is already dispatched from. */
static void toggleSelection(rack::app::ModuleWidget* mw) {
	if (!mw || !APP->scene || !APP->scene->rack)
		return;
	APP->scene->rack->select(mw, !APP->scene->rack->isSelected(mw));
}

/** Decide from the freshly-hovered widget whether this press is frozen.
 * Called after handleHover so APP->event->hoveredWidget is the touch
 * target. Layout lock: a press whose deepest target IS a ModuleWidget is a
 * module drag (controls hover as their own widgets, children of it) and a
 * press on/inside a PortWidget is a cable interaction -- both swallowed. */
static bool pressBlockedByLock() {
	int mode = lockMode.load(std::memory_order_relaxed);
	if (mode <= 0)
		return false;
	if (mode >= 2)
		return true;
	rack::widget::Widget* w = APP->event->hoveredWidget;
	if (!w)
		return false;
	for (rack::widget::Widget* p = w; p; p = p->parent) {
		if (dynamic_cast<rack::app::PortWidget*>(p))
			return true;
		if (dynamic_cast<rack::app::ModuleWidget*>(p))
			return p == w;
	}
	return false;
}


static void startInertia() {
	if (st.panVelocity.norm() >= INERTIA_MIN_SPEED) {
		st.inertiaActive = true;
		st.inertiaVel = st.panVelocity;
		st.inertiaCentroid = st.lastCentroid;
		st.inertiaTime = rack::system::getTime();
	}
	st.panVelocity = rack::math::Vec();
}


/** Slot whose parked cable end the finger is currently pulling out, or -1.
Lives here rather than in cable_park because it is touch state: the bar only
needs to know where to draw the cable. */
static int g_parkDrag = -1;


/** The port an in-flight Rack cable drag started from, or NULL. Rack keeps the
half-made cable in the rack widget; we only ever read it. */
static rack::app::PortWidget* incompleteCablePort() {
	if (!APP->scene || !APP->scene->rack)
		return NULL;
	std::vector<rack::app::CableWidget*> cables = APP->scene->rack->getIncompleteCables();
	if (!cables.empty()) {
		rack::app::CableWidget* cw = cables.back();
		rack::app::PortWidget* p = cw->inputPort ? cw->inputPort : cw->outputPort;
		if (p)
			return p;
	}
	// Dragging a cable that was ALREADY plugged in goes through the plug, not
	// through a freshly made incomplete cable, so the list above can be empty
	// while a cable end is very much in flight. Fall back to whatever widget
	// the drag actually started on.
	rack::widget::Widget* dragged = APP->event->getDraggedWidget();
	if (rack::app::PortWidget* pw = dynamic_cast<rack::app::PortWidget*>(dragged))
		return pw;
	LOGI("cablepark: nothing to park (%zu incomplete cables, dragged widget is %s)",
		cables.size(), dragged ? typeid(*dragged).name() : "nothing");
	return NULL;
}


static rack::math::Vec scenePos(float x, float y) {
	float ratio = APP->window ? APP->window->pixelRatio : 1.f;
	return rack::math::Vec(x, y).div(ratio);
}


static void sendLeftButton(rack::math::Vec pos, int action) {
	APP->event->handleButton(pos, GLFW_MOUSE_BUTTON_LEFT, action, 0);
}


static void endLeftDrag(rack::math::Vec pos) {
	if (st.leftSent) {
		sendLeftButton(pos, GLFW_RELEASE);
		st.leftSent = false;
	}
}


/** Snap a normal Rack cable release to the same compatible jack highlighted
 * by the parking overlay. Rack still owns the drag and creates the cable; we
 * only move its virtual cursor to a nearby compatible target before release. */
static rack::math::Vec snapCableRelease(rack::math::Vec pos) {
	if (!APP->scene || !APP->scene->rack)
		return pos;
	rack::app::PortWidget* from = NULL;
	std::vector<rack::app::CableWidget*> cables = APP->scene->rack->getIncompleteCables();
	if (!cables.empty()) {
		rack::app::CableWidget* cw = cables.back();
		from = cw->inputPort ? cw->inputPort : cw->outputPort;
	}
	if (!from)
		from = dynamic_cast<rack::app::PortWidget*>(APP->event->getDraggedWidget());
	if (!from)
		return pos;
	int want = from->type == rack::engine::Port::INPUT
		? rack::engine::Port::OUTPUT : rack::engine::Port::INPUT;
	rack::app::PortWidget* target = rackdroid::cableParkNearestPort(pos.x, pos.y, want);
	if (!target)
		return pos;
	rack::math::Vec snapped = target->getAbsoluteOffset(target->box.size.div(2.f));
	APP->event->handleHover(snapped, snapped.minus(st.lastPos));
	rackdroid::cableParkSetInflightPos(snapped.x, snapped.y);
	return snapped;
}


int touchHandleEvent(AInputEvent* event) {
	if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION)
		return 0;
	if (!APP->event || !APP->window)
		return 0;

	windowNoteInteraction();

	int32_t action = AMotionEvent_getAction(event);
	int32_t actionMasked = action & AMOTION_EVENT_ACTION_MASK;
	size_t pointerCount = AMotionEvent_getPointerCount(event);

	rack::math::Vec pos = scenePos(AMotionEvent_getX(event, 0), AMotionEvent_getY(event, 0));

	switch (actionMasked) {
		case AMOTION_EVENT_ACTION_DOWN: {
			st.down = true;
			st.gesture = false;
			st.longPressFired = false;
			st.inertiaActive = false; // any touch cancels coasting
			st.panVelocity = rack::math::Vec();
			st.downTime = rack::system::getTime();
			st.downPos = pos;
			st.lastPos = pos;
			st.stillTime = st.downTime;
			st.stillPos = pos;
			// Anchor the in-flight position at the press point right away. The
			// cable Rack creates on this press is checked against it before the
			// first MOVE arrives; without this it keeps last drag's stale value
			// and the spare hole flashes for a frame if that value was over the
			// bar (e.g. just after a park).
			rackdroid::cableParkSetInflightPos(pos.x, pos.y);
			// Tapping the bar's handle collapses it to just that handle, or
			// expands it again. Checked before the hole grab so the handle is
			// never read as a park interaction.
			if (rackdroid::cableParkArrowAt(pos.x, pos.y)) {
				rackdroid::cableParkToggleCollapsed();
				return 1;
			}
			// Pulling a parked cable end out of the bar: our own drag, Rack
			// must not see it at all or it would start panning the rack.
			{
				int slot = rackdroid::cableParkSlotAt(pos.x, pos.y);
				if (slot >= 0 && rackdroid::cableParkSlotFilled(slot)) {
					g_parkDrag = slot;
					rackdroid::cableParkSetDragging(slot, pos.x, pos.y);
					return 1;
				}
			}
			// Move the virtual cursor there first, then press.
			APP->event->handleHover(pos, rack::math::Vec());
			// Padlock active and the target is frozen: swallow the press.
			// leftSent stays false, so MOVE/UP/long-press all no-op, but
			// a second finger still enters pan/zoom as usual.
			if (pressBlockedByLock())
				return 1;
			// Multi-select mode, finger on a module: a selection gesture, not a
			// Rack interaction. Nothing is sent, so knobs, ports and the module
			// itself stay put; the release toggles the selection and a hold
			// (touchStep) turns it into a move. Empty rack still falls through
			// to the marquee below.
			if (multiSelect.load(std::memory_order_relaxed)) {
				if (rack::app::ModuleWidget* mw = hoveredModule()) {
					st.selectTarget = mw;
					// The hover above was only to find out what is under the
					// finger. Drop it again: left in place it pops the tooltip
					// of whatever knob happens to be there, which has nothing
					// to do with picking the module.
					APP->event->handleLeave();
					return 1;
				}
			}
			// Empty rack, normal mode: one finger pans the view. Sending the
			// left button here would start Rack's selection marquee instead --
			// that only happens now in the toolbar's multi-select mode.
			if (!multiSelect.load(std::memory_order_relaxed) && !overInteractive()) {
				st.panSingle = true;
				st.lastCentroid = pos;
				st.lastMoveTime = st.downTime;
				st.panVelocity = rack::math::Vec();
				return 1;
			}
			sendLeftButton(pos, GLFW_PRESS);
			st.leftSent = true;
			return 1;
		}

		case AMOTION_EVENT_ACTION_POINTER_DOWN: {
			// Second finger: abort the left drag / one-finger pan, enter
			// pan/zoom mode.
			if (pointerCount == 2) {
				endLeftDrag(st.lastPos);
				st.panSingle = false;
				st.selectTarget = NULL;
				st.gesture = true;
				st.inertiaActive = false;
				st.panVelocity = rack::math::Vec();
				rack::math::Vec p1 = scenePos(AMotionEvent_getX(event, 1), AMotionEvent_getY(event, 1));
				st.lastCentroid = pos.plus(p1).mult(0.5f);
				st.lastDist = pos.minus(p1).norm();
				st.lastMoveTime = rack::system::getTime();
			}
			return 1;
		}

		case AMOTION_EVENT_ACTION_MOVE: {
			// Multi-select: the finger left the module before the hold fired, so
			// this was never a tap on it. Hand the gesture to the view pan --
			// dragging must not move a module unless it was held first.
			if (st.selectTarget) {
				if (pos.minus(st.downPos).norm() > LONG_PRESS_SLOP_PX) {
					st.selectTarget = NULL;
					st.panSingle = true;
					st.lastCentroid = pos;
					st.lastMoveTime = rack::system::getTime();
					st.panVelocity = rack::math::Vec();
				}
				st.lastPos = pos;
				return 1;
			}
			// One-finger pan on empty rack: scroll the view, and track velocity
			// so the release can coast, exactly like the two-finger pan.
			if (st.panSingle) {
				rack::math::Vec delta = pos.minus(st.lastPos);
				APP->event->handleScroll(pos, delta);
				double now = rack::system::getTime();
				double dt = now - st.lastMoveTime;
				if (dt > 1e-4) {
					rack::math::Vec instV = clampPanVelocity(delta.div(dt));
					st.panVelocity = st.panVelocity.mult(0.5f).plus(instV.mult(0.5f));
				}
				st.lastCentroid = pos;
				st.lastMoveTime = now;
				st.lastPos = pos;
				return 1;
			}
			if (g_parkDrag >= 0) {
				// Hover so the port under the finger lights up as it would in a
				// normal Rack cable drag; the bar draws the cable itself.
				APP->event->handleHover(pos, pos.minus(st.lastPos));
				rackdroid::cableParkSetDragging(g_parkDrag, pos.x, pos.y);
				st.lastPos = pos;
				return 1;
			}
			if (st.gesture && pointerCount >= 2) {
				rack::math::Vec p1 = scenePos(AMotionEvent_getX(event, 1), AMotionEvent_getY(event, 1));
				rack::math::Vec centroid = pos.plus(p1).mult(0.5f);
				float dist = pos.minus(p1).norm();
				double now = rack::system::getTime();
				double dt = now - st.lastMoveTime;

				// Pinch → Ctrl+scroll (Rack's zoom gesture). Rate-limited
				// against dt (clampPinchDistRate), not merely clamped per
				// callback (clampPinchRatio, kept as a belt-and-braces cap
				// on the resulting ratio itself) -- see clampPinchDistRate's
				// comment for why a burst of queued samples needs the former.
				if (st.lastDist > 0.f && dt > 1e-4) {
					float distDelta = clampPinchDistRate(dist - st.lastDist, (float) dt);
					float ratio = clampPinchRatio((st.lastDist + distDelta) / st.lastDist - 1.f);
					// Zooming IN past the cap is simply not asked for: letting
					// it through and clamping afterwards would fight the
					// gesture once per frame. Zooming out is always fine.
					bool atCap = ratio > 0.f && APP->scene && APP->scene->rackScroll
						&& APP->scene->rackScroll->getZoom() >= MAX_RACK_ZOOM;
					if (std::fabs(ratio) > PINCH_DETECT_RATIO && !atCap) {
						windowSetMods(GLFW_MOD_CONTROL);
						APP->event->handleScroll(centroid, rack::math::Vec(0.f, ratio * PINCH_ZOOM_SPEED * 50.f));
						windowSetMods(0);
					}
				}
				// Two-finger pan → scroll
				rack::math::Vec delta = centroid.minus(st.lastCentroid);
				if (delta.norm() > 0.f)
					APP->event->handleScroll(centroid, delta);

				// Track panning velocity for release inertia (EMA).
				if (dt > 1e-4) {
					rack::math::Vec instV = clampPanVelocity(delta.div(dt));
					st.panVelocity = st.panVelocity.mult(0.5f).plus(instV.mult(0.5f));
				}
				st.lastMoveTime = now;
				st.lastCentroid = centroid;
				st.lastDist = dist;
				st.lastPos = pos;
				return 1;
			}

			if (st.down && st.leftSent) {
				rack::math::Vec delta = pos.minus(st.lastPos);
				APP->event->handleHover(pos, delta);
				// Tell the bar where an in-flight cable end is, so it only
				// reveals its spare hole once the cable is dragged over it.
				rackdroid::cableParkSetInflightPos(pos.x, pos.y);
				// Moved past a hair? Then the finger is dragging, not resting:
				// push the still-clock forward so the long-press never fires
				// mid-drag (a slow module move used to pop the context menu).
				if (pos.minus(st.stillPos).norm() > LONG_PRESS_STILL_PX) {
					st.stillTime = rack::system::getTime();
					st.stillPos = pos;
				}
				st.lastPos = pos;
			}
			return 1;
		}

		case AMOTION_EVENT_ACTION_POINTER_UP: {
			if (st.gesture && pointerCount == 2) {
				// Back to single-finger mode; don't resume the left drag.
				st.gesture = false;
				st.lastDist = 0.f;
				startInertia();
			}
			return 1;
		}

		case AMOTION_EVENT_ACTION_UP: {
			// Multi-select tap on a module: add it to the selection, or take it
			// out if it was already in. A hold already cleared selectTarget and
			// turned the gesture into a move, so it never lands here.
			if (st.selectTarget) {
				toggleSelection(st.selectTarget);
				st.selectTarget = NULL;
				st.down = false;
				st.gesture = false;
				return 1;
			}
			// One-finger pan released: coast, then done.
			if (st.panSingle) {
				st.panSingle = false;
				st.down = false;
				startInertia();
				return 1;
			}
			// Report the release point so a drop onto the spare hole still sees
			// it (visibleHoles keys the spare on the in-flight end being on the
			// bar, and this is the last position before the cable is discarded).
			rackdroid::cableParkSetInflightPos(pos.x, pos.y);
			// Dropping a parked end onto a port completes the cable.
			if (g_parkDrag >= 0) {
				rack::app::PortWidget* target =
					dynamic_cast<rack::app::PortWidget*>(APP->event->getHoveredWidget());
				// An incompatible jack under the finger is not an answer: keep
				// looking for one that could actually take this end, so landing
				// on an input while holding an input still finds the output
				// next to it.
				int want = rackdroid::cableParkSlotType(g_parkDrag) == 0 ? 1 : 0;
				if (target && target->type != want)
					target = NULL;
				if (!target) {
					// Landed beside the jack rather than on it: snap to the
					// nearest port that could actually take this end.
					target = rackdroid::cableParkNearestPort(pos.x, pos.y, want);
				}
				if (target)
					rackdroid::cableParkConnect(g_parkDrag, target);
				else if (pos.x <= rackdroid::cableParkRightEdge()) {
					// Let go again over the bar: a cancel. Keep it parked.
					LOGI("cablepark: pull-out cancelled, slot %d stays", g_parkDrag);
				}
				else {
					// Carried off and dropped in the rack void: discard it. An
					// end the user pulled out and abandoned is one they no longer
					// want -- leaving it in its hole is the "why is this still
					// here" that flash-and-keep used to cause.
					LOGI("cablepark: dropped in the void, discarding slot %d", g_parkDrag);
					rackdroid::cableParkClear(g_parkDrag);
				}
				rackdroid::cableParkSetDragging(-1, 0.f, 0.f);
				g_parkDrag = -1;
				st.down = false;
				st.gesture = false;
				return 1;
			}
			// Releasing an in-flight Rack cable drag over an empty hole parks
			// it: we only record which port it came from, then let Rack discard
			// its half-made cable as usual.
			rack::math::Vec releasePos = pos;
			if (!st.gesture && st.leftSent) {
				int slot = rackdroid::cableParkSlotAt(pos.x, pos.y);
				if (slot >= 0) {
					// Dropped on the bar: the end always goes to the first free
					// hole, so the holes stay packed from the top (drop anywhere,
					// no gaps). Every way of failing still has to say so on screen.
					int free = rackdroid::cableParkFirstFree();
					if (free < 0) {
						LOGI("cablepark: bar is full");
						rackdroid::cableParkFlashRefused(slot);
					}
					else if (rack::app::PortWidget* from = incompleteCablePort()) {
						rackdroid::cableParkStore(free, from);
					}
					else {
						// incompleteCablePort() already logged why.
						rackdroid::cableParkFlashRefused(free);
					}
				}
				else {
					releasePos = snapCableRelease(pos);
				}
			}
			if (!st.gesture) {
				endLeftDrag(releasePos);
				// Tap landed on a text field: no hardware keyboard on
				// Android, so edit it through a system prompt.
				rack::ui::TextField* field = dynamic_cast<rack::ui::TextField*>(APP->event->selectedWidget);
				if (field && !st.longPressFired) {
					std::string edited;
					std::string title = field->placeholder.empty() ? "Edit text" : field->placeholder;
					if (dialogPrompt(title, field->getText(), edited))
						field->setText(edited);
				}
			}
			st.down = false;
			st.gesture = false;
			return 1;
		}

		case AMOTION_EVENT_ACTION_CANCEL: {
			if (g_parkDrag >= 0) {
				rackdroid::cableParkSetDragging(-1, 0.f, 0.f);
				g_parkDrag = -1;
			}
			endLeftDrag(st.lastPos);
			st.down = false;
			st.gesture = false;
			st.panSingle = false;
			st.selectTarget = NULL;
			return 1;
		}

		default:
			return 0;
	}
}


void touchStep() {
	if (!APP->event || !APP->window)
		return;

	// Pan inertia: coast the rack after a two-finger flick.
	if (st.inertiaActive) {
		double now = rack::system::getTime();
		double dt = now - st.inertiaTime;
		st.inertiaTime = now;
		if (dt <= 0.0 || dt > 0.1)
			dt = 1.0 / 60.0; // clamp after stalls
		rack::math::Vec delta = st.inertiaVel.mult(dt);
		APP->event->handleScroll(st.inertiaCentroid, delta);
		st.inertiaVel = st.inertiaVel.mult(std::exp(-INERTIA_DECAY * dt));
		windowNoteInteraction(); // keep rendering at full rate while coasting
		if (st.inertiaVel.norm() < INERTIA_STOP_SPEED)
			st.inertiaActive = false;
	}

	// A hold on empty rack deliberately does nothing: one finger there pans the
	// view, and popping the module palette out from under a resting thumb was
	// not wanted. The palette has its own toolbar button, and the guide says so.
	//
	// Multi-select: holding a module starts moving it. The press Rack never saw
	// is sent now, so the following MOVEs drag the module exactly as they would
	// outside this mode. No context menu here -- while selecting, a hold means
	// "pick this up", and the menu would fight the gesture for the same finger.
	if (st.down && st.selectTarget && !st.gesture && !st.longPressFired) {
		double held = rack::system::getTime() - st.stillTime;
		float moved = st.lastPos.minus(st.downPos).norm();
		if (held >= LONG_PRESS_SECONDS && moved < LONG_PRESS_SLOP_PX) {
			st.longPressFired = true;
			st.selectTarget = NULL;
			APP->event->handleHover(st.lastPos, rack::math::Vec());
			sendLeftButton(st.lastPos, GLFW_PRESS);
			st.leftSent = true;
		}
	}

	// Long-press without movement → context menu (right click). Timed from when
	// the finger last came to rest, and only while it is still near the press
	// point -- so a drag (even a slow one) never opens the menu.
	if (st.down && st.leftSent && !st.gesture && !st.longPressFired) {
		double held = rack::system::getTime() - st.stillTime;
		float moved = st.lastPos.minus(st.downPos).norm();
		if (held >= LONG_PRESS_SECONDS && moved < LONG_PRESS_SLOP_PX) {
			st.longPressFired = true;
			// Tell the menu layer whether this menu belongs to a module, so a
			// module's preset Copy/Paste can be labelled apart from the rack's
			// and a text field's identically named rows.
			if (hoveredModule())
				menuExpectModuleMenu();
			else
				menuClearModuleMenu();
			// Release the left drag, then emit a right click at the position.
			endLeftDrag(st.lastPos);
			APP->event->handleButton(st.lastPos, GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS, 0);
			APP->event->handleButton(st.lastPos, GLFW_MOUSE_BUTTON_RIGHT, GLFW_RELEASE, 0);
		}
	}
}


// Toolbar padlocks (MainActivity): 0 unlocked, 1 layout lock, 2 full lock.
extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeSetLockMode(JNIEnv*, jobject, jint mode) {
	lockMode.store(mode, std::memory_order_relaxed);
}

// Toolbar multi-select toggle: on = one finger draws the selection marquee;
// off (default) = one finger pans the rack.
extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeSetMultiSelect(JNIEnv*, jobject, jboolean on) {
	multiSelect.store(on, std::memory_order_relaxed);
}


} // namespace rackdroid
