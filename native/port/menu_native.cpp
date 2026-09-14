/* Native Android menus.
 *
 * Rack draws its menus in-canvas (nanovg), built for a mouse. This bridge
 * replaces them with real Android bottom-sheet menus:
 *
 *  1. When a MenuOverlay appears, capture its top Menu, serialize the rows,
 *     and ask Java to show a bottom sheet (jni_bridge -> MainActivity).
 *  2. The canvas overlay is hidden ONLY after Java confirms the sheet is up
 *     (nativeMenuShown). If Java fails, we fall back to the canvas menu and
 *     disable the native path — so a UI-thread error can never crash or
 *     blank the menu.
 *  3. A tap calls nativeMenuSelect(i); we record it and process on the render
 *     thread next frame (Rack widgets aren't thread-safe). Submenu -> drill
 *     down; leaf -> run its action and close.
 *
 * Safe because Rack dispatches a MenuItem's action via doAction() on the
 * captured item object, independent of cursor position.
 */
#include <atomic>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <context.hpp>
#include <common.hpp>
#include <string.hpp>
#include <history.hpp>
#include <asset.hpp>
#include <system.hpp>
#include <patch.hpp>
#include <app/Scene.hpp>
#include <ui/Menu.hpp>
#include <ui/MenuItem.hpp>
#include <ui/MenuLabel.hpp>
#include <ui/MenuSeparator.hpp>
#include <ui/MenuOverlay.hpp>
#include <widget/Widget.hpp>

#include <jni.h>
#include <android/log.h>

#include <settings.hpp>

#include "menu_native.hpp"
#include "jni_bridge.hpp"

// Mirror to Rack's logger too: it lands in <userDir>/log.txt, readable from
// the in-app log viewer (no adb needed on device).
#define LOGI(...) do { __android_log_print(ANDROID_LOG_INFO, "rackdroid.menu", __VA_ARGS__); INFO(__VA_ARGS__); } while (0)
#define LOGE(...) do { __android_log_print(ANDROID_LOG_ERROR, "rackdroid.menu", __VA_ARGS__); WARN(__VA_ARGS__); } while (0)

using namespace rack;


namespace rackdroid {


enum RowFlag {
	ROW_DISABLED = 1,
	ROW_SUBMENU = 2,
	ROW_LABEL = 4,
	ROW_SEPARATOR = 8,
	ROW_BACK = 16,
	// Synthetic "Share patch" row appended to the File menu (label is
	// localized on the Java side; the action runs here, see handleSelect).
	ROW_SHARE = 32,
	// Synthetic Help rows: in-app guide and step-by-step wizard (pure
	// Java UI; selecting them just calls back into MainActivity).
	ROW_GUIDE = 64,
	ROW_WIZARD = 128,
	ROW_WIZARD_PRO = 256,
	// Synthetic global-setting sliders appended to the View menu. Rack keeps
	// these as ui::Slider children (skipped by the enumeration below); we
	// re-expose them as SeekBar rows that write settings::cable* directly
	// (nativeSetCable*), which is simpler and pointer-safe (globals, always
	// valid) than bridging the live Quantity across threads.
	ROW_SLIDER_TENSION = 512,
	ROW_SLIDER_OPACITY = 1024,
	// A module's Preset ▸ Copy/Paste. Upstream labels them exactly like the
	// toolbar's copy/paste of the selection, but they carry that one module's
	// settings, not the modules themselves -- Java relabels them to say so.
	ROW_PRESET_COPY = 2048,
	ROW_PRESET_PASTE = 4096,
	// Synthetic Help row: run the first-run interface tour again.
	ROW_TOUR = 8192,
	// Synthetic Engine row: how much delay the user is willing to trade for
	// safety. This is the one audio decision the engine cannot make for them
	// -- everything else here is measured, but whether an occasional click
	// matters more than the delay on every note is a preference, not a fact.
	// It takes the place the Threads row used to occupy, which was the
	// opposite case: a question with one right answer that the user had no
	// way to know.
	ROW_LATENCY = 16384,
};


struct Row {
	ui::MenuItem* item;
	int flags;
	Row(ui::MenuItem* item, int flags) : item(item), flags(flags) {}
};


struct NativeMenu {
	ui::MenuOverlay* overlay = NULL;
	ui::Menu* menu = NULL;
	std::vector<Row> rows;
	bool active = false;
	bool shown = false;      // canvas overlay hidden / sheet confirmed
	bool disabled = false;   // native path failed once; use canvas from now on
	std::atomic<int> pendingSelect{-1};
	std::atomic<bool> pendingDismiss{false};
	std::atomic<bool> pendingShown{false};
	std::atomic<bool> backPending{false};
	std::atomic<int> toolbarTap{-1};
	// -1 none, 0 undo, 1 redo (toolbar ↶/↷ buttons)
	std::atomic<int> historyPending{-1};
	// Toolbar copy/paste/delete of the selected modules (run on the render
	// thread). selectionCount is republished every frame so Java can size its
	// confirmation dialog without reaching into the rack from the UI thread.
	std::atomic<bool> copyPending{false};
	std::atomic<bool> pastePending{false};
	std::atomic<bool> deletePending{false};
	std::atomic<int> selectionCount{0};
	// True while the menus on screen were opened by a long press on a module,
	// so its Preset ▸ Copy/Paste can be told apart from the identically labelled
	// rows elsewhere. Set by the touch layer, cleared by anything else that
	// opens a menu.
	bool moduleMenuActive = false;
	// The next captured top-level menu is the File menu (toolbar tap 0):
	// present() appends the synthetic Share row to it.
	bool fileMenuPending = false;
	bool helpMenuPending = false;
	bool engineMenuPending = false;
	bool viewMenuPending = false;
	bool sharePending = false;
};

static NativeMenu g;


/** Capturable if it has at least one actionable MenuItem. Non-list children
 * (e.g. a param value slider) are skipped when serialized. */
static bool capturable(ui::Menu* menu) {
	for (widget::Widget* c : menu->children) {
		if (dynamic_cast<ui::MenuItem*>(c))
			return true;
	}
	return false;
}


/** Upstream MenuBar rows that are dead weight on Android and hidden from the
 * bottom sheet: Quit (apps exit via system navigation), Fullscreen
 * (Window::setFullScreen is a no-op -- always fullscreen), the mouse-only
 * View toggles (no cursor, no wheel on touch), and every Help row whose
 * action is system::openBrowser/openDirectory (xdg-open: does not exist on
 * Android) or the VCV update check (network is stubbed out). Matched against
 * the same string::translate calls that created them, so filtering holds in
 * every display language. */
static bool hiddenOnAndroid(const std::string& text) {
	static const std::set<std::string> hidden = {
		string::translate("MenuBar.file.quit"),
		string::translate("MenuBar.view.fullscreen"),
		string::translate("MenuBar.view.mouseWheelZoom"),
		string::translate("MenuBar.view.lockCursor"),
		string::translate("MenuBar.view.knobScroll"),
		string::translate("MenuBar.help.manual"),
		string::translate("MenuBar.help.tips"),
		string::translate("MenuBar.help.support"),
		"VCVRack.com",
		string::translate("MenuBar.help.userFolder"),
		string::translate("MenuBar.help.changelog"),
		// The engine picks the thread count itself (checkThreadCount in
		// main_android.cpp) from what it measures on the device's actual audio
		// path. It is not a preference a user can hold a useful opinion about
		// -- the right answer was 8 on one phone and 2 on another, and moved
		// again when one of them got hot -- and leaving the row in meant two
		// owners writing one number, which is where every thread bug in issue
		// #3 lived.
		string::translate("MenuBar.engine.threads"),
		string::f(string::translate("MenuBar.help.update"), APP_NAME),
		string::f(string::translate("MenuBar.help.checkUpdate"), APP_NAME),
	};
	return hidden.count(text) > 0;
}


/** The module Preset submenu's Copy/Paste, which act on that module's settings
 * -- not on the selection the toolbar's copy/paste duplicates. Upstream labels
 * both pairs simply "Copy"/"Paste", so on a touch device the two read as the
 * same command; these rows are re-labelled in Java, where the strings are
 * localized. The identical labels on the rack menu and on text fields must not
 * be touched, hence the flag: only a menu opened from a module counts. */
static int presetClipboardFlag(const std::string& text) {
	if (text == string::translate("ModuleWidget.copy"))
		return ROW_PRESET_COPY;
	if (text == string::translate("ModuleWidget.paste"))
		return ROW_PRESET_PASTE;
	return 0;
}


static void present(ui::Menu* menu) {
	// A module's menu AND everything opened from it: upstream keeps Copy/Paste
	// in the Preset submenu. That submenu reaches us WITHOUT a parentMenu (the
	// bottom sheet re-presents it as a fresh top-level list), so the structure
	// cannot tell us where it came from -- the flag tracks the interaction that
	// opened the menu chain instead, and is cleared by the toolbar and by a
	// long press that did not land on a module.
	bool moduleMenu = g.moduleMenuActive;
	g.menu = menu;
	g.rows.clear();
	std::vector<std::string> labels, rights;
	std::vector<int> flags;

	if (menu->parentMenu) {
		g.rows.push_back(Row(NULL, ROW_BACK));
		labels.push_back("Back");
		rights.push_back("");
		flags.push_back(ROW_BACK);
	}

	for (widget::Widget* c : menu->children) {
		if (dynamic_cast<ui::MenuSeparator*>(c)) {
			g.rows.push_back(Row(NULL, ROW_SEPARATOR));
			labels.push_back("");
			rights.push_back("");
			flags.push_back(ROW_SEPARATOR);
		}
		else if (auto* lbl = dynamic_cast<ui::MenuLabel*>(c)) {
			g.rows.push_back(Row(NULL, ROW_LABEL));
			labels.push_back(lbl->text);
			rights.push_back("");
			flags.push_back(ROW_LABEL);
		}
		else if (auto* it = dynamic_cast<ui::MenuItem*>(c)) {
			if (hiddenOnAndroid(it->text))
				continue;
			int f = 0;
			if (moduleMenu)
				f |= presetClipboardFlag(it->text);
			if (it->disabled)
				f |= ROW_DISABLED;
			g.rows.push_back(Row(it, f));
			labels.push_back(it->text);
			rights.push_back(it->rightText);
			flags.push_back(f);
		}
		// Other widget types (sliders, text fields) are skipped.
	}

	if (g.fileMenuPending && !menu->parentMenu) {
		g.rows.push_back(Row(NULL, ROW_SEPARATOR));
		labels.push_back("");
		rights.push_back("");
		flags.push_back(ROW_SEPARATOR);
		g.rows.push_back(Row(NULL, ROW_SHARE));
		labels.push_back("Share patch…"); // replaced by a localized string in Java
		rights.push_back("");
		flags.push_back(ROW_SHARE);
	}
	if (g.engineMenuPending && !menu->parentMenu) {
		g.rows.push_back(Row(NULL, ROW_SEPARATOR));
		labels.push_back("");
		rights.push_back("");
		flags.push_back(ROW_SEPARATOR);
		g.rows.push_back(Row(NULL, ROW_LATENCY));
		labels.push_back("Response"); // localized in Java
		rights.push_back("");
		flags.push_back(ROW_LATENCY);
	}
	if (g.helpMenuPending && !menu->parentMenu) {
		g.rows.push_back(Row(NULL, ROW_SEPARATOR));
		labels.push_back("");
		rights.push_back("");
		flags.push_back(ROW_SEPARATOR);
		g.rows.push_back(Row(NULL, ROW_GUIDE));
		labels.push_back("Guide"); // localized in Java
		rights.push_back("");
		flags.push_back(ROW_GUIDE);
		g.rows.push_back(Row(NULL, ROW_WIZARD));
		labels.push_back("Tutorials"); // localized in Java (opens the library)
		rights.push_back("");
		flags.push_back(ROW_WIZARD);
		g.rows.push_back(Row(NULL, ROW_TOUR));
		labels.push_back("Interface tour"); // localized in Java
		rights.push_back("");
		flags.push_back(ROW_TOUR);
	}
	if (g.viewMenuPending && !menu->parentMenu) {
		// Cable feel, missing before because Rack keeps these as ui::Slider
		// widgets (skipped above). Rendered as SeekBar rows on the Java side.
		g.rows.push_back(Row(NULL, ROW_SEPARATOR));
		labels.push_back("");
		rights.push_back("");
		flags.push_back(ROW_SEPARATOR);
		g.rows.push_back(Row(NULL, ROW_SLIDER_TENSION));
		labels.push_back("Cable tension"); // localized in Java
		rights.push_back("");
		flags.push_back(ROW_SLIDER_TENSION);
		g.rows.push_back(Row(NULL, ROW_SLIDER_OPACITY));
		labels.push_back("Cable opacity"); // localized in Java
		rights.push_back("");
		flags.push_back(ROW_SLIDER_OPACITY);
	}

	LOGI("presenting %zu rows", labels.size());
	nativeMenuShow(labels, rights, flags);
}


static void closeAll() {
	if (g.overlay)
		g.overlay->requestDelete();
	g.overlay = NULL;
	g.menu = NULL;
	g.active = false;
	g.shown = false;
	g.rows.clear();
	nativeMenuDismiss();
}


static void handleSelect(int idx) {
	if (idx < 0 || idx >= (int) g.rows.size())
		return;
	Row& row = g.rows[idx];

	if (row.flags & ROW_BACK) {
		ui::Menu* parent = g.menu ? g.menu->parentMenu : NULL;
		if (parent) {
			parent->setChildMenu(NULL); // deletes current submenu
			present(parent);
		}
		return;
	}
	if (row.flags & ROW_SHARE) {
		g.sharePending = true;
		closeAll();
		return;
	}
	if (row.flags & ROW_LATENCY) {
		closeAll();
		nativeShowLatencyPicker();
		return;
	}
	if (row.flags & (ROW_GUIDE | ROW_WIZARD | ROW_WIZARD_PRO | ROW_TOUR)) {
		int which = (row.flags & ROW_GUIDE) ? 0
			: (row.flags & ROW_WIZARD) ? 1
			: (row.flags & ROW_TOUR) ? 3 : 2;
		closeAll();
		nativeShowHelp(which);
		return;
	}
	if ((row.flags & (ROW_LABEL | ROW_SEPARATOR | ROW_DISABLED)) || !row.item)
		return;

	ui::MenuItem* item = row.item;
	ui::Menu* child = item->createChildMenu();
	if (child) {
		g.menu->setChildMenu(child); // attach so the overlay owns it
		child->hide();
		present(child);
	}
	else {
		item->doAction(true); // runs action; requests overlay delete
		closeAll();
	}
}


/** True if `overlay` is still a live child of the scene. Rack can delete the
 * overlay on its own (background tap, item action, Escape) — after that all
 * our captured pointers are dangling and must not be touched. */
static bool overlayAlive(app::Scene* scene) {
	if (!g.overlay)
		return false;
	for (widget::Widget* c : scene->children) {
		if (c == g.overlay)
			return true;
	}
	return false;
}


/** Android back key: close whatever overlay is open on canvas (the module
 * browser, or -- if the bridge above is disabled/fell back -- a stray
 * ui::MenuOverlay) instead of letting the key fall through to Activity
 * default handling and exit the whole app. Best-effort, fire-and-forget:
 * MainActivity always consumes the key regardless of the outcome here. */
static void processBackKey(app::Scene* scene) {
	if (!g.backPending.exchange(false))
		return;
	if (!scene)
		return;
	if (scene->browser && scene->browser->visible) {
		scene->browser->hide();
		return;
	}
	for (widget::Widget* c : scene->children) {
		if (dynamic_cast<ui::MenuOverlay*>(c)) {
			c->requestDelete();
			break;
		}
	}
}


/** Android toolbar buttons (File/Edit/View/Engine/Library/Help) replace the
 * canvas MenuBar row outright -- scene->menuBar is hidden (see
 * processNativeMenus below), not just visually covered by the native row,
 * so it can never peek out from behind/above it (an earlier covered-not-
 * hidden version left exactly that sliver visible at the top edge, above
 * the native row, when the row was inset for the status bar/cutout).
 *
 * Hiding it means position-based dispatch can no longer reach its buttons:
 * Widget::recursePositionEvent (the routine behind both real clicks and
 * touch_input.cpp's injectTap) skips invisible widgets by design. So a tap
 * here calls the button's onAction() directly instead of synthesizing a
 * click at its screen position -- the exact same handler a real click
 * would run (ui::Button dispatches onButton -> onAction on release), just
 * invoked on the widget pointer we already have rather than routed there
 * by position/visibility. Index order matches MenuBar's own
 * `layout->addChild(fileButton); ...; addChild(helpButton);` sequence
 * (third_party/Rack/src/app/MenuBar.cpp), i.e. 0=File .. 5=Help. */
static void processToolbarTap(app::Scene* scene) {
	int index = g.toolbarTap.exchange(-1);
	if (index < 0)
		return;
	g.fileMenuPending = (index == 0);
	g.viewMenuPending = (index == 2);
	g.engineMenuPending = (index == 3);
	g.helpMenuPending = (index == 5);
	// A toolbar menu is not a module's, whatever the last long press opened.
	g.moduleMenuActive = false;
	if (!scene || !scene->menuBar || scene->menuBar->children.empty())
		return;
	widget::Widget* layout = scene->menuBar->children.front();
	if (index >= (int) layout->children.size())
		return;
	auto it = layout->children.begin();
	std::advance(it, index);
	widget::Widget* button = *it;
	button->onAction(widget::Widget::ActionEvent());
}


/** Toolbar ↶/↷: same routine as Rack's Ctrl+Z/Ctrl+Shift+Z handlers
 * (app/Scene.cpp onHoverKey), run on the render thread like every other
 * bridged action. canUndo/canRedo guards keep a no-op tap from logging a
 * complaint or touching state. */
static void processHistory() {
	int action = g.historyPending.exchange(-1);
	if (action < 0)
		return;
	if (!APP->history)
		return;
	if (action == 0 && APP->history->canUndo())
		APP->history->undo();
	else if (action == 1 && APP->history->canRedo())
		APP->history->redo();
}


/** Toolbar copy/paste of the selected modules -- Rack's own selection
 * clipboard, run on the render thread (it owns the rack widget). Copy is a
 * no-op with nothing selected; paste drops the clipboard near the last view. */
static void processClipboard() {
	if (!APP->scene || !APP->scene->rack)
		return;
	if (g.copyPending.exchange(false) && APP->scene->rack->hasSelection())
		APP->scene->rack->copyClipboardSelection();
	if (g.pastePending.exchange(false))
		APP->scene->rack->pasteClipboardAction();
	// One undoable action for the whole selection, like the desktop menu.
	if (g.deletePending.exchange(false) && APP->scene->rack->hasSelection())
		APP->scene->rack->deleteSelectionAction();
	g.selectionCount.store((int) APP->scene->rack->getSelected().size(),
		std::memory_order_relaxed);
}


/** Save the live patch to user/share/<name>.vcv and hand it to Java for
 * the system share sheet. Runs on the render thread (patch/engine owner);
 * PatchManager::save() archives the current state without touching the
 * patch's own path. */
static void processShare() {
	if (!g.sharePending)
		return;
	g.sharePending = false;
	try {
		std::string dir = rack::asset::user("share");
		rack::system::createDirectories(dir);
		std::string stem = APP->patch->path.empty() ? "patch"
			: rack::system::getStem(APP->patch->path);
		std::string path = dir + "/" + stem + ".vcv";
		APP->patch->save(path);
		nativeSharePatch(path);
	}
	catch (std::exception& e) {
		LOGE("share failed: %s", e.what());
	}
}


void menuExpectModuleMenu() {
	g.moduleMenuActive = true;
}


void menuClearModuleMenu() {
	g.moduleMenuActive = false;
}


void processNativeMenus() {
	app::Scene* scene = APP->scene;
	// The Android toolbar replaces it entirely (see processToolbarTap);
	// setVisible() no-ops once already hidden, so this is cheap to repeat.
	if (scene && scene->menuBar)
		scene->menuBar->hide();
	processBackKey(scene);
	processToolbarTap(scene);
	processHistory();
	processClipboard();
	processShare();
	if (!scene || g.disabled)
		return;

	try {
		// The overlay vanished under us (Rack deleted it): drop everything
		// without dereferencing, and close the sheet.
		if (g.active && !overlayAlive(scene)) {
			g.overlay = NULL;
			g.menu = NULL;
			g.active = false;
			g.shown = false;
			g.rows.clear();
			g.pendingSelect.store(-1);
			g.pendingShown.store(false);
			g.pendingDismiss.store(false);
			nativeMenuDismiss();
			return;
		}

		if (g.active) {
			// Java confirmed the sheet is up.
			if (g.pendingShown.exchange(false))
				g.shown = true;
			if (g.pendingDismiss.exchange(false)) {
				if (g.shown) {
					closeAll();
				}
				else {
					// Java failed to show before confirming: un-hide the
					// canvas menu (hidden at capture, see below) and fall
					// back to it permanently (never crash/blank).
					LOGE("native menu did not show; using canvas menus");
					if (g.overlay)
						g.overlay->show();
					g.disabled = true;
					g.active = false;
					g.overlay = NULL;
					g.menu = NULL;
					g.rows.clear();
				}
				return;
			}
			int idx = g.pendingSelect.exchange(-1);
			if (idx >= 0)
				handleSelect(idx);
		}

		if (!g.active) {
			for (widget::Widget* c : scene->children) {
				// Scene keeps a permanent hidden MenuOverlay (the module
				// browser) as a child from construction on. It is not a
				// plain ui::Menu; skip it explicitly (also handled below by
				// `top` staying NULL, but this documents the real reason
				// and avoids relying on that fallthrough).
				if (c == scene->browser)
					continue;
				ui::MenuOverlay* overlay = dynamic_cast<ui::MenuOverlay*>(c);
				if (!overlay)
					continue;
				ui::Menu* top = NULL;
				for (widget::Widget* mc : overlay->children) {
					top = dynamic_cast<ui::Menu*>(mc);
					if (top)
						break;
				}
				if (!top || !capturable(top))
					continue; // not this one; keep scanning (was a `return`
					          // bug that made the browser overlay above
					          // silently block every real menu, every frame)
				LOGI("capturing menu (%zu children)", top->children.size());
				g.overlay = overlay;
				g.active = true;
				g.shown = false;
				// Hide the canvas overlay IMMEDIATELY: leaving it visible
				// until Java confirmed flashed the light blendish menu for
				// 1-2 frames on every open. If Java fails, the dismiss
				// handler above un-hides it and falls back.
				overlay->hide();
				present(top);
				return;
			}
		}
	}
	catch (std::exception& e) {
		LOGE("exception in native menu: %s; disabling", e.what());
		g.disabled = true;
		g.active = false;
		g.overlay = NULL;
		g.menu = NULL;
		g.rows.clear();
	}
}


// ---- JNI callbacks from MainActivity (Java UI thread) ----

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeMenuSelect(JNIEnv*, jobject, jint index) {
	g.pendingSelect.store(index);
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeMenuDismiss(JNIEnv*, jobject) {
	g.pendingDismiss.store(true);
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeMenuShown(JNIEnv*, jobject) {
	g.pendingShown.store(true);
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeBackPressed(JNIEnv*, jobject) {
	g.backPending.store(true);
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeHistoryAction(JNIEnv*, jobject, jint action) {
	g.historyPending.store(action);
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeCopySelection(JNIEnv*, jobject) {
	g.copyPending.store(true);
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativePasteSelection(JNIEnv*, jobject) {
	g.pastePending.store(true);
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeDeleteSelection(JNIEnv*, jobject) {
	g.deletePending.store(true);
}

/** How many modules are selected, as of the last render pass. Java asks before
 * offering to delete them; a frame of staleness cannot matter, since the finger
 * that would change it is the same one now holding the toolbar. */
extern "C" JNIEXPORT jint JNICALL
Java_org_rackdroid_MainActivity_nativeSelectionCount(JNIEnv*, jobject) {
	return (jint) g.selectionCount.load(std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeToolbarTap(JNIEnv*, jobject, jint index) {
	g.toolbarTap.store(index);
}

// Cable feel. Plain global floats (settings.hpp), read by CableWidget every
// frame and persisted to settings.json on exit; an aligned float store is
// atomic on arm64, so writing them straight from the Java UI thread is safe
// and needs no render-thread round-trip. Both are clamped to [0, 1].
extern "C" JNIEXPORT jfloat JNICALL
Java_org_rackdroid_MainActivity_nativeGetCableTension(JNIEnv*, jobject) {
	return settings::cableTension;
}
extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeSetCableTension(JNIEnv*, jobject, jfloat v) {
	settings::cableTension = v < 0.f ? 0.f : (v > 1.f ? 1.f : (float) v);
}
extern "C" JNIEXPORT jfloat JNICALL
Java_org_rackdroid_MainActivity_nativeGetCableOpacity(JNIEnv*, jobject) {
	return settings::cableOpacity;
}
extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeSetCableOpacity(JNIEnv*, jobject, jfloat v) {
	settings::cableOpacity = v < 0.f ? 0.f : (v > 1.f ? 1.f : (float) v);
}


} // namespace rackdroid
