/* Runtime parameter/port labels.
 *
 * Upstream module panels bake their control labels ("FREQ", "CV", "GATE"...)
 * as vector paths in the panel SVG. RackDroid ships original panels without
 * those paths (the VCV panel art is CC BY-NC-ND and can't be reused in a
 * commercial build), so labels would be missing.
 *
 * This overlay redraws them from data the engine already owns — each
 * ParamQuantity's and PortInfo's `name` (GPLv3 code, not VCV artwork) — for
 * every module in the rack, so all plugins (Fundamental, Bogaudio, Core,
 * future ones) get labels automatically with no per-panel work.
 *
 * It is a single Widget added as a child of the RackWidget, drawing in rack
 * coordinate space; per-control positions come from getRelativeOffset().
 */
#include <string>
#include <set>
#include <algorithm>
#include <cmath>

#include <nanovg.h>

#include <context.hpp>
#include <asset.hpp>
#include <window/Window.hpp>
#include <widget/Widget.hpp>
#include <app/Scene.hpp>
#include <app/RackWidget.hpp>
#include <app/ModuleWidget.hpp>
#include <app/ParamWidget.hpp>
#include <app/PortWidget.hpp>
#include <engine/ParamQuantity.hpp>
#include <engine/PortInfo.hpp>
#include <engine/Module.hpp>
#include <widget/event.hpp>
#include <system.hpp>
#include <plugin/Model.hpp>
#include <plugin/Plugin.hpp>

#include "label_overlay.hpp"

using namespace rack;


namespace rackdroid {


/* Freshly-added modules (native browser tap): drawn with a decaying
 * expanding glow for POP_SECONDS. Render-thread only, tiny ring buffer. */
static const double POP_SECONDS = 0.7;
static struct { long long id; double time; } g_added[8];
static int g_addedNext = 0;

void noteModuleAdded(long long moduleId) {
	g_added[g_addedNext % 8] = {moduleId, system::getTime()};
	g_addedNext++;
}


static std::string shortLabel(const std::string& name) {
	// First word, uppercased, capped — mirrors the terse panel labels.
	std::string s = name.substr(0, name.find(' '));
	if (s.size() > 9)
		s = s.substr(0, 9);
	std::transform(s.begin(), s.end(), s.begin(), ::toupper);
	return s;
}


struct PanelLabelOverlay : widget::Widget {
	void draw(const DrawArgs& args) override {
		if (!APP->scene || !APP->scene->rack || !APP->window)
			return;
		// Geomini (the app typeface); DejaVu if it ever goes missing.
		std::shared_ptr<window::Font> font =
			APP->window->loadFont(asset::system("res/fonts/Geomini.ttf"));
		if (!font)
			font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
		if (!font)
			return;

		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, 6.5f);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);

		widget::Widget* ref = APP->scene->rack;

		// Module currently being dragged. Only when the drag target IS the
		// ModuleWidget itself (finger on the panel body): knob/port drags
		// target their own child widgets and must not light the module up.
		app::ModuleWidget* draggedModule =
			dynamic_cast<app::ModuleWidget*>(APP->event->getDraggedWidget());
		double now = system::getTime();

		for (app::ModuleWidget* mw : APP->scene->rack->getModules()) {
			// Skip the whole module before touching its params and ports.
			// drawLabel() already culls each label, but only after the caller
			// has walked the widget hierarchy (getRelativeOffset) once per
			// param and once per port to find out where it would have gone --
			// for every module in the patch, on every frame. Measured on an
			// S22: 3.9 ms a frame with 75 modules, 23% of a 60 Hz budget,
			// nearly all of it computing positions for labels off screen. One
			// offset and one rectangle test per module answers the same
			// question. MARGIN covers the labels that hang below a module's
			// bottom edge, which must not vanish when the panel itself is only
			// just out of view.
			static const float MARGIN = 24.f;
			math::Vec mpos = mw->getRelativeOffset(math::Vec(0, 0), ref);
			math::Rect mbox = math::Rect(mpos, mw->box.size).grow(math::Vec(MARGIN, MARGIN));
			if (!mbox.intersects(args.clipBox))
				continue;
			// "Lift" glow while a module is dragged; expanding pop glow
			// right after it lands from the browser.
			float glowAlpha = 0.f, glowInflate = 0.f;
			if (mw == draggedModule) {
				glowAlpha = 0.55f;
				glowInflate = 5.f;
			}
			else if (mw->module) {
				for (const auto& a : g_added) {
					if (a.id == mw->module->id && now - a.time < POP_SECONDS) {
						float t = (float) ((now - a.time) / POP_SECONDS);
						glowAlpha = 0.7f * (1.f - t) * (1.f - t);
						glowInflate = 4.f + t * 26.f;
						break;
					}
				}
			}
			if (glowAlpha > 0.f) {
				math::Vec p = mw->getRelativeOffset(math::Vec(0, 0), ref);
				for (int i = 0; i < 3; i++) {
					float inf = glowInflate + i * 3.f;
					nvgBeginPath(args.vg);
					nvgRoundedRect(args.vg, p.x - inf, p.y - inf,
						mw->box.size.x + 2 * inf, mw->box.size.y + 2 * inf, 6.f + inf * 0.4f);
					nvgStrokeColor(args.vg, nvgRGBAf(1.f, 0.855f, 0.624f,
						glowAlpha / (1.f + i * 1.2f)));
					nvgStrokeWidth(args.vg, 2.5f);
					nvgStroke(args.vg);
				}
			}
			// Module name across the top -- but only for plugins shipping
			// RackDroid's regenerated panels. Those panels carry their name
			// as an SVG <text> element, which nanosvg (Rack's SVG renderer)
			// silently ignores, so without this the module is anonymous.
			// Plugins with original upstream art (Bogaudio, Valley) bake
			// the name as vector paths and need no help.
			static const std::set<std::string> regenArtPlugins = {
				"Core", "Fundamental", "FrozenWasteland", "AudibleInstruments",
				"ImpromptuModular", "CountModula", "Bidoo", "GrandeModular", "Befaco",
				"RackDroidDrums",
			};
			if (mw->model && mw->model->plugin &&
					regenArtPlugins.count(mw->model->plugin->slug)) {
				math::Vec top = mw->getRelativeOffset(math::Vec(mw->box.size.x * 0.5f, 0), ref);
				nvgFontSize(args.vg, 7.5f);
				// Warm white: the label sits on the dark panel face just
				// below the cream header strip these panels draw.
				drawLabel(args, top.x, top.y + 12.0f, mw->model->name, nvgRGB(0xED, 0xE6, 0xD8));
				nvgFontSize(args.vg, 6.5f);
			}
			// Params: label below the control
			for (app::ParamWidget* pw : mw->getParams()) {
				engine::ParamQuantity* pq = pw->getParamQuantity();
				if (!pq || pq->name.empty())
					continue;
				math::Vec c = pw->getRelativeOffset(pw->box.size.mult(0.5f), ref);
				float ly = c.y + pw->box.size.y * 0.5f + 0.5f;
				if (!labelVisible(args, c.x, ly))
					continue;
				drawLabel(args, c.x, ly, shortLabel(pq->name), nvgRGB(0xED, 0xE6, 0xD8));
			}
			// Inputs/outputs: label below the jack, tinted by direction
			for (app::PortWidget* port : mw->getPorts()) {
				engine::PortInfo* pi = port->getPortInfo();
				if (!pi || pi->name.empty())
					continue;
				NVGcolor col = (port->type == engine::Port::INPUT)
					? nvgRGB(0xB8, 0xAF, 0x9C) : nvgRGB(0xFF, 0xDA, 0x9F);
				math::Vec c = port->getRelativeOffset(port->box.size.mult(0.5f), ref);
				float ly = c.y + port->box.size.y * 0.5f + 0.5f;
				if (!labelVisible(args, c.x, ly))
					continue;
				drawLabel(args, c.x, ly, shortLabel(pi->name), col);
			}
		}
	}

	/** Cheap cull: is a label at this point worth building at all? Separate
	from drawLabel because shortLabel() allocates -- twice, for the substr and
	the copy -- and doing that for a label that is then thrown away costs a
	few thousand allocations a second on a full rack. */
	static bool labelVisible(const DrawArgs& args, float x, float y) {
		return !(x < args.clipBox.pos.x - 40 || x > args.clipBox.pos.x + args.clipBox.size.x + 40 ||
			y < args.clipBox.pos.y - 20 || y > args.clipBox.pos.y + args.clipBox.size.y + 20);
	}

	void drawLabel(const DrawArgs& args, float x, float y, const std::string& text, NVGcolor col) {
		if (!labelVisible(args, x, y))
			return;
		// Subtle shadow for legibility over busy panels
		nvgFillColor(args.vg, nvgRGBA(0, 0, 0, 180));
		nvgText(args.vg, x + 0.3f, y + 0.3f, text.c_str(), NULL);
		nvgFillColor(args.vg, col);
		nvgText(args.vg, x, y, text.c_str(), NULL);
	}
};


void installLabelOverlay() {
	if (!APP->scene || !APP->scene->rack)
		return;
	PanelLabelOverlay* overlay = new PanelLabelOverlay;
	overlay->box.pos = math::Vec(0, 0);
	overlay->box.size = APP->scene->rack->box.size;
	APP->scene->rack->addChild(overlay);
}


} // namespace rackdroid
