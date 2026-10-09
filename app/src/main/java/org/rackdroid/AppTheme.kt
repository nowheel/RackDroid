package org.rackdroid

import android.content.Context
import android.graphics.Color

/** Semantic color roles shared by every Kotlin-side touch surface (toolbar,
 * menus, module palette/browser, dialogs). NOT the rack/module panels
 * themselves -- those stay under Rack's own light/dark panel setting. */
data class Palette(
	val id: String,
	val nameRes: Int,
	/** Opaque warm/cool base used for full-screen and card backgrounds
	 * (glass cards derive their translucent variants from this via
	 * [AppTheme.withAlpha]). */
	val surface: Int,
	/** Solid inset surface sitting on top of [surface] (inactive chips,
	 * step-indicator dots, drag handles). */
	val surfaceInset: Int,
	val textPrimary: Int,
	val textSecondary: Int,
	val textDisabled: Int,
	/** The theme's accent hue (buttons, active tab/star, highlights). */
	val accent: Int,
	/** Text/icon color for content drawn on top of [accent] (e.g. a filled
	 * button's label). */
	val onAccent: Int,
	/** Destructive actions (uninstall, delete). */
	val danger: Int,
)

/** Selectable color themes for the touch UI. Persisted in SharedPreferences,
 * same pattern as the "locks"/"guide" prefs in MainActivity. Mirrors
 * AppFont's lazy-singleton shape so every Kotlin surface can reach it
 * without an import (same package). */
object AppTheme {
	private const val PREFS = "theme"
	private const val KEY_PRESET = "preset"

	val amber = Palette(
		id = "amber", nameRes = R.string.theme_amber,
		surface = Color.parseColor("#221F1A"),
		surfaceInset = Color.parseColor("#3A352D"),
		textPrimary = Color.parseColor("#EDE6D8"),
		textSecondary = Color.parseColor("#9A9284"),
		textDisabled = Color.parseColor("#6A645A"),
		accent = Color.parseColor("#FFDA9F"),
		onAccent = Color.parseColor("#17140F"),
		danger = Color.parseColor("#FF9E9E"),
	)

	val blueNight = Palette(
		// id must match the rack-theme asset dir name (graphics/themes/<id>/,
		// gen_themes.py, packSystemAssets) so applyRackTheme finds it.
		id = "blue", nameRes = R.string.theme_blue_night,
		surface = Color.parseColor("#161B22"),
		surfaceInset = Color.parseColor("#232B36"),
		textPrimary = Color.parseColor("#E4EAF2"),
		textSecondary = Color.parseColor("#8D97A6"),
		textDisabled = Color.parseColor("#565F6B"),
		accent = Color.parseColor("#7FC4FF"),
		onAccent = Color.parseColor("#0B1520"),
		danger = Color.parseColor("#FF8A80"),
	)

	val emerald = Palette(
		id = "emerald", nameRes = R.string.theme_emerald,
		surface = Color.parseColor("#141B16"),
		surfaceInset = Color.parseColor("#22302A"),
		textPrimary = Color.parseColor("#E3EDE4"),
		textSecondary = Color.parseColor("#8CA290"),
		textDisabled = Color.parseColor("#516155"),
		accent = Color.parseColor("#6FE0A0"),
		onAccent = Color.parseColor("#0B1B10"),
		danger = Color.parseColor("#FF9E8A"),
	)

	val violet = Palette(
		id = "violet", nameRes = R.string.theme_violet,
		surface = Color.parseColor("#1B1622"),
		surfaceInset = Color.parseColor("#2C2436"),
		textPrimary = Color.parseColor("#EAE3F2"),
		textSecondary = Color.parseColor("#9C8FA8"),
		textDisabled = Color.parseColor("#5E5468"),
		accent = Color.parseColor("#C9A6F5"),
		onAccent = Color.parseColor("#1A0E22"),
		danger = Color.parseColor("#FF9EB0"),
	)

	/** The light one. Its accent is darker than the others': it has to be
	 * read on cream, where theirs are read on near-black. */
	val cream = Palette(
		id = "cream", nameRes = R.string.theme_cream,
		surface = Color.parseColor("#F1EBDD"),
		surfaceInset = Color.parseColor("#DDD5C2"),
		textPrimary = Color.parseColor("#2B2721"),
		textSecondary = Color.parseColor("#6E6657"),
		textDisabled = Color.parseColor("#A39B8B"),
		accent = Color.parseColor("#B85C1C"),
		onAccent = Color.parseColor("#FFF8EC"),
		danger = Color.parseColor("#C0392B"),
	)

	val all = listOf(amber, blueNight, emerald, violet, cream)

	var current: Palette = amber
		private set

	private var initialized = false

	/** Restores the persisted choice. Call once, before building any themed
	 * view (MainActivity.onCreate, before addMidiButton()). */
	fun init(context: Context) {
		if (initialized) return
		initialized = true
		val id = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY_PRESET, amber.id)
		current = all.find { it.id == id } ?: amber
	}

	fun set(context: Context, palette: Palette) {
		current = palette
		context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
			.putString(KEY_PRESET, palette.id).apply()
		// Also hand the choice to the native side: the rack panels/rail/
		// background are themed by copying SVGs at startup (asset_extract.cpp
		// applyRackTheme reads this file from userDir = filesDir/user). Applies
		// on next launch, so the picker prompts for a restart.
		runCatching {
			val f = java.io.File(context.filesDir, "user/rack-theme.txt")
			f.parentFile?.mkdirs()
			f.writeText(palette.id)
		}
	}

	/** A hairline or faint tint that shows on [Palette.surface]: white on the
	 * dark themes, black on the light one. */
	fun edge(pct: Int): Int = withAlpha(if (current === cream) Color.BLACK else Color.WHITE, pct)

	/** [color] with its alpha replaced by [pct] percent (0-100). Replaces the
	 * many hand-picked "#XXbbggrr" alpha variants of the same base hue. */
	fun withAlpha(color: Int, pct: Int): Int {
		val a = (pct * 255 / 100).coerceIn(0, 255)
		return Color.argb(a, Color.red(color), Color.green(color), Color.blue(color))
	}
}
