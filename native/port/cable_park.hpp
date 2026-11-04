#pragma once

namespace rack { namespace app { struct PortWidget; } namespace math { struct Vec; } }

namespace rackdroid {

/** Cable parking bar.
 *
 * On a phone you cannot pan the rack while dragging a cable, so wiring two
 * modules that are not on screen together at the same time is close to
 * impossible. This bar gives a cable end somewhere to wait: drop it in one of
 * the holes, pan wherever you like, then drag it out onto the target port.
 *
 * Three holes to start with, growing one at a time as they fill, up to ten --
 * enough for the "route a voice through several modules" case without the bar
 * eating the screen when it is empty. Past what fits in a column it takes a
 * second one rather than shrinking the holes.
 *
 * Everything here lives in the port layer -- Rack's own drag is left alone
 * and only observed, so no upstream file is touched. */

/** How far from a jack a drop still counts as hitting it, as a multiple of that
jack's radius ON SCREEN. It has to be relative: a fixed distance in scene units
is right at one zoom level only, and once the rack is zoomed out far enough that
jacks are closer together than the snap distance, every drop lands on whichever
jack happens to be nearest -- and the highlight drawn with the same fixed radius
covers the panel in overlapping rings. */
static const float CABLE_PARK_SNAP_JACKS = 2.2f;
/** Floor, in scene units: a fingertip cannot aim better than this however far
out the rack is zoomed. */
static const float CABLE_PARK_SNAP_MIN = 10.f;

/** Width of ONE column of the bar, in screen units. */
static const float CABLE_PARK_BAR_W = 46.f;

/** Right edge of the bar as it stands, in screen units, cutout inset included.
Shared so the touch layer can tell "over the bar" (a cancel / a drop target)
from "in the rack void" (a discard). Not a constant: a landscape screen has no
room for ten holes in a column, so past the fourth the bar takes a second one
and gets wider. Render thread only -- it reads pixelRatio and the scene. */
float cableParkRightEdge();

/** How far in from the left edge of the window the bar has to start, in screen
units. Zero everywhere except under a display cutout: the window draws under
the camera so the rack fills the whole screen, and the bar -- the one piece of
chrome that lives on that edge -- is pushed past it, or a punch-hole eats the
middle jack. Set it in PIXELS from Java (the only side that knows the inset);
the conversion happens on the render thread, where pixelRatio is readable. */
void cableParkSetLeftInsetPx(float px);

/** How far down the toolbar card reaches, in pixels. The bar centres itself on
the screen, and in landscape the card takes nearly a third of the height, so the
bar's top cap -- the collapse handle -- slid underneath it and could not be
tapped. The bar is pushed down only when it would otherwise start above this;
where there is room to centre, nothing moves. */
void cableParkSetTopInsetPx(float px);
/** That same edge in screen units: where the rack stops being covered. */
float cableParkTopInset();

/** Adds the bar widget to the scene. Call once, after the Scene exists. */
void installCableParkBar();

void cableParkSetVisible(bool visible);
bool cableParkVisible();

/** Index of the hole under a screen position, or -1. */
int cableParkSlotAt(float x, float y);

/** True if a screen position is on the bar's collapse/expand handle. */
bool cableParkArrowAt(float x, float y);

/** Collapse the bar to just its handle, or expand it again. */
void cableParkToggleCollapsed();
/** True if that hole currently holds a cable end. */
bool cableParkSlotFilled(int slot);

/** Lowest empty slot index, or -1 if the bar is full. Parking always targets
this so the holes stay packed from the top. */
int cableParkFirstFree();

/** Records the port an in-flight cable drag came from into `slot`.
Returns false if the slot is taken or there is no cable being dragged. */
bool cableParkStore(int slot, rack::app::PortWidget* port);

/** Direction of the end parked in `slot`: engine::Port::INPUT (0) or
OUTPUT (1), or -1 when the slot is empty. */
int cableParkSlotType(int slot);

/** On-screen radius of a jack in scene units — `box.size` is in rack-local
units, so it shrinks with the rack's zoom while a hardcoded radius does not. */
float cableParkPortRadius(rack::app::PortWidget* pw);

/** How far a drop may land from THIS jack and still connect to it. Shared so
the highlight drawn during the drag and the port actually chosen on release use
the SAME reach — if they diverge the bar lights up a target it will not connect
to, which is worse than no highlight at all. */
inline float cableParkSnapRadius(rack::app::PortWidget* pw) {
	float r = cableParkPortRadius(pw) * CABLE_PARK_SNAP_JACKS;
	return r > CABLE_PARK_SNAP_MIN ? r : CABLE_PARK_SNAP_MIN;
}

/** Nearest port of `wantType` (engine::Port::INPUT/OUTPUT) within snapping
reach of a screen position, or NULL. A jack is a few units across and a
fingertip is not: without this, a drop that looks on target lands on the module
body and nothing happens. */
rack::app::PortWidget* cableParkNearestPort(float x, float y, int wantType);

/** Connects the end parked in `slot` to `target`, clearing the slot.
Returns false if the two ports cannot form a cable (same direction, missing
module, already connected). */
bool cableParkConnect(int slot, rack::app::PortWidget* target);

/** Flash a hole red: the drop was refused or found nothing. Feedback the user
can see, since the reason otherwise only exists in the log. */
void cableParkFlashRefused(int slot);

void cableParkClear(int slot);

/** While the user drags a parked end out of the bar, so the bar can draw the
line to the finger. Pass -1 to stop. */
void cableParkSetDragging(int slot, float x, float y);
int cableParkDraggingSlot();

/** Report where an in-flight Rack cable end currently is (screen units), so the
bar can reveal its spare hole only once the cable is dragged over it. */
void cableParkSetInflightPos(float x, float y);

/** Republish the bar's rectangle for the interface tour. Render thread only:
the geometry and pixelRatio both read Rack's thread-local context. */
void cableParkPublishBounds();

/** The last published rectangle as {left, top, right, bottom} window pixels,
false while the bar is not showing. Safe from any thread -- it only reads
atomics, which is what the UI thread needs. */
bool cableParkBoundsPx(int* out4);

} // namespace rackdroid
