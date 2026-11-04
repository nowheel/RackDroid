#pragma once

#include <cstdint>
#include <jansson.h>

namespace rackdroid {

/** Module bookmarks ("segnaposti").
 *
 * A module marked from its context menu gets the lowest free number; a dot on
 * the right edge (Java) lists them, and a tap on one scrolls the rack to that
 * module at the zoom the user is on. Numbers are not packed when one goes:
 * people remember "3 is the mixer".
 *
 * They are kept in the patch file under one extra top-level key. Rack reads
 * only the keys it knows, so VCV Rack opens the patch as before and drops the
 * bookmarks only if it saves it again.
 *
 * Render thread only, except bookmarkJumpRequest(). */

/** The module's number, or 0 if it is not marked. */
int bookmarkNumber(int64_t moduleId);

/** Marks the module with the lowest free number, or unmarks it. */
void bookmarkToggle(int64_t moduleId);

/** Hooks in the patched copy of Rack's patch.cpp (native/CMakeLists.txt). */
void bookmarksToJson(json_t* rootJ);
void bookmarksFromJson(json_t* rootJ);

/** Once a frame: the jump in progress, and the list handed to Java when it
changes (a module deleted, Undo bringing it back, a patch loaded). */
void processBookmarks();

} // namespace rackdroid
