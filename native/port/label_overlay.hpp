#pragma once

namespace rackdroid {

/** Adds the runtime parameter/port label overlay to the rack. Call once after
the patch is launched (the overlay walks the live module list each frame, so
it covers modules added later too). */
void installLabelOverlay();

/** Mark a module as freshly added: the overlay draws a short cream
"pop" glow around it (called by the native browser on tap-to-place). */
void noteModuleAdded(long long moduleId);

/** The rack theme in place has light panels (asset_extract.cpp says so at
startup): the labels over the panels that follow the theme are drawn in dark
ink. */
extern bool labelOverlayLightPanels;

} // namespace rackdroid
