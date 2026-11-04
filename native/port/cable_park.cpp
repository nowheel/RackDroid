/* Cable parking bar -- see cable_park.hpp for what it is for.
 *
 * Implementation notes:
 *
 * A slot stores the port's IDENTITY (module id + port id + direction), never a
 * PortWidget*. The widget behind a port can be destroyed while the end waits
 * in the bar -- the user is free to delete the module, or reload the patch --
 * and a dangling pointer would be a use-after-free at connect time. Resolving
 * the id back to a widget on demand simply fails when the module is gone.
 *
 * The bar is a Scene child, not a RackWidget child: it must stay pinned to the
 * screen edge while the rack pans and zooms underneath it.
 */
#include <app/CableWidget.hpp>
#include <app/ModuleWidget.hpp>
#include <app/PortWidget.hpp>
#include <app/RackWidget.hpp>
#include <app/Scene.hpp>
#include <context.hpp>
#include <engine/Engine.hpp>
#include <engine/Module.hpp>
#include <widget/Widget.hpp>
#include <window/Window.hpp>
#include <asset.hpp>
#include <engine/PortInfo.hpp>
#include <plugin/Model.hpp>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>

#include <logger.hpp>
#include <system.hpp>
#include "cable_park.hpp"

#if __ANDROID__
#include <android/log.h>
#include <jni.h>
#define LOGI(...) do { __android_log_print(ANDROID_LOG_INFO, "rackdroid.cablepark", __VA_ARGS__); INFO(__VA_ARGS__); } while (0)
#else
#define LOGI(...) INFO(__VA_ARGS__)
#endif

using namespace rack;

namespace rackdroid {


// The bar starts with MIN_HOLES and grows a hole at a time, up to MAX_SLOTS, as
// they fill -- a spare hole is revealed while a cable is in flight and every
// visible hole is taken, so there is always somewhere to drop the next end.
static const int MAX_SLOTS = 10;
static const int MIN_HOLES = 3;
// Screen units (Rack's own, i.e. pre-pixelRatio) -- the Scene is laid out in
// them, so the bar keeps its size on every density.
static const float BAR_W = CABLE_PARK_BAR_W;
// Set from Java in pixels; read here in screen units. See cable_park.hpp.
static std::atomic<float> g_leftInsetPx{0.f};
static std::atomic<float> g_topInsetPx{0.f};
static float toUnits(float px) {
	if (px <= 0.f)
		return 0.f;
	float ratio = APP && APP->window && APP->window->pixelRatio > 0.f
		? APP->window->pixelRatio : 1.f;
	return px / ratio;
}
static float barLeft() { return toUnits(g_leftInsetPx.load(std::memory_order_relaxed)); }
static float barTop() { return toUnits(g_topInsetPx.load(std::memory_order_relaxed)); }
static const float HOLE_R = 13.f;
static const float HOLE_GAP = 46.f;

struct Slot {
	int64_t moduleId = -1;
	int portId = -1;
	int type = -1;              // engine::Port::INPUT / OUTPUT
	// Captured when parking: the module can be renamed or deleted while the
	// end waits, and the label should still say what you put in there.
	std::string moduleName;
	std::string portName;
	NVGcolor color = nvgRGB(0xC8, 0x98, 0x5C);
	bool filled() const { return moduleId >= 0; }
};

static Slot g_slots[MAX_SLOTS];
// Shown by default: the bar is up as soon as the app opens (the toolbar button
// and the bar's own handle toggle it from there).
static bool g_visible = true;

/** The bar's rectangle in window pixels, republished every frame by the render
thread for the interface tour to read from the UI thread. */
static std::atomic<int> g_boundsPx[4];
static std::atomic<bool> g_boundsValid{false};
// Collapsed to just its handle (the bar tucked away but not gone), toggled by
// the handle chevron. Separate from g_visible, which is the toolbar on/off.
static bool g_collapsed = false;
static int g_dragSlot = -1;
// Slot to flash red, and when it started: a refusal the user cannot see is
// indistinguishable from a feature that does not work.
static int g_refuseSlot = -1;
static double g_refuseTime = 0.0;
static float g_dragX = 0.f, g_dragY = 0.f;


// Where the in-flight cable end currently is, in screen units, reported by the
// touch layer while a left drag is active. Used to reveal the spare hole only
// once the cable is actually over the bar.
static float g_inflightX = 1e6f, g_inflightY = 0.f;

/** True while a cable end is being dragged that could be parked -- Rack keeps it
as an incomplete cable. Pulling a parked end back out is our own drag and leaves
no incomplete cable, so it does not count (you need no spare hole to pull out). */
static bool parkableCableInFlight() {
	return APP->scene && APP->scene->rack &&
		!APP->scene->rack->getIncompleteCables().empty();
}

/** Number of parked ends. Slots are kept packed (see cableParkClear), so the
filled ones are always 0..count-1 with no gaps. */
static int filledCount() {
	int n = 0;
	for (int i = 0; i < MAX_SLOTS; i++)
		if (g_slots[i].filled())
			n++;
	return n;
}

/** Holes the bar rests at: the parked count, never fewer than MIN_HOLES. This
is what the layout centres on, and what the bar shrinks back to as ends leave. */
static int restingHoles() {
	int n = filledCount();
	if (n < MIN_HOLES) n = MIN_HOLES;
	if (n > MAX_SLOTS) n = MAX_SLOTS;
	return n;
}

// The collapse/expand handle. Expanded, its chevron sits INSIDE the bar at the
// top (a cap above hole 0). Collapsed, only a glass pill with the chevron
// remains, at the same spot so it never jumps.
static const float PILL_W = 38.f;
static const float PILL_H = 30.f;
static const float HANDLE_CAP = 28.f; // room at the top of the bar for the chevron

/** How many holes fit in one column between the toolbar and the bottom of the
screen. A column of ten is 506 units tall; a landscape phone has about 415 to
begin with and the toolbar card takes a third of that, so past the fourth hole
the column simply ran off the bottom of the display. Portrait has room for all
ten, which is why this only ever splits the bar on its side.

The reserve at the bottom is a hole's radius: without it the last hole comes out
flush with the screen edge, which is both ugly and hard to hit. */
static int holesPerColumn() {
	float h = APP->scene ? APP->scene->box.size.y : 800.f;
	// A bar of n holes stands (n-1)*HOLE_GAP + 92 tall: the cap and its gap
	// above hole 0, and half a gap plus the end margin below the last one.
	float avail = h - barTop() - HOLE_R;
	int n = (int) std::floor((avail - 92.f) / HOLE_GAP) + 1;
	if (n < 1) n = 1;
	if (n > MAX_SLOTS) n = MAX_SLOTS;
	return n;
}

static int columnsFor(int holes) {
	int per = holesPerColumn();
	return (holes + per - 1) / per;
}

/** Columns the bar rests at, before any drag spare. Kept apart from
barColumns() because the spare depends on the cable being over the bar, and
"over the bar" depends on how wide the bar is -- asking barColumns() there would
be a question answering itself. */
static int restingColumns() { return columnsFor(restingHoles()); }

/** True when a parkable cable is being dragged AND its end is over the bar. The
spare hole is an invitation to drop right here, so it should only appear once the
user brings the cable onto the bar -- not every time any cable moves anywhere.

Measured against the width the bar would have WITH the spare, not the width it
has now. When the resting holes fill the last column exactly, the spare opens a
new one -- and reaching for it took the cable past the old right edge, which
turned the spare off, which took the column away again. The hole flickered out
from under the finger and the drop landed on nothing. */
static bool parkableOverBar() {
	if (!parkableCableInFlight())
		return false;
	int n = restingHoles();
	if (n < MAX_SLOTS)
		n++;
	return g_inflightX <= barLeft() + BAR_W * columnsFor(n);
}

/** How many holes to show right now: the resting count, plus one spare (up to
MAX_SLOTS) while a cable is dragged onto the bar and every resting hole is full
-- that extra hole is the drop target. */
static int visibleHoles() {
	int n = restingHoles();
	if (n < MAX_SLOTS && filledCount() == n && parkableOverBar())
		n++;
	return n;
}

/** Columns the bar currently needs, and how wide that makes it. The holes keep
their size when they run out of height -- shrinking them would be the wrong
trade on a touch screen -- so the bar grows sideways instead. */
static int barColumns() {
	int per = holesPerColumn();
	return (visibleHoles() + per - 1) / per;
}

static float barWidth() { return BAR_W * barColumns(); }

/** Vertical centre of hole `i`, in screen units. The resting holes are centred
on the screen, so the bar stays centred whatever the count; the drag spare (when
present) hangs just below them.

Centred, but never above the toolbar's bottom edge: in landscape the card takes
nearly a third of the screen, and a centred bar put its top cap -- the collapse
handle -- underneath it, where the tap lands on the card instead. Pushing the
whole bar down to clear it is the only move that keeps every hole reachable; it
only happens when the bar would otherwise run under the card, so in portrait
with a few holes nothing shifts at all. */
static float holeY(int i) {
	float h = APP->scene ? APP->scene->box.size.y : 800.f;
	int per = holesPerColumn();
	// Rows, not holes: a second column starts at the top again, so what the
	// layout centres on is the length of the longest column.
	int n = restingHoles();
	if (n > per) n = per;
	float first = h * 0.5f - (n - 1) * HOLE_GAP * 0.5f;
	float lowest = barTop() + HANDLE_CAP + 8.f + HOLE_GAP * 0.5f;
	if (first < lowest)
		first = lowest;
	return first + (i % per) * HOLE_GAP;
}

/** Horizontal centre of hole `i`: the second column sits one bar-width right of
the first, so the holes stay on the same grid the eye already learned. */
static float holeX(int i) {
	return barLeft() + BAR_W * 0.5f + (i / holesPerColumn()) * BAR_W;
}

/** The handle stays over the first column whatever the bar does. */
static float handleX() { return barLeft() + BAR_W * 0.5f; }

static float handleY() {
	return holeY(0) - HOLE_GAP * 0.5f - 8.f - HANDLE_CAP * 0.5f;
}

/** Draws the chevron inside the handle. `pointLeft` = "‹" (collapse, tuck the
bar to the edge); otherwise "›" (expand, pull it back out). Sized to match the
toolbar's own chevron: 13-unit arms (each ~13 px), rotated 90 deg since ours
points sideways rather than up. */
static void drawChevron(NVGcontext* vg, float cx, float cy, bool pointLeft) {
	float dx = pointLeft ? 2.f : -2.f;
	nvgBeginPath(vg);
	nvgMoveTo(vg, cx + dx, cy - 4.f);
	nvgLineTo(vg, cx - dx, cy);
	nvgLineTo(vg, cx + dx, cy + 4.f);
	nvgStrokeColor(vg, nvgRGBA(0xED, 0xE6, 0xD8, 0xE0));
	nvgStrokeWidth(vg, 1.6f);
	nvgLineCap(vg, NVG_ROUND);
	nvgLineJoin(vg, NVG_ROUND);
	nvgStroke(vg);
}

/** Draws the glass pill background (same smoked glass as the toolbar handle). */
static void drawPill(NVGcontext* vg, float cx, float cy) {
	nvgBeginPath(vg);
	nvgRoundedRect(vg, cx - PILL_W * 0.5f, cy - PILL_H * 0.5f, PILL_W, PILL_H, PILL_H * 0.5f);
	nvgFillColor(vg, nvgRGBA(0x1B, 0x18, 0x13, 0xE6));
	nvgFill(vg);
	nvgStrokeColor(vg, nvgRGBA(0xFF, 0xFF, 0xFF, 0x1E));
	nvgStrokeWidth(vg, 1.f);
	nvgStroke(vg);
}


static app::PortWidget* resolvePort(const Slot& s) {
	if (!s.filled() || !APP->scene || !APP->scene->rack)
		return NULL;
	app::ModuleWidget* mw = APP->scene->rack->getModule(s.moduleId);
	if (!mw)
		return NULL;
	return s.type == engine::Port::INPUT ? mw->getInput(s.portId) : mw->getOutput(s.portId);
}


/** Geomini if the assets are there, DejaVu otherwise -- same fallback the
panel label overlay uses. */
static int labelFont(const widget::Widget::DrawArgs& args) {
	static std::shared_ptr<window::Font> font;
	if (!font)
		font = APP->window->loadFont(rack::asset::system("res/fonts/Geomini.ttf"));
	if (!font || !font->handle)
		font = APP->window->loadFont(rack::asset::system("res/fonts/DejaVuSans.ttf"));
	return font && font->handle ? font->handle : 0;
}


/** Ring every port of `want` type that could take a cable end aimed at
 * (cx, cy), the nearest one emphasised. Shared by the parked-end drag and a
 * normal in-rack cable drag, so both give the same "here is where this fits"
 * feedback. Off-screen ports are culled -- a big patch has hundreds and this
 * runs every frame next to a live audio engine. */
static void highlightCompatiblePorts(NVGcontext* vg, math::Vec boxSize,
		int want, float cx, float cy, NVGcolor tint) {
	if (!APP->scene || !APP->scene->rack)
		return;
	app::PortWidget* pick = cableParkNearestPort(cx, cy, want);
	for (app::ModuleWidget* mw : APP->scene->rack->getModules()) {
		if (!mw)
			continue;
		for (app::PortWidget* pw : mw->getPorts()) {
			if (!pw || pw->type != want)
				continue;
			math::Vec c = pw->getAbsoluteOffset(pw->box.size.div(2.f));
			if (c.x < -20.f || c.y < -20.f ||
				c.x > boxSize.x + 20.f || c.y > boxSize.y + 20.f)
				continue;
			bool chosen = (pw == pick);
			float r = cableParkPortRadius(pw);
			nvgBeginPath(vg);
			nvgCircle(vg, c.x, c.y, chosen ? r * 1.7f : r * 1.15f);
			nvgStrokeColor(vg, nvgTransRGBA(tint, chosen ? 0xFF : 0x55));
			nvgStrokeWidth(vg, chosen ? r * 0.3f : r * 0.14f);
			nvgStroke(vg);
		}
	}
}


struct CableParkBar : widget::Widget {
	void step() override {
		// Follows rotation/resize here, where the context is guaranteed.
		if (APP->scene)
			box.size = APP->scene->box.size;
		// Republish the bar's rectangle for the interface tour, which reads it
		// from the UI thread and must never touch the context itself.
		cableParkPublishBounds();
		// Drop any parked end whose module has been deleted. It has nowhere to
		// return to, so leaving it in the hole just dangles a reference to a
		// module that no longer exists. Keyed on the module id, which survives a
		// patch reload (ids are restored from JSON), so a reload that destroys
		// and recreates the same modules does not clear these -- only a real
		// deletion (or New/empty patch) does. Widget stepping is single-threaded
		// and patch load is synchronous, so step() never runs mid-reload.
		if (APP->scene && APP->scene->rack) {
			// cableParkClear compacts, so a cleared slot is replaced by the one
			// above -- re-check the same index rather than stepping past it.
			int i = 0;
			while (i < MAX_SLOTS) {
				if (g_slots[i].filled() &&
					!APP->scene->rack->getModule(g_slots[i].moduleId)) {
					LOGI("slot %d module %lld deleted; clearing parked end",
						i, (long long) g_slots[i].moduleId);
					cableParkClear(i);
				} else {
					i++;
				}
			}
		}
		widget::Widget::step();
	}

	void draw(const DrawArgs& args) override {
		// A normal in-rack cable drag gets the same compatible-port highlight as
		// a parked end -- and it must show even when the bar is hidden or
		// collapsed, so it runs before those early returns. Rack keeps the
		// half-made cable as an incomplete cable; g_dragSlot < 0 excludes our own
		// parked-end pull-out (which has no incomplete cable and highlights below).
		if (g_dragSlot < 0 && APP->scene && APP->scene->rack) {
			std::vector<app::CableWidget*> inc = APP->scene->rack->getIncompleteCables();
			if (!inc.empty()) {
				app::CableWidget* cw = inc.back();
				int want = cw->inputPort ? engine::Port::OUTPUT
					: (cw->outputPort ? engine::Port::INPUT : -1);
				if (want >= 0)
					highlightCompatiblePorts(args.vg, box.size, want,
						g_inflightX, g_inflightY, cw->color);
			}
		}

		if (!g_visible)
			return;

		// Collapsed: nothing but the handle pill remains, its chevron pointing
		// out ("›") so it reads as "pull the bar back out".
		if (g_collapsed) {
			drawPill(args.vg, handleX(), handleY());
			drawChevron(args.vg, handleX(), handleY(), false);
			return;
		}

		int holes = visibleHoles();
		// Bar body: same smoked-glass slab as the Kotlin surfaces. The top cap
		// holds the close chevron INSIDE the bar (no protruding pill); the body
		// grows and shrinks with the visible holes below it. Its height is the
		// LONGEST column, not the last hole: a second column starts at the top
		// again, so measuring to hole n-1 would cut the slab short.
		int rows = holes < holesPerColumn() ? holes : holesPerColumn();
		float top = holeY(0) - HOLE_GAP * 0.5f - 8.f - HANDLE_CAP;
		float height = holeY(rows - 1) + HOLE_GAP * 0.5f + 10.f - top;
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, barLeft() + 4.f, top, barWidth() - 8.f, height, 16.f);
		nvgFillColor(args.vg, nvgRGBA(0x1B, 0x18, 0x13, 0xE0));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgRGBA(0xFF, 0xFF, 0xFF, 0x18));
		nvgStrokeWidth(args.vg, 1.f);
		nvgStroke(args.vg);

		// Close chevron, inside the cap, pointing in ("‹") to tuck the bar away.
		// A hairline below it separates the handle from the holes. Tap handled
		// in the touch layer.
		drawChevron(args.vg, handleX(), handleY(), true);
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, barLeft() + 12.f, handleY() + HANDLE_CAP * 0.5f - 1.f);
		nvgLineTo(args.vg, barLeft() + barWidth() - 12.f, handleY() + HANDLE_CAP * 0.5f - 1.f);
		nvgStrokeColor(args.vg, nvgRGBA(0xFF, 0xFF, 0xFF, 0x12));
		nvgStrokeWidth(args.vg, 1.f);
		nvgStroke(args.vg);

		for (int i = 0; i < holes; i++) {
			float cx = holeX(i), cy = holeY(i);
			// Jack: dark bore with a metal ring, mirroring the panel art so the
			// holes read as sockets rather than as buttons.
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, cx, cy, HOLE_R);
			nvgFillColor(args.vg, nvgRGB(0x14, 0x12, 0x0E));
			nvgFill(args.vg);
			float refuse = 0.f;
			if (i == g_refuseSlot) {
				double age = rack::system::getTime() - g_refuseTime;
				refuse = age < 1.0 ? (float) (1.0 - age) : 0.f;
			}
			// A slot being dragged out has left its hole -- the end is on the
			// finger now, so the hole must read as empty. Drawing the resting
			// plug here as well as the in-flight cable below is what made a
			// pulled-out end look doubled.
			bool resting = g_slots[i].filled() && i != g_dragSlot;
			NVGcolor ring = resting
				? g_slots[i].color : nvgRGBA(0x8A, 0x81, 0x73, 0x90);
			if (refuse > 0.f)
				ring = nvgLerpRGBA(ring, nvgRGB(0xE0, 0x50, 0x40), refuse);
			nvgStrokeColor(args.vg, ring);
			nvgStrokeWidth(args.vg, resting ? 2.5f + refuse * 2.f : 1.5f);
			nvgStroke(args.vg);

			if (resting) {
				// Plug sitting in the hole.
				nvgBeginPath(args.vg);
				nvgCircle(args.vg, cx, cy, HOLE_R * 0.45f);
				nvgFillColor(args.vg, g_slots[i].color);
				nvgFill(args.vg);
			}
		}

		// Cable still attached to the port it came from. Drawn every frame from
		// the port's CURRENT position, so it follows the rack as you pan and
		// zoom -- that motion is the whole point: it shows you where the end
		// belongs while you go looking for its destination.
		nvgFontSize(args.vg, 8.f);
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		for (int i = 0; i < MAX_SLOTS; i++) {
			// The dragged slot's cable is drawn to the finger below, not to its
			// hole -- skip its resting stub and label so it does not read as a
			// second cable branching from the hole.
			if (!g_slots[i].filled() || i == g_dragSlot)
				continue;
			float cx = holeX(i), cy = holeY(i);
			app::PortWidget* pw = resolvePort(g_slots[i]);
			if (pw) {
				math::Vec e = pw->getAbsoluteOffset(pw->box.size.div(2.f));
				nvgBeginPath(args.vg);
				nvgMoveTo(args.vg, cx, cy);
				nvgQuadTo(args.vg, (cx + e.x) * 0.5f, (cy + e.y) * 0.5f + 45.f, e.x, e.y);
				nvgStrokeColor(args.vg, nvgTransRGBA(g_slots[i].color, 0xB0));
				nvgStrokeWidth(args.vg, 4.f);
				nvgLineCap(args.vg, NVG_ROUND);
				nvgStroke(args.vg);
			}
			// Which module and which jack is waiting here. Only while the bar is
			// a single column: the labels are placed by their hole's row, and
			// once a second column starts the rows repeat, so two ends would
			// write their names over each other. There is no room beside a
			// landscape bar for ten of them stacked either -- the screen is
			// shorter than the list. What tells the ends apart there is what
			// told them apart all along: each keeps its colour, and the stub
			// above is drawn live from the hole to the port it came from.
			if (restingColumns() > 1)
				continue;
			// Off the RESTING width, not the current one: a drag spare can open
			// a second column for a moment, and the labels sliding right and
			// back as the finger passes over the bar is pure noise.
			float tx = barLeft() + BAR_W + 8.f;
			nvgFontFaceId(args.vg, labelFont(args));
			nvgFillColor(args.vg, nvgRGBA(0, 0, 0, 190));
			nvgText(args.vg, tx + 0.4f, cy - 5.f + 0.4f, g_slots[i].moduleName.c_str(), NULL);
			nvgFillColor(args.vg, nvgRGB(0xED, 0xE6, 0xD8));
			nvgText(args.vg, tx, cy - 5.f, g_slots[i].moduleName.c_str(), NULL);
			if (!g_slots[i].portName.empty()) {
				std::string sub = g_slots[i].portName +
					(g_slots[i].type == engine::Port::INPUT ? " in" : " out");
				nvgFillColor(args.vg, nvgRGBA(0, 0, 0, 190));
				nvgText(args.vg, tx + 0.4f, cy + 6.f + 0.4f, sub.c_str(), NULL);
				nvgFillColor(args.vg, g_slots[i].color);
				nvgText(args.vg, tx, cy + 6.f, sub.c_str(), NULL);
			}
		}

		// While a parked end is in flight, light up every jack that could take
		// it. Without this the user aims blind and only learns the answer from
		// the red flash AFTER letting go -- every bug report on this feature so
		// far was a correct refusal that simply could not be seen coming.
		if (g_dragSlot >= 0 && g_slots[g_dragSlot].filled() && APP->scene->rack) {
			int want = g_slots[g_dragSlot].type == engine::Port::INPUT
				? engine::Port::OUTPUT : engine::Port::INPUT;
			highlightCompatiblePorts(args.vg, box.size, want,
				g_dragX, g_dragY, g_slots[g_dragSlot].color);
		}

		// The end being pulled out, drawn as ONE cable from its fixed source
		// port to the finger. It starts at the source, not the hole: the loose
		// end has lifted OUT of the hole and is following the finger, so the
		// hole is empty and there is a single strand, not two meeting at the
		// hole (which is what read as the cable "doubling").
		if (g_dragSlot >= 0 && g_slots[g_dragSlot].filled()) {
			app::PortWidget* pw = resolvePort(g_slots[g_dragSlot]);
			math::Vec start = pw
				? pw->getAbsoluteOffset(pw->box.size.div(2.f))
				: math::Vec(holeX(g_dragSlot), holeY(g_dragSlot));
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, start.x, start.y);
			// Slack in the middle, like Rack's own cables.
			float mx = (start.x + g_dragX) * 0.5f;
			float my = (start.y + g_dragY) * 0.5f + 40.f;
			nvgQuadTo(args.vg, mx, my, g_dragX, g_dragY);
			nvgStrokeColor(args.vg, g_slots[g_dragSlot].color);
			nvgStrokeWidth(args.vg, 5.f);
			nvgLineCap(args.vg, NVG_ROUND);
			nvgStroke(args.vg);
		}
	}
};

static CableParkBar* g_bar = NULL;


void installCableParkBar() {
	if (g_bar || !APP->scene)
		return;
	g_bar = new CableParkBar;
	g_bar->box.pos = math::Vec(0, 0);
	g_bar->box.size = APP->scene->box.size;
	APP->scene->addChild(g_bar);
}


/** Callable from ANY thread, including Java's UI thread via JNI: it touches
plain state only. Rack's context is per-thread (see contextSet's own docs), so
APP is NULL on any thread that never had one set -- reading APP->scene here
dereferenced null and took the whole app down. Anything needing the context is
done by the widget below, which runs on the render thread. */
void cableParkSetVisible(bool visible) {
	g_visible = visible;
	if (!visible)
		g_dragSlot = -1;
	else
		g_collapsed = false; // re-enabling from the toolbar always opens it
}

bool cableParkVisible() { return g_visible; }

/** True if a screen position is on the collapse/expand handle (in either
state, since the handle stays put). */
bool cableParkArrowAt(float x, float y) {
	if (!g_visible)
		return false;
	float cx = handleX(), cy = handleY();
	return std::fabs(x - cx) <= PILL_W * 0.5f + 4.f &&
		std::fabs(y - cy) <= HANDLE_CAP * 0.5f + 4.f;
}

void cableParkToggleCollapsed() {
	g_collapsed = !g_collapsed;
	if (g_collapsed)
		g_dragSlot = -1;
}


int cableParkSlotAt(float x, float y) {
	if (!g_visible || g_collapsed || x > barLeft() + barWidth())
		return -1;
	// Only the holes currently on screen are droppable; the spare that appears
	// while parking is included by visibleHoles() so it can be dropped onto.
	int holes = visibleHoles();
	for (int i = 0; i < holes; i++) {
		float dy = y - holeY(i);
		float dx = x - holeX(i);
		// Generous radius: fingers are not mouse pointers.
		if (dx * dx + dy * dy <= (HOLE_R + 12.f) * (HOLE_R + 12.f))
			return i;
	}
	return -1;
}

bool cableParkSlotFilled(int slot) {
	return slot >= 0 && slot < MAX_SLOTS && g_slots[slot].filled();
}


bool cableParkStore(int slot, app::PortWidget* port) {
	if (slot < 0 || slot >= MAX_SLOTS || !port || !port->module)
		return false;
	if (g_slots[slot].filled())
		return false;
	g_slots[slot].moduleId = port->module->id;
	g_slots[slot].portId = port->portId;
	g_slots[slot].type = port->type;
	g_slots[slot].moduleName = port->module->model ? port->module->model->name : "?";
	{
		engine::PortInfo* info = port->getPortInfo();
		g_slots[slot].portName = info ? info->getName() : "";
	}
	// Claim the colour now, not at connect time. Two ends waiting in identical
	// tan drew as the same cable twice, and the colour the bar showed was not
	// the colour the finished cable got.
	if (APP->scene && APP->scene->rack)
		g_slots[slot].color = APP->scene->rack->getNextCableColor();
	LOGI("parked %s port %d of module %lld in slot %d",
		port->type == engine::Port::INPUT ? "input" : "output",
		port->portId, (long long) port->module->id, slot);
	return true;
}


int cableParkSlotType(int slot) {
	if (slot < 0 || slot >= MAX_SLOTS || !g_slots[slot].filled())
		return -1;
	return g_slots[slot].type;
}


float cableParkPortRadius(app::PortWidget* pw) {
	math::Vec o = pw->getAbsoluteOffset(math::Vec(0.f, 0.f));
	math::Vec e = pw->getAbsoluteOffset(pw->box.size);
	float r = (e.x - o.x) * 0.5f;
	return r > 3.f ? r : 3.f;
}


app::PortWidget* cableParkNearestPort(float x, float y, int wantType) {
	if (!APP->scene || !APP->scene->rack)
		return NULL;
	app::PortWidget* best = NULL;
	float bestDist = -1.f;
	for (app::ModuleWidget* mw : APP->scene->rack->getModules()) {
		if (!mw)
			continue;
		for (app::PortWidget* pw : mw->getPorts()) {
			if (!pw || pw->type != wantType)
				continue;
			// Centre of the jack in screen units.
			math::Vec c = pw->getAbsoluteOffset(pw->box.size.div(2.f));
			float dx = c.x - x, dy = c.y - y;
			float d = dx * dx + dy * dy;
			float reach = cableParkSnapRadius(pw);
			if (d > reach * reach)
				continue;
			if (bestDist < 0.f || d < bestDist) {
				bestDist = d;
				best = pw;
			}
		}
	}
	return best;
}


bool cableParkConnect(int slot, app::PortWidget* target) {
	if (slot < 0 || slot >= MAX_SLOTS || !target || !target->module)
		return false;
	const Slot& s = g_slots[slot];
	if (!s.filled())
		return false;
	// A cable needs one input end and one output end.
	if (s.type == target->type) {
		// A cable runs from an output to an input; two of the same cannot pair.
		LOGI("refused: both ends are %s", s.type == engine::Port::INPUT ? "inputs" : "outputs");
		g_refuseSlot = slot;
		g_refuseTime = rack::system::getTime();
		return false;
	}
	app::PortWidget* parked = resolvePort(s);
	if (!parked) {
		LOGI("parked module %lld is gone; dropping slot %d", (long long) s.moduleId, slot);
		cableParkClear(slot);
		return false;
	}
	app::PortWidget* in = s.type == engine::Port::INPUT ? parked : target;
	app::PortWidget* out = s.type == engine::Port::OUTPUT ? parked : target;
	if (APP->scene->rack->getCable(out, in)) {
		// The link the user is dropping this end onto already exists, so the
		// end has effectively reached its destination. Empty the hole rather
		// than flashing and keeping it: a parked end that will not leave the
		// bar even though its cable is right there reads as a duplicate.
		LOGI("slot %d target already connected; clearing", slot);
		cableParkClear(slot);
		return true;
	}
	app::CableWidget* cw = new app::CableWidget;
	// Rack picks the colour when the USER drags a cable; building one by hand
	// skips that, and the default-constructed NVGcolor drew as a dead grey wire
	// that looked disabled. Reuse the colour claimed at park time so the cable
	// the user was looking at in the bar is the cable they end up with.
	cw->color = s.color;
	cw->inputPort = in;
	cw->outputPort = out;
	// Registers the cable with the engine from the two ports.
	cw->updateCable();
	if (!cw->isComplete()) {
		delete cw;
		return false;
	}
	APP->scene->rack->addCable(cw);
	cableParkClear(slot);
	LOGI("connected slot %d", slot);
	return true;
}


void cableParkFlashRefused(int slot) {
	if (slot < 0 || slot >= MAX_SLOTS)
		return;
	g_refuseSlot = slot;
	g_refuseTime = rack::system::getTime();
}


void cableParkClear(int slot) {
	if (slot < 0 || slot >= MAX_SLOTS)
		return;
	// Compact: pull the holes above down into the gap so the filled ones stay
	// contiguous from the top. Without this, clearing a middle end left a hole
	// the bar could never shed, and removing ends never shrank it back to three.
	for (int i = slot; i < MAX_SLOTS - 1; i++)
		g_slots[i] = g_slots[i + 1];
	g_slots[MAX_SLOTS - 1] = Slot();
	// Transient indices follow the shift.
	if (g_dragSlot == slot) g_dragSlot = -1;
	else if (g_dragSlot > slot) g_dragSlot--;
	if (g_refuseSlot == slot) g_refuseSlot = -1;
	else if (g_refuseSlot > slot) g_refuseSlot--;
}

int cableParkFirstFree() {
	for (int i = 0; i < MAX_SLOTS; i++)
		if (!g_slots[i].filled())
			return i;
	return -1;
}


void cableParkSetDragging(int slot, float x, float y) {
	g_dragSlot = slot;
	g_dragX = x;
	g_dragY = y;
}

int cableParkDraggingSlot() { return g_dragSlot; }

void cableParkSetInflightPos(float x, float y) {
	g_inflightX = x;
	g_inflightY = y;
}


void cableParkSetLeftInsetPx(float px) {
	g_leftInsetPx.store(px > 0.f ? px : 0.f, std::memory_order_relaxed);
}


void cableParkSetTopInsetPx(float px) {
	g_topInsetPx.store(px > 0.f ? px : 0.f, std::memory_order_relaxed);
}


float cableParkRightEdge() { return barLeft() + barWidth(); }


float cableParkTopInset() { return barTop(); }


void cableParkPublishBounds() {
	// Render thread only: holeY() and pixelRatio both read the Rack context,
	// which is thread-local. Java asks for this from the UI thread, so the
	// answer has to be sitting in plain atomics by the time it does.
	if (!g_visible) {
		g_boundsValid.store(false, std::memory_order_release);
		return;
	}
	float x, y, w, h;
	if (g_collapsed) {
		x = handleX() - PILL_W * 0.5f;
		y = handleY() - PILL_H * 0.5f;
		w = PILL_W;
		h = PILL_H;
	}
	else {
		int holes = visibleHoles();
		if (holes <= 0) {
			g_boundsValid.store(false, std::memory_order_release);
			return;
		}
		// Same geometry the draw pass uses, so a spotlight over the bar lands
		// on exactly what is on screen.
		int rows = holes < holesPerColumn() ? holes : holesPerColumn();
		float top = holeY(0) - HOLE_GAP * 0.5f - 8.f - HANDLE_CAP;
		x = barLeft() + 4.f;
		y = top;
		w = barWidth() - 8.f;
		h = holeY(rows - 1) + HOLE_GAP * 0.5f + 10.f - top;
	}
	float ratio = APP && APP->window ? APP->window->pixelRatio : 1.f;
	g_boundsPx[0].store((int) (x * ratio), std::memory_order_relaxed);
	g_boundsPx[1].store((int) (y * ratio), std::memory_order_relaxed);
	g_boundsPx[2].store((int) ((x + w) * ratio), std::memory_order_relaxed);
	g_boundsPx[3].store((int) ((y + h) * ratio), std::memory_order_relaxed);
	g_boundsValid.store(true, std::memory_order_release);
}


bool cableParkBoundsPx(int* out4) {
	if (!g_boundsValid.load(std::memory_order_acquire))
		return false;
	for (int i = 0; i < 4; i++)
		out4[i] = g_boundsPx[i].load(std::memory_order_relaxed);
	return true;
}


} // namespace rackdroid


#if __ANDROID__
extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeSetCableParkVisible(JNIEnv*, jobject, jboolean visible) {
	rackdroid::cableParkSetVisible(visible);
}

/** The window draws under the display cutout so the rack reaches every edge;
 * this is how far in from the left the camera reaches, so the bar can start
 * past it. Java is the only side that can ask -- the NDK exposes no cutout. */
extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeCableParkLeftInset(JNIEnv*, jobject, jint px) {
	rackdroid::cableParkSetLeftInsetPx((float) px);
}

/** Bottom edge of the toolbar card, so the bar can keep its collapse handle out
 * from under it. Republished whenever the card is laid out -- collapsing it
 * gives the bar the room back. */
extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeCableParkTopInset(JNIEnv*, jobject, jint px) {
	rackdroid::cableParkSetTopInsetPx((float) px);
}

/** The bar's on-screen rectangle as {left, top, right, bottom} window pixels,
 * or null when it is not showing. The interface tour spotlights it, and the bar
 * is drawn by the render thread -- Java has no view to measure. Scene units are
 * window pixels divided by pixelRatio, the same conversion the touch layer
 * applies in reverse. */
extern "C" JNIEXPORT jintArray JNICALL
Java_org_rackdroid_MainActivity_nativeCableParkBounds(JNIEnv* env, jobject) {
	int px[4];
	if (!rackdroid::cableParkBoundsPx(px))
		return NULL;
	jint values[4] = { px[0], px[1], px[2], px[3] };
	jintArray out = env->NewIntArray(4);
	if (!out)
		return NULL;
	env->SetIntArrayRegion(out, 0, 4, values);
	return out;
}
#endif

