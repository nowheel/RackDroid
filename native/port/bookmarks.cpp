/* Module bookmarks -- see bookmarks.hpp for what they are for. */

#include <app/ModuleWidget.hpp>
#include <app/RackScrollWidget.hpp>
#include <app/RackWidget.hpp>
#include <app/Scene.hpp>
#include <context.hpp>
#include <plugin/Model.hpp>
#include <plugin/Plugin.hpp>
#include <system.hpp>
#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "bookmarks.hpp"
#include "cable_park.hpp"
#if defined(__ANDROID__)
#include <jni.h>
#include "jni_bridge.hpp"
#endif

using namespace rack;

namespace rackdroid {

static const char* JSON_KEY = "rackdroidBookmarks";
static const double JUMP_SECONDS = 0.3;

// Module id -> its number. A deleted module keeps its entry: Undo brings it
// back with the same id, and with its number. It is left out of the list and
// of the saved file, and forgotten when the next patch loads.
static std::map<int64_t, int> g_marks;
// The list Java was last given; "" = no dot.
static std::string g_published;
static double g_lastCheck = 0.0;
static std::atomic<int64_t> g_jumpPending{-1};

static struct {
	bool on = false;
	math::Vec from, to;
	double start = 0.0;
} g_jump;


int bookmarkNumber(int64_t moduleId) {
	auto it = g_marks.find(moduleId);
	return it == g_marks.end() ? 0 : it->second;
}


void bookmarkToggle(int64_t moduleId) {
	g_lastCheck = 0.0;
	if (g_marks.erase(moduleId))
		return;
	std::set<int> taken;
	for (auto& m : g_marks)
		taken.insert(m.second);
	int n = 1;
	while (taken.count(n))
		n++;
	g_marks[moduleId] = n;
}


void bookmarksToJson(json_t* rootJ) {
	if (!APP->scene || !APP->scene->rack)
		return;
	json_t* marksJ = json_array();
	for (auto& m : g_marks) {
		if (APP->scene->rack->getModule(m.first))
			json_array_append_new(marksJ, json_pack("{s:I, s:i}", "id", (json_int_t) m.first, "n", m.second));
	}
	if (json_array_size(marksJ) > 0)
		json_object_set_new(rootJ, JSON_KEY, marksJ);
	else
		json_decref(marksJ);
}


void bookmarksFromJson(json_t* rootJ) {
	g_marks.clear();
	g_jump.on = false;
	g_lastCheck = 0.0;
	size_t i;
	json_t* markJ;
	json_array_foreach(json_object_get(rootJ, JSON_KEY), i, markJ) {
		json_t* idJ = json_object_get(markJ, "id");
		json_t* nJ = json_object_get(markJ, "n");
		if (json_is_integer(idJ) && json_is_integer(nJ) && json_integer_value(nJ) > 0)
			g_marks[json_integer_value(idJ)] = (int) json_integer_value(nJ);
	}
}


/** Centre the module at the zoom the user is on, in the part of the screen
the toolbar card does not cover. */
static void startJump(int64_t moduleId) {
	app::RackScrollWidget* rs = APP->scene->rackScroll;
	app::ModuleWidget* mw = APP->scene->rack->getModule(moduleId);
	if (!rs || !mw)
		return;
	math::Vec size = rs->box.size;
	float top = std::min(cableParkTopInset(), size.y * 0.5f);
	math::Vec centre(size.x * 0.5f, (top + size.y) * 0.5f);
	g_jump.on = true;
	g_jump.from = rs->offset;
	g_jump.to = mw->box.getCenter().mult(rs->getZoom()).minus(centre);
	g_jump.start = system::getTime();
}


static void stepJump() {
	if (!g_jump.on)
		return;
	float k = (float) ((system::getTime() - g_jump.start) / JUMP_SECONDS);
	if (k >= 1.f) {
		k = 1.f;
		g_jump.on = false;
	}
	float ease = k * k * (3.f - 2.f * k);
	APP->scene->rackScroll->offset = g_jump.from.plus(g_jump.to.minus(g_jump.from).mult(ease));
}


/** One line a module, by number: id, number, model name, plugin brand. */
static std::string buildList() {
	std::vector<std::pair<int, int64_t>> live;
	for (auto& m : g_marks) {
		if (APP->scene->rack->getModule(m.first))
			live.push_back({m.second, m.first});
	}
	std::sort(live.begin(), live.end());
	std::string list;
	for (auto& l : live) {
		plugin::Model* model = APP->scene->rack->getModule(l.second)->model;
		list += std::to_string(l.second) + "\t" + std::to_string(l.first) + "\t"
			+ (model ? model->name : "") + "\t"
			+ (model && model->plugin ? model->plugin->brand : "") + "\n";
	}
	return list;
}


void processBookmarks() {
	if (!APP->scene || !APP->scene->rack || !APP->scene->rackScroll)
		return;
	int64_t want = g_jumpPending.exchange(-1);
	if (want >= 0)
		startJump(want);
	stepJump();

	// ponytail: a scan of the rack four times a second; a deleted module has
	// no event of its own to hook.
	double now = system::getTime();
	if (now - g_lastCheck < 0.25)
		return;
	g_lastCheck = now;
	std::string list = buildList();
	if (list == g_published)
		return;
	g_published = list;
#if defined(__ANDROID__)
	nativeBookmarksChanged(list);
#endif
}

} // namespace rackdroid


#if defined(__ANDROID__)
extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeBookmarkJump(JNIEnv*, jobject, jlong moduleId) {
	rackdroid::g_jumpPending.store((int64_t) moduleId);
}
#endif
