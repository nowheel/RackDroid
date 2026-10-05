#pragma once

namespace rackdroid {

/** Bridges Rack menus to native Android bottom sheets. Call every frame from
the render loop, before drawing. Representable menus are shown natively and
their canvas overlay hidden; menus with non-list widgets are left to the
canvas path. */
void processNativeMenus();

/** Tell the menu layer that the context menu about to open belongs to a module,
so its Copy/Paste rows can be dropped (the toolbar owns those now). Called from
the touch layer when a long press lands on a ModuleWidget; the flag is consumed
by the next captured top-level menu. */
void menuExpectModuleMenu();

/** The menus about to open did NOT come from a module (a toolbar menu, or a
long press on empty rack), so their Copy/Paste rows are somebody else's. */
void menuClearModuleMenu();

/** True while one of the Audio module's own block size rows is running its
action. The audio driver asks, to tell a size the user picked there -- their
choice, to be remembered -- from one a patch file carries. */
bool menuUserActionRunning();

} // namespace rackdroid
