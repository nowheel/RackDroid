package org.rackdroid

import android.Manifest
import android.app.AlertDialog
import android.app.Dialog
import android.app.NativeActivity
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiManager
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.os.PowerManager
import android.provider.OpenableColumns
import android.text.InputType
import android.view.Gravity
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.PopupWindow
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.core.content.FileProvider
import androidx.core.content.IntentCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import java.io.File
import java.lang.ref.WeakReference
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.atomic.AtomicInteger

/** Cross-window blur (the glass card look) needs isCrossWindowBlurEnabled,
 *  API 31+ only -- calling it on API 29/30 throws NoSuchMethodError. Shared
 *  by every glass surface (MainActivity, ModulePalette, HelpUi). */
fun crossWindowBlurEnabled(windowManager: android.view.WindowManager): Boolean =
	android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.S &&
		windowManager.isCrossWindowBlurEnabled

/**
 * Java-side services for the native app (native/port/):
 *  - runtime permissions
 *  - clipboard (jni_bridge.cpp)
 *  - blocking dialogs: message / text prompt / patch file picker (osdialog)
 *  - MIDI device plumbing: MidiManager devices are opened here and handed to
 *    the native AMidi driver (amidi_driver.cpp)
 *
 * Dialog methods are called from the native glue thread and block it with a
 * latch while the dialog runs on the UI thread.
 */
class MainActivity : NativeActivity() {

	private val uiHandler = Handler(Looper.getMainLooper())
	// Cable-park toolbar toggle. Held at class scope so styleCableParkButton can
	// reach it. Defaults on: the bar is shown at startup (native g_visible also
	// defaults true), so the button starts lit and in sync.
	private var cableParkOn = true
	private var cableParkButton: ImageButton? = null
	private var midiManager: MidiManager? = null
	private val midiDevices = HashMap<Int, MidiDevice>()
	private val nextMidiId = AtomicInteger(1)
	private var startupRecoveryActive = false
	private var recoveryDialogShown = false
	private var audioFocusRequest: AudioFocusRequest? = null

	/** Our half of the strings -- toolbar, palette, tour, our dialogs -- comes
	 * from Android resources, which follow the DEVICE locale. Rack's half comes
	 * from its own translation files, which follow settings::language. Picking
	 * a language in Rack's Help menu used to move only its half, leaving an
	 * English File menu under an Italian toolbar (or the reverse). Once the
	 * user has chosen, that choice is remembered here and forced onto the
	 * resources too, so the two halves agree.
	 *
	 * Only after a deliberate choice: with no preference stored, resources
	 * follow the device exactly as they always did, and the port layer
	 * separately defaults Rack to the same place. */
	override fun attachBaseContext(base: Context) {
		val lang = runCatching {
			base.getSharedPreferences("ui", Context.MODE_PRIVATE).getString("lang", null)
		}.getOrNull()
		if (lang.isNullOrEmpty()) {
			super.attachBaseContext(base)
			return
		}
		val cfg = android.content.res.Configuration(base.resources.configuration)
		val loc = java.util.Locale.forLanguageTag(lang)
		java.util.Locale.setDefault(loc)
		cfg.setLocale(loc)
		super.attachBaseContext(base.createConfigurationContext(cfg))
	}

	/** Called from the render thread when Rack's Help ▸ Language changed it.
	 * The engine has already saved the patch and the settings by now. Rack asks
	 * for a restart and cannot perform one; Android can, and has to -- the
	 * resource locale above is read when the activity is built. */
	fun languageChangedFromNative(code: String) {
		uiHandler.post {
			runCatching {
				getSharedPreferences("ui", Context.MODE_PRIVATE)
					.edit().putString("lang", code).commit()
				val i = packageManager.getLaunchIntentForPackage(packageName)
				if (i != null) {
					i.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TASK or Intent.FLAG_ACTIVITY_NEW_TASK)
					startActivity(i)
				}
			}
			// Not finish(): the engine, the audio stream and the loaded plugin
			// libraries all live in this process, and the new one has to build
			// them from scratch. Ending the process is the restart.
			Runtime.getRuntime().exit(0)
		}
	}

	override fun onCreate(savedInstanceState: Bundle?) {
		prepareStartupRecovery()
		super.onCreate(savedInstanceState)
		activeActivity = WeakReference(this)

		AppTheme.init(this) // before any themed view gets built below
		ThumbnailCache.configure(this)
		// Draw under the camera. Hiding the system bars already gave us the
		// whole screen in portrait, but a cutout is a separate permission: with
		// the default mode a landscape window stops at the camera's edge, and
		// what is left over is a black strip down one side that no amount of
		// padding work inside the window can fill. SHORT_EDGES covers it in
		// both orientations, because the edge the camera sits on is the same
		// physical edge whichever way the phone is held. Only this window: the
		// toolbar and the palette are PopupWindows of their own, they keep the
		// default mode, and the window manager already lays them out clear of
		// the cutout -- their frames start at its inner edge. Padding them by
		// hand as well counted it twice and pushed the card visibly off centre.
		window.attributes = window.attributes.apply {
			layoutInDisplayCutoutMode =
				android.view.WindowManager.LayoutParams
					.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
		}
		hideSystemBars()
		jlog("RackDroid ${packageManager.getPackageInfo(packageName, 0).versionName} starting")

		val missing = arrayOf(Manifest.permission.RECORD_AUDIO, Manifest.permission.POST_NOTIFICATIONS)
			.filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
		if (missing.isNotEmpty()) {
			// No callback handling needed: audio_oboe reopens streams lazily
			// (output-only until mic granted); the foreground service runs
			// with or without a visible notification.
			requestPermissions(missing.toTypedArray(), 1)
		}

		// Shields the process (engine + audio) from the low-memory killer
		// while backgrounded.
		RackService.start(this)

		// The audio side starts on its balanced default; tell it what this
		// install actually chose before anything is played through it.
		applyLatencyMode()

		addMidiButton()
		initMidi()
		handleImportIntent(intent)

		// Side-loaded module packs are NOT loaded from here any more: the native
		// startup path calls loadUserPluginsFromNative() below and waits, so the
		// packs are registered BEFORE the autosaved patch is restored. Doing it
		// on a timer here raced the patch restore, and losing that race was not
		// merely cosmetic -- see loadUserPluginsFromNative.
		// The model list and the palette are NOT put up on a timer either: the
		// native side calls patchReadyFromNative() once the patch is up, which
		// is the event these actually depend on.

		// First launch: a spotlight tour of the whole interface (toolbar,
		// palette, cable-park bar, gestures). Shown once; finishing/skipping it
		// sets "tour_done". The patch-building Wizard stays in Help ▸ Tutorials.
		val tourPending = !startupRecoveryActive &&
			!getSharedPreferences("guide", Context.MODE_PRIVATE).getBoolean("tour_done", false)
		if (tourPending)
			uiHandler.postDelayed({
				runCatching { Tour(this).show() }
			}, 2500)
		// Update check (GitHub build only, and only once the user has opted in).
		// Never on the very first launch: the tour owns that screen, and asking
		// about network access before the app has been seen at all is a poor
		// first impression. A recovery launch stays silent too.
		if (!tourPending && !startupRecoveryActive)
			uiHandler.postDelayed({
				runCatching { AppUpdates.onStart(this) }
			}, 1200)
	}

	/** Mark every launch as incomplete before NativeActivity starts. If two
	 * consecutive launches fail before patchReadyFromNative(), the third skips
	 * both the autosave and side-loaded native plugins. No user file is changed. */
	private fun prepareStartupRecovery() {
		val prefs = getSharedPreferences("startup_recovery", Context.MODE_PRIVATE)
		val now = System.currentTimeMillis()
		val previousIncomplete = prefs.getBoolean("in_progress", false)
		val recent = now - prefs.getLong("started_at", 0L) <= 5 * 60 * 1000L
		val failures = if (previousIncomplete && recent)
			(prefs.getInt("failures", 0) + 1).coerceAtMost(3)
		else 0
		startupRecoveryActive = failures >= 2
		prefs.edit()
			.putBoolean("in_progress", true)
			.putLong("started_at", now)
			.putInt("failures", failures)
			.commit()
		nativeSetStartupOptions(startupRecoveryActive, startupRecoveryActive)
		// Only Help > Language ever writes this, so an empty value tells the
		// engine that nobody has chosen and it may follow the device. Pushed
		// here because startRack() reads it, and that runs on the render
		// thread once the first window arrives -- well after this.
		runCatching {
			nativeSetChosenLanguage(
				getSharedPreferences("ui", Context.MODE_PRIVATE).getString("lang", "") ?: "")
		}
	}

	private fun markStartupReady() {
		getSharedPreferences("startup_recovery", Context.MODE_PRIVATE).edit()
			.putBoolean("in_progress", false)
			.putInt("failures", 0)
			.apply()
	}

	private fun showStartupRecoveryDialog() {
		if (!startupRecoveryActive || recoveryDialogShown || isFinishing) return
		recoveryDialogShown = true
		AlertDialog.Builder(this)
			.setTitle(R.string.recovery_title)
			.setMessage(R.string.recovery_message)
			.setNegativeButton(R.string.recovery_keep_open, null)
			.setNeutralButton(R.string.recovery_manage_modules) { _, _ -> showModuleManager() }
			.setPositiveButton(R.string.recovery_start_fresh) { _, _ -> archiveAutosaveAndRestart() }
			.show()
	}

	/** Move, never delete, the suspect autosave. A normal restart then creates
	 * a fresh session while the complete previous autosave remains recoverable. */
	private fun archiveAutosaveAndRestart() {
		val source = File(filesDir, "user/autosave")
		var archived = !source.exists()
		if (source.exists()) {
			val recovery = File(filesDir, "user/recovery").apply { mkdirs() }
			val stamp = java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US)
				.format(java.util.Date())
			val dest = File(recovery, "autosave-$stamp")
			archived = source.renameTo(dest)
		}
		if (!archived) {
			Toast.makeText(this, R.string.recovery_archive_failed, Toast.LENGTH_LONG).show()
			return
		}
		getSharedPreferences("startup_recovery", Context.MODE_PRIVATE).edit().clear().commit()
		packageManager.getLaunchIntentForPackage(packageName)?.let {
			it.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TASK or Intent.FLAG_ACTIVITY_NEW_TASK)
			startActivity(it)
		}
		finishAffinity()
		Runtime.getRuntime().exit(0)
	}

	/** Screen bounds of the toolbar card, so the interface tour can spotlight
	 * it. Null until the toolbar popup is laid out. */
	fun toolbarBounds(): android.graphics.Rect? {
		val v = buttonPopup?.contentView ?: return null
		if (!v.isAttachedToWindow || v.width == 0) return null
		val loc = IntArray(2)
		v.getLocationOnScreen(loc)
		return android.graphics.Rect(loc[0], loc[1], loc[0] + v.width, loc[1] + v.height)
	}

	/** Live demonstrations for the tour. Everything here is put back: the demo
	 * deselects exactly what it selected and restores module positions to the
	 * coordinates they had, so replaying the tour over real work changes
	 * nothing. See native/port/tour_demo.cpp. */
	fun tourDemo(what: Int) = runCatching { nativeTourDemo(what) }

	/** The part of the screen the demonstrations should play in, in screen
	 * pixels -- the tour's lit rectangle, which in landscape is half the width.
	 * Without it the engine frames the modules in the middle of the window and
	 * they end up jammed against the edge of the spotlight, half of them behind
	 * the card explaining them. An empty rect means the whole window. */
	fun tourStage(r: android.graphics.Rect?) = runCatching {
		if (r == null) nativeTourStage(0, 0, 0, 0)
		else nativeTourStage(r.left, r.top, r.width(), r.height())
	}

	/** True while the tour is opening menus to show what they hold. The sheets
	 * it opens are made untouchable (see showNativeMenu): they are there to be
	 * looked at, and a stray tap on File ▸ New during the demonstration would
	 * throw away the patch the tour is running on top of. Taps fall through to
	 * the tour's own scrim instead, which is what the user expects to hit. */
	var tourMenuDemo = false

	/** Open one of the toolbar menus for the tour, or close what is open. */
	fun tourOpenMenu(index: Int) = runCatching { nativeToolbarTap(index) }

	fun tourCloseSheet() = uiHandler.post { runCatching { menuDialog?.dismiss() } }

	/** Whether the module palette is on screen right now. The tour asks before
	 * opening it, so a palette the user already had open is left exactly as it
	 * was -- category chip included -- instead of being reset and then shut. */
	fun tourPaletteIsOpen(): Boolean = modulePalette.bounds() != null

	fun tourShowPalette(show: Boolean) = uiHandler.post {
		runCatching { if (show) modulePalette.showAll() else modulePalette.hide() }
	}

	/** Fold the palette's tile strip away for the tour and put it back, chip
	 * and all. Not hide(): the rack demonstrations only need the vertical
	 * space the tiles take, and a palette the user had open on a category must
	 * come back on that category. */
	fun tourFoldPalette(fold: Boolean) = uiHandler.post {
		runCatching {
			if (fold) modulePalette.foldForTour() else modulePalette.unfoldForTour()
		}
	}

	/** Multi-select as the toolbar shows it. Held here (rather than as a local
	 * of the toolbar builder) so the tour can switch it and have the button
	 * light up with it. */
	private var multiSelectOn = false
	private var applyMultiSelect: ((Boolean) -> Unit)? = null

	fun tourMultiSelectIsOn(): Boolean = multiSelectOn

	/** Modules on the rack right now, or -1 if the render thread has not run a
	 * frame yet. The tour asks before building its steps: on an empty rack
	 * there is nothing to slide, select or patch -- but "not known yet" must
	 * not be mistaken for "empty". */
	fun tourModuleCount(): Int = runCatching { nativeRackModuleCount() }.getOrDefault(-1)

	fun tourSetMultiSelect(on: Boolean) = uiHandler.post {
		runCatching { applyMultiSelect?.invoke(on) }
	}

	/** Screen bounds of the module palette, for the same tour. */
	fun paletteBounds(): android.graphics.Rect? = modulePalette.bounds()

	// The tour walks the toolbar part by part, so it needs the pieces and not
	// just the card. Held from addMidiButton(), null until it has run.
	private var menuRowView: View? = null
	private var toolGridView: View? = null

	private fun viewBounds(v: View?): android.graphics.Rect? {
		if (v == null || !v.isAttachedToWindow || v.width == 0 || v.height == 0) return null
		val loc = IntArray(2)
		v.getLocationOnScreen(loc)
		return android.graphics.Rect(loc[0], loc[1], loc[0] + v.width, loc[1] + v.height)
	}

	/** The File/Edit/View/Engine/Help strip. */
	fun toolbarMenuRowBounds(): android.graphics.Rect? = viewBounds(menuRowView)

	/** One button of that strip, 0..4, for the tour's per-menu steps. */
	fun toolbarMenuButtonBounds(index: Int): android.graphics.Rect? {
		val row = menuRowView as? ViewGroup ?: return null
		if (index < 0 || index >= row.childCount) return null
		return viewBounds(row.getChildAt(index))
	}

	/** One row of the eight-column tool grid: 0 = build/play, 1 = edit/protect.
	 * The grid lays both rows out at the same height, so halving is exact. */
	fun toolRowBounds(row: Int): android.graphics.Rect? {
		val r = viewBounds(toolGridView) ?: return null
		val half = r.height() / 2
		return if (row == 0) android.graphics.Rect(r.left, r.top, r.right, r.top + half)
		else android.graphics.Rect(r.left, r.top + half, r.right, r.bottom)
	}

	/** A span of columns within a tool row, for pointing at single buttons —
	 * the two padlocks, say, which are columns 6 and 7 of the second row. */
	fun toolCellsBounds(row: Int, firstColumn: Int, columnCount: Int): android.graphics.Rect? {
		val r = toolRowBounds(row) ?: return null
		val cell = r.width() / 8f
		return android.graphics.Rect(
			(r.left + cell * firstColumn).toInt(), r.top,
			(r.left + cell * (firstColumn + columnCount)).toInt(), r.bottom)
	}

	/** Screen bounds of the cable parking bar, which the render thread draws --
	 * there is no view to measure, so the geometry comes back through JNI in
	 * window pixels and is shifted into screen space by the canvas position. */
	fun cableParkBounds(): android.graphics.Rect? {
		val b = runCatching { nativeCableParkBounds() }.getOrNull() ?: return null
		if (b.size < 4) return null
		val loc = IntArray(2)
		window.decorView.getLocationOnScreen(loc)
		return android.graphics.Rect(
			loc[0] + b[0], loc[1] + b[1], loc[0] + b[2], loc[1] + b[3])
	}

	/** Native Android top toolbar, covering the tiny canvas MenuBar strip:
	 *  a full-width row of File/Edit/View/Engine/Library/Help buttons (equal
	 *  weight, so all six always fit with no scrolling) — each injects a
	 *  synthetic tap on the real canvas MenuButton via nativeToolbarTap, the
	 *  same ui::Menu a real tap would open, now correctly shown as a bottom
	 *  sheet since the capture bridge was fixed — plus a second row with the
	 *  🎹 Bluetooth-MIDI scanner and ⓘ credits/log buttons.
	 *
	 * MUST live in its own window (PopupWindow): NativeActivity takes the
	 * main window's surface and input queue (takeSurface/takeInputQueue),
	 * so views added to the main window are never drawn and never get
	 * touches — an addContentView overlay here is invisible. Dialogs and
	 * popups are separate windows, which is why they work. */
	private var buttonPopup: PopupWindow? = null

	// Set by addMidiButton; collapses/expands the top tools card. Called
	// when a tutorial opens so the card folds to just its glass arrow,
	// clearing the top for the tutorial step card.
	private var collapseToolbar: ((Boolean) -> Unit)? = null
	private var toolbarUserCollapsed = false
	fun setToolbarCollapsedForTutorial(collapsed: Boolean) {
		uiHandler.post {
			runCatching { collapseToolbar?.invoke(if (collapsed) true else toolbarUserCollapsed) }
		}
	}

	/** A toolbar dimension, cut by a third in landscape.
	 *
	 * The card carries a menu strip and two rows of tools whatever the shape of
	 * the screen, and in landscape that is close to half the height -- on a
	 * phone it left the rack a slot, and the interface tour had nothing to
	 * demonstrate in. The icons are drawable vectors and lose nothing by being
	 * smaller; the touch targets stay finger-sized because a landscape phone is
	 * wide, so the cells grow sideways as they shrink vertically. */
	private fun tbDp(v: Int): Int {
		val land = resources.configuration.orientation ==
			android.content.res.Configuration.ORIENTATION_LANDSCAPE
		return if (land) dp((v * 0.7f).toInt().coerceAtLeast(2)) else dp(v)
	}

	/** The toolbar is built once and the manifest keeps this activity alive
	 * across a rotation, so nothing would re-read tbDp() when the screen turns.
	 * Re-apply the sizes to the views that are already there rather than
	 * rebuilding the card: addMidiButton() wires up more than layout, and
	 * running it twice would register it all twice. */
	override fun onConfigurationChanged(newConfig: android.content.res.Configuration) {
		super.onConfigurationChanged(newConfig)
		uiHandler.post { runCatching { applyToolbarDensity() } }
		// The palette needs the same treatment, and for the same reason: its
		// sizes are all fixed when it opens.
		uiHandler.post { runCatching { modulePalette.relayoutForOrientation() } }
		// The insets are not always right on the first pass after a rotation --
		// the window is resized before the cutout follows it round -- and a
		// stale one leaves the park bar under the camera until something else
		// happens to ask. Cheap enough to simply ask again.
		uiHandler.postDelayed({ runCatching { publishCutoutToNative() } }, 400L)
	}

	private var toolbarMenuRow: LinearLayout? = null
	private var toolbarGrid: android.widget.GridLayout? = null
	private var toolbarHolder: android.widget.FrameLayout? = null
	private var toolbarCard: LinearLayout? = null

	/** Tell the render thread how far in from the left the camera reaches. The
	 * window draws under it now, so the rack fills the screen -- but the cable
	 * park bar sits on that same edge, and a punch-hole straight through the
	 * middle jack is worse than the black strip it replaced. The NDK exposes no
	 * cutout, so this is the only side that can answer. */
	private fun publishCutoutToNative() {
		val left = ViewCompat.getRootWindowInsets(window.decorView)
			?.getInsets(WindowInsetsCompat.Type.displayCutout())?.left ?: 0
		runCatching { nativeCableParkLeftInset(left) }
	}

	/** And how far down the toolbar card reaches, for the same reason: the park
	 * bar centres itself on the screen, and in landscape the card is tall enough
	 * that the bar's collapse handle ended up behind it. Driven off the card's
	 * own layout, so collapsing the toolbar hands the room straight back. */
	private fun publishToolbarBottomToNative() {
		val h = toolbarHolder ?: return
		if (h.height <= 0) return
		val loc = IntArray(2)
		h.getLocationOnScreen(loc)
		runCatching { nativeCableParkTopInset(loc[1] + h.height) }
	}

	private fun applyToolbarDensity() {
		publishCutoutToNative()
		val land = resources.configuration.orientation ==
			android.content.res.Configuration.ORIENTATION_LANDSCAPE
		// Flush with the top edge in landscape: the four points above the card
		// are worth having in portrait, where height is what there is most of,
		// and in landscape they come straight out of the rack. Only the top --
		// the side inset costs nothing there and keeps the card off the rounded
		// corners of the screen.
		toolbarHolder?.setPadding(dp(8), if (land) 0 else dp(4), dp(8), 0)
		// The menu labels sit in a box taller than they are, and that empty band
		// above FILE is what reads as a margin at the top of the screen even
		// once the card itself starts at the first row of pixels. In landscape
		// it goes, together with the card's own top inset and the labels' top
		// margin: the gap the eye sees is all three of them stacked.
		toolbarCard?.setPadding(dp(4), if (land) 0 else dp(2), dp(4), 0)
		// And move the window itself. It is placed once, when the app starts,
		// at the status-bar inset of whatever orientation that was -- so an app
		// launched in portrait and then turned kept a portrait inset above the
		// card, which is the gap that survived every padding change. Landscape
		// has nothing up there to clear.
		buttonPopup?.let { p ->
			val top = if (land) 0 else (ViewCompat.getRootWindowInsets(window.decorView)
				?.getInsets(WindowInsetsCompat.Type.systemBars()
					or WindowInsetsCompat.Type.displayCutout())?.top ?: 0)
			runCatching { p.update(0, top, -1, -1) }
		}
		toolbarMenuRow?.let { row ->
			for (i in 0 until row.childCount) {
				val v = row.getChildAt(i)
				v.setPadding(0, if (land) tbDp(6) else dp(12), 0, if (land) tbDp(8) else dp(12))
				(v.layoutParams as? LinearLayout.LayoutParams)?.let {
					it.setMargins(dp(2), if (land) 0 else dp(2), dp(2), dp(2))
					v.layoutParams = it
				}
			}
		}
		toolbarGrid?.let { grid ->
			for (i in 0 until grid.childCount) {
				val v = grid.getChildAt(i)
				v.setPadding(tbDp(7), tbDp(9), tbDp(7), tbDp(9))
				(v.layoutParams as? android.widget.GridLayout.LayoutParams)?.let {
					it.height = tbDp(42); v.layoutParams = it
				}
			}
			grid.requestLayout()
		}
	}

	private fun addMidiButton() {
		val toolbarPrefs = getSharedPreferences("toolbar", Context.MODE_PRIVATE)
		toolbarUserCollapsed = toolbarPrefs.getBoolean("collapsed", false)
		val menuRow = LinearLayout(this).apply {
			orientation = LinearLayout.HORIZONTAL
			// Translucent: the rack scrolls under a smoked-glass strip (the
			// popup window also gets blur-behind below, where supported).
			setBackgroundColor(AppTheme.withAlpha(AppTheme.current.surface, 76))
		}
		// label -> canvas MenuBar child index (File=0 .. Help=5). "Library"
		// (index 4) is deliberately absent: it only manages a VCV account /
		// online library, useless in this port -- but the index mapping must
		// stay explicit so Help still reaches child 5.
		val menuLabels = listOf(R.string.menu_file to 0, R.string.menu_edit to 1,
			R.string.menu_view to 2, R.string.menu_engine to 3, R.string.menu_help to 5)
		for ((labelRes, i) in menuLabels) {
			// Flat text buttons with the amber ripple, not the stock grey
			// Material pills: reads as a real app toolbar.
			menuRow.addView(TextView(this).apply {
				text = getString(labelRes).uppercase()
				// Single line + autosize: localized labels vary a lot in
				// length ("VISUALIZZA" wrapped at a fixed 12sp).
				isSingleLine = true
				androidx.core.widget.TextViewCompat.setAutoSizeTextTypeUniformWithConfiguration(
					this, 7, 12, 1, android.util.TypedValue.COMPLEX_UNIT_SP)
				setTypeface(AppFont.get(this@MainActivity), Typeface.BOLD)
				setTextColor(AppTheme.current.textPrimary)
				gravity = Gravity.CENTER
				background = amberRippleRounded()
				setPadding(0, tbDp(12), 0, tbDp(12))
				layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
					.apply { setMargins(dp(2), dp(2), dp(2), dp(2)) }
				setOnClickListener { nativeToolbarTap(i) }
			})
		}
		// Fifteen tools do not have a usable touch target when squeezed into one
		// phone-width row. Keep eight fixed columns and flow them over two rows;
		// an invisible final cell keeps the second row aligned with the first.
		toolbarMenuRow = menuRow
		val toolGrid = android.widget.GridLayout(this).apply {
			columnCount = 8
			rowCount = 2
			alignmentMode = android.widget.GridLayout.ALIGN_BOUNDS
			useDefaultMargins = false
			layoutParams = LinearLayout.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
		}
		toolbarGrid = toolGrid
		val iconTint = android.content.res.ColorStateList.valueOf(AppTheme.current.textPrimary)
		val amberTint = android.content.res.ColorStateList.valueOf(AppTheme.current.accent)
		// One consistent set of line icons (vector drawables, uniformly tinted),
		// each in an equal-weight slot so the row spreads evenly -- replaces the
		// old grab-bag of mismatched colour emoji. Toggles light up amber.
		fun iconButton(iconRes: Int, desc: String, onClick: (ImageButton) -> Unit) =
			ImageButton(this).apply {
				setImageResource(iconRes)
				imageTintList = iconTint
				scaleType = ImageView.ScaleType.FIT_CENTER
				background = amberRippleRounded()
				contentDescription = desc
				minimumWidth = 0; minimumHeight = 0
				setPadding(tbDp(7), tbDp(9), tbDp(7), tbDp(9))
				setOnClickListener { onClick(this) }
			}
		fun addTool(view: View, index: Int) {
			toolGrid.addView(view, android.widget.GridLayout.LayoutParams(
				android.widget.GridLayout.spec(index / 8),
				android.widget.GridLayout.spec(index % 8, 1f)).apply {
				width = 0
				height = tbDp(42)
				setMargins(dp(1), 0, dp(1), 0)
			})
		}
		val paletteButton = iconButton(R.drawable.ic_tb_modules, getString(R.string.menu_view)) { modulePalette.toggle() }
		val installButton = iconButton(R.drawable.ic_tb_install, getString(R.string.modules_manager_title)) { showModuleManager() }
		// Cable parking bar: you cannot pan the rack while dragging a cable, so
		// this is where a cable end waits while you scroll to its destination.
		cableParkButton = iconButton(R.drawable.ic_tb_cablepark,
				getString(R.string.cable_park_title)) {
			cableParkOn = !cableParkOn
			nativeSetCableParkVisible(cableParkOn)
			styleCableParkButton()
			if (cableParkOn)
				Toast.makeText(this, getString(R.string.cable_park_hint), Toast.LENGTH_LONG).show()
		}
		styleCableParkButton()
		val themeButton = iconButton(R.drawable.ic_tb_theme, getString(R.string.theme_picker_title)) { showThemePicker() }
		val undoButton = iconButton(R.drawable.ic_tb_undo, getString(R.string.toolbar_undo)) { nativeHistoryAction(0) }
		val redoButton = iconButton(R.drawable.ic_tb_redo, getString(R.string.toolbar_redo)) { nativeHistoryAction(1) }
		val midiButton = iconButton(R.drawable.ic_tb_midi, getString(R.string.toolbar_midi)) { showBleMidiScanner() }
		val keyboardButton = iconButton(R.drawable.ic_tb_keyboard, getString(R.string.toolbar_keyboard)) { toggleVirtualKeyboard() }
		val creditsButton = iconButton(R.drawable.ic_tb_info, getString(R.string.toolbar_info)) { showCredits() }
		val recordButton = iconButton(R.drawable.ic_tb_record, getString(R.string.toolbar_record)) { toggleRecording(it) }
		recordButton.imageTintList = android.content.res.ColorStateList.valueOf(AppTheme.current.danger)
		// Patch padlocks. Outline lock freezes the layout (module positions +
		// cables, knobs stay live); the solid lock freezes everything including
		// parameters. Active = amber + full opacity; inactive dimmed. Persisted.
		val lockPrefs = getSharedPreferences("locks", Context.MODE_PRIVATE)
		var layoutLock = lockPrefs.getBoolean("layout", false)
		var fullLock = lockPrefs.getBoolean("full", false)
		lateinit var lockButton: ImageButton
		lateinit var fullLockButton: ImageButton
		fun applyLocks() {
			nativeSetLockMode(if (fullLock) 2 else if (layoutLock) 1 else 0)
			lockPrefs.edit().putBoolean("layout", layoutLock).putBoolean("full", fullLock).apply()
			val l1 = layoutLock || fullLock
			lockButton.imageTintList = if (l1) amberTint else iconTint
			lockButton.alpha = if (l1) 1f else 0.6f
			fullLockButton.imageTintList = if (fullLock) amberTint else iconTint
			fullLockButton.alpha = if (fullLock) 1f else 0.6f
		}
		lockButton = iconButton(R.drawable.ic_tb_lock_outline, getString(R.string.toolbar_lock_layout)) { layoutLock = !layoutLock; applyLocks() }
		fullLockButton = iconButton(R.drawable.ic_tb_lock, getString(R.string.toolbar_lock_all)) { fullLock = !fullLock; applyLocks() }
		applyLocks() // restore persisted state (also styles both buttons)
		// Multi-select: while on, a one-finger drag on empty rack draws the
		// selection marquee; while off (default), that drag pans the view.
		// Session-only, dimmed when off.
		lateinit var selectButton: ImageButton
		// One place that turns the mode on or off, because the tour drives it
		// too: the button has to light up when the interface tour reaches the
		// multi-select step, or it demonstrates a mode nothing shows as on.
		applyMultiSelect = { on ->
			multiSelectOn = on
			nativeSetMultiSelect(on)
			selectButton.imageTintList = if (on) amberTint else iconTint
			selectButton.alpha = if (on) 1f else 0.6f
		}
		selectButton = iconButton(R.drawable.ic_tb_select, getString(R.string.select_modules_title)) {
			applyMultiSelect?.invoke(!multiSelectOn)
		}
		selectButton.alpha = 0.6f
		// Copy / paste the selected modules (Rack's own selection clipboard).
		val copyButton = iconButton(R.drawable.ic_tb_copy, getString(R.string.copy_modules_title)) {
			nativeCopySelection()
			Toast.makeText(this, getString(R.string.copy_modules_done), Toast.LENGTH_SHORT).show()
		}
		val pasteButton = iconButton(R.drawable.ic_tb_paste, getString(R.string.paste_modules_title)) { nativePasteSelection() }
		// Delete the selection. Always confirmed: the rack is the patch, and a
		// mis-tap here costs work that the user cannot see leaving the screen.
		val deleteButton = iconButton(R.drawable.ic_tb_delete, getString(R.string.delete_modules_title)) {
			confirmDeleteSelection()
		}
		deleteButton.imageTintList = android.content.res.ColorStateList.valueOf(AppTheme.current.danger)
		// Row 1: things used to build/play/view the rack. Row 2: edit and
		// protection commands. The grouping is stable even when more space is
		// available, so muscle memory does not depend on device width.
		listOf(paletteButton, installButton, cableParkButton!!, themeButton,
			midiButton, keyboardButton, recordButton, creditsButton,
			undoButton, redoButton, selectButton, copyButton, pasteButton,
			deleteButton, lockButton, fullLockButton)
			.forEachIndexed { index, view -> addTool(view, index) }
		menuRow.setBackgroundColor(Color.TRANSPARENT)
		val menuDivider = View(this).apply {
			setBackgroundColor(AppTheme.withAlpha(Color.WHITE, 10))
			layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1))
				.apply { setMargins(dp(10), dp(3), dp(10), dp(4)) }
		}

		// Single floating glass card at the TOP (user: tools back on top):
		// the five menus over the tool icons, one hide handle for both. No
		// blur-behind -- persistent window, would smear the whole rack (see
		// the 0.21.2 incident). LayoutTransition animates the collapse.
		// A reusable rounded glass pill for the handle button.
		fun glassPill() = GradientDrawable().apply {
			cornerRadius = dp(18).toFloat()
			setColor(AppTheme.withAlpha(AppTheme.current.surface, 85))
			setStroke(dp(1), AppTheme.withAlpha(Color.WHITE, 15))
		}
		val cardBg = GradientDrawable().apply {
			cornerRadius = dp(20).toFloat()
			setColor(AppTheme.withAlpha(AppTheme.current.surface, 85))
			setStroke(dp(1), AppTheme.withAlpha(Color.WHITE, 15))
		}
		lateinit var collapseButton: ImageButton
		lateinit var card: LinearLayout
		var collapsed = false
		// Apply a collapse state (shared by the handle tap and the
		// tutorial-opened auto-collapse). Idempotent.
		fun applyCollapsed(c: Boolean) {
			if (collapsed == c) return
			collapsed = c
			val vis = if (collapsed) View.GONE else View.VISIBLE
			menuRow.visibility = vis
			menuDivider.visibility = vis
			toolGrid.visibility = vis
			// Collapsed: only this arrow, as a standalone glass button (the
			// card's own glass background disappears). Expanded: the full
			// glass card, arrow flat inside it.
			card.background = if (collapsed) null else cardBg
			collapseButton.background = if (collapsed) glassPill() else amberRippleRounded()
			collapseButton.contentDescription = getString(
				if (collapsed) R.string.toolbar_expand else R.string.toolbar_collapse)
			collapseButton.animate().rotation(if (collapsed) 180f else 0f).setDuration(240L).start()
		}
		collapseToolbar = { c -> applyCollapsed(c) }
		menuRowView = menuRow
		toolGridView = toolGrid
		collapseButton = ImageButton(this).apply {
			setImageResource(R.drawable.ic_tb_chevron)
			imageTintList = android.content.res.ColorStateList.valueOf(AppTheme.current.textPrimary)
			scaleType = ImageView.ScaleType.FIT_CENTER
			contentDescription = getString(R.string.toolbar_collapse)
			minimumWidth = 0; minimumHeight = 0
			setPadding(dp(10), dp(4), dp(10), dp(4))
			setOnClickListener {
				toolbarUserCollapsed = !collapsed
				toolbarPrefs.edit().putBoolean("collapsed", toolbarUserCollapsed).apply()
				applyCollapsed(toolbarUserCollapsed)
			}
		}
		val handleRow = LinearLayout(this).apply {
			orientation = LinearLayout.HORIZONTAL
			gravity = Gravity.CENTER_HORIZONTAL
			addView(collapseButton, LinearLayout.LayoutParams(dp(64), dp(26)))
		}
		toolbarCard = null
		card = LinearLayout(this).apply {
			orientation = LinearLayout.VERTICAL
			background = cardBg
			clipToOutline = true
			setPadding(dp(4), dp(2), dp(4), 0)
			layoutTransition = android.animation.LayoutTransition()
			addView(menuRow, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
			addView(menuDivider)
			addView(toolGrid)
			addView(handleRow)
		}
		toolbarCard = card
		collapseButton.background = amberRippleRounded()
		applyCollapsed(toolbarUserCollapsed)
		val holder = android.widget.FrameLayout(this).apply {
			addView(card)
		}
		toolbarHolder = holder
		// Every time the card is measured -- first layout, collapse, expand,
		// rotation -- tell the render thread where its bottom edge ended up.
		holder.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
			publishToolbarBottomToNative()
		}
		// The padding lives in applyToolbarDensity, which is also what a
		// rotation calls; run it once here or the first layout has none.
		applyToolbarDensity()
		val topPopup = PopupWindow(holder,
			ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
		topPopup.isFocusable = false // never steal keys from the canvas
		buttonPopup = topPopup
		val decor = window.decorView
		decor.post { // runs once the decor is attached (valid window token)
			try {
				val top = ViewCompat.getRootWindowInsets(decor)?.getInsets(
					WindowInsetsCompat.Type.systemBars()
						or WindowInsetsCompat.Type.displayCutout())?.top ?: 0
				// The status-bar inset is what was still holding the card away
				// from the top edge in landscape, and it is not the padding:
				// the popup itself was being placed below it. There is nothing
				// up there to avoid when the screen is on its side.
				val land = resources.configuration.orientation ==
					android.content.res.Configuration.ORIENTATION_LANDSCAPE
				topPopup.showAtLocation(decor, Gravity.TOP or Gravity.START, 0,
					if (land) 0 else top)
				// Entrance: the card drops in from above.
				card.alpha = 0f
				card.translationY = -dp(40).toFloat()
				card.animate().alpha(1f).translationY(0f).setDuration(320L)
					.setInterpolator(android.view.animation.DecelerateInterpolator()).start()
				jlog("toolbar shown (top inset $top)")
			} catch (t: Throwable) {
				jlog("toolbar failed: ${android.util.Log.getStackTraceString(t)}")
			}
		}
	}

	// ---- Master WAV recording (⏺ toolbar button) ----
	// The native side taps the oboe output callback into a WAV file under
	// filesDir/user/recordings/; on stop the file is published to
	// Documents/RackDroid/ via MediaStore (no permission needed for files
	// this app contributes) so it's reachable from any file manager.
	private var recordingFile: File? = null

	private fun toggleRecording(button: ImageButton) {
		val current = recordingFile
		if (current == null) {
			val dir = File(filesDir, "user/recordings")
			dir.mkdirs()
			val stamp = java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US)
				.format(java.util.Date())
			val f = File(dir, "rackdroid-$stamp.wav")
			if (nativeRecordStart(f.absolutePath)) {
				recordingFile = f
				button.setImageResource(R.drawable.ic_tb_stop)
				android.widget.Toast.makeText(this, getString(R.string.toast_recording), android.widget.Toast.LENGTH_SHORT).show()
			} else {
				android.widget.Toast.makeText(this, getString(R.string.toast_recording_failed), android.widget.Toast.LENGTH_SHORT).show()
			}
		} else {
			nativeRecordStop()
			recordingFile = null
			button.setImageResource(R.drawable.ic_tb_record)
			Thread {
				val name = current.name
				try {
					val coll = android.provider.MediaStore.Files.getContentUri("external")
					val values = android.content.ContentValues().apply {
						put(android.provider.MediaStore.MediaColumns.DISPLAY_NAME, name)
						put(android.provider.MediaStore.MediaColumns.MIME_TYPE, "audio/wav")
						put(android.provider.MediaStore.MediaColumns.RELATIVE_PATH, "Documents/RackDroid/")
					}
					val uri = contentResolver.insert(coll, values)
					if (uri != null) {
						contentResolver.openOutputStream(uri)?.use { out ->
							current.inputStream().use { it.copyTo(out) }
						}
						runOnUiThread {
							android.widget.Toast.makeText(this,
								getString(R.string.toast_saved_to, name), android.widget.Toast.LENGTH_LONG).show()
						}
					}
				} catch (t: Throwable) {
					jlog("recording export failed: ${android.util.Log.getStackTraceString(t)}")
				}
			}.start()
		}
	}

	/** Virtual on-screen piano docked at the bottom of the screen, toggled
	 * by the ⌨ icon. Plays through Rack's built-in "Computer keyboard/mouse"
	 * MIDI driver (native/port/keyboard_native.cpp), so it works with any
	 * MIDI-CV module already set to that driver -- including the tutorial
	 * patch's default -- with no new MIDI routing to configure. Own window
	 * (PopupWindow), same reason as the toolbar: NativeActivity's main
	 * window never draws or receives touches for anything added to it
	 * directly. */
	private var keyboardPopup: PopupWindow? = null
	private var pianoView: PianoKeyboardView? = null

	private fun toggleVirtualKeyboard() {
		val existing = keyboardPopup
		if (existing != null) {
			pianoView?.releaseAll()
			existing.dismiss()
			keyboardPopup = null
			pianoView = null
			return
		}

		val piano = PianoKeyboardView(this).apply {
			onPress = { key -> nativeKeyboardPress(key) }
			onRelease = { key -> nativeKeyboardRelease(key) }
		}
		pianoView = piano

		fun octaveButton(label: String, down: Boolean) = Button(this).apply {
			text = label
			setOnClickListener {
				// A shift command (not a note) -- press+release right away,
				// matching a real, non-sustained key tap.
				val key = if (down) '`'.code else '1'.code
				nativeKeyboardPress(key)
				nativeKeyboardRelease(key)
			}
		}

		val row = LinearLayout(this).apply {
			orientation = LinearLayout.HORIZONTAL
			setBackgroundColor(AppTheme.current.surface)
			addView(octaveButton("‹", down = true),
				LinearLayout.LayoutParams(dp(44), ViewGroup.LayoutParams.MATCH_PARENT))
			addView(piano, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f))
			addView(octaveButton("›", down = false),
				LinearLayout.LayoutParams(dp(44), ViewGroup.LayoutParams.MATCH_PARENT))
		}
		// Close (✕) button, top-right corner over the keyboard.
		val closeBtn = ImageButton(this).apply {
			setImageResource(R.drawable.ic_tb_close)
			imageTintList = android.content.res.ColorStateList.valueOf(AppTheme.current.textPrimary)
			scaleType = ImageView.ScaleType.FIT_CENTER
			contentDescription = getString(android.R.string.cancel)
			setPadding(dp(6), dp(6), dp(6), dp(6))
			background = GradientDrawable().apply {
				shape = GradientDrawable.OVAL
				setColor(AppTheme.withAlpha(Color.BLACK, 70))
			}
			setOnClickListener {
				pianoView?.releaseAll()
				keyboardPopup?.dismiss()
				keyboardPopup = null
				pianoView = null
			}
		}
		val keyboardHolder = android.widget.FrameLayout(this).apply {
			addView(row, android.widget.FrameLayout.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
			addView(closeBtn, android.widget.FrameLayout.LayoutParams(dp(34), dp(34)).apply {
				gravity = Gravity.TOP or Gravity.END
				setMargins(0, dp(6), dp(6), 0)
			})
		}

		val popup = PopupWindow(keyboardHolder, ViewGroup.LayoutParams.MATCH_PARENT, dp(180))
		popup.isFocusable = false // never steal keys from the canvas
		keyboardPopup = popup
		val decor = window.decorView
		decor.post {
			try {
				val bottom = ViewCompat.getRootWindowInsets(decor)?.getInsets(
					WindowInsetsCompat.Type.systemBars())?.bottom ?: 0
				popup.showAtLocation(decor, Gravity.BOTTOM or Gravity.START, 0, bottom)
				jlog("virtual keyboard shown")
			} catch (t: Throwable) {
				jlog("virtual keyboard failed: ${android.util.Log.getStackTraceString(t)}")
			}
		}
	}

	private fun showCredits() {
		val text = """
			RackDroid ${packageManager.getPackageInfo(packageName, 0).versionName}
			An unofficial Android build of the VCV Rack engine.

			LICENSES
			• Engine & modules (VCV Rack, Fundamental, Bogaudio): GPL-3.0-or-later. Complete source:
			  github.com/nowheel/RackDroid
			• Component Library, Core & Fundamental panel graphics: original artwork © RackDroid, GPL-3.0.
			• Bogaudio module graphics: © Matt Demanett, CC BY-SA 4.0.
			• Fonts: DejaVu (free), Noto/Share Tech Mono/Nunito/DSEG (OFL).

			Not affiliated with or endorsed by VCV. "VCV" is a trademark of VCV and is not used here.
		""".trimIndent()
		val dialog = AlertDialog.Builder(this)
			.setTitle("Credits & licenses")
			.setMessage(text)
			.setPositiveButton(android.R.string.ok, null)
			.setNeutralButton("Log") { _, _ -> showLogViewer() }
		// Only the GitHub build can update itself; the Play build leaves that
		// to the store, so the row is not there at all (AppUpdates.SUPPORTED).
		if (AppUpdates.SUPPORTED)
			dialog.setNegativeButton(R.string.menu_check_updates) { _, _ -> AppUpdates.checkNow(this) }
		dialog.show()
	}

	/** Java-side log, mirrored to user/java-log.txt: logcat is unreachable
	 * without root, so UI-thread errors must land in a file the in-app viewer
	 * and the Documents export can reach. */
	private fun jlog(msg: String) {
		android.util.Log.i("rackdroid.java", msg)
		try {
			val f = File(filesDir, "user/java-log.txt")
			f.parentFile?.mkdirs()
			if (f.length() > 256 * 1024) f.delete()
			val ts = java.text.SimpleDateFormat("HH:mm:ss.SSS", java.util.Locale.US)
				.format(java.util.Date())
			f.appendText("[$ts] $msg\n")
		} catch (_: Throwable) {}
	}

	/** Copies both logs where any file manager can read them
	 * (Documents/RackDroid/), for when even the in-app viewer is unreachable.
	 * MediaStore needs no permission for files this app contributes.
	 *
	 * Reuses the existing entry instead of delete-then-insert: an app may only
	 * delete MediaStore rows it OWNS, and ownership is lost when the app is
	 * reinstalled (owner_package_name goes NULL). The delete therefore matched
	 * nothing, every export inserted a fresh row, and MediaStore uniquified the
	 * name -- leaking "log (1).txt", "log (2).txt", ... one file per onPause,
	 * until it ran out of candidates and threw "Failed to build unique file". */
	private fun exportLogs() {
		val coll = android.provider.MediaStore.Files.getContentUri("external")
		val prefs = getSharedPreferences("logexport", Context.MODE_PRIVATE)
		for (name in listOf("log.txt", "java-log.txt")) {
			val src = File(filesDir, "user/$name")
			if (!src.exists()) continue
			// Remember the entry we created rather than looking it up by name.
			// Two reasons a name lookup cannot work: under scoped storage a
			// query only returns non-media files this app OWNS, so a leftover
			// row from an earlier install (owner NULL) is invisible to us AND
			// still holds the name; MediaStore then uniquifies our insert to
			// "log (1).txt", which the next lookup for "log.txt" misses again.
			var uri = prefs.getString(name, null)?.let { android.net.Uri.parse(it) }
			// Two passes at most: reuse the remembered entry, and if it has gone
			// stale (user deleted it, storage cleared) claim a fresh one once.
			for (attempt in 0..1) {
				if (uri == null) {
					uri = runCatching {
						contentResolver.insert(coll, android.content.ContentValues().apply {
							put(android.provider.MediaStore.MediaColumns.DISPLAY_NAME, name)
							put(android.provider.MediaStore.MediaColumns.MIME_TYPE, "text/plain")
							put(android.provider.MediaStore.MediaColumns.RELATIVE_PATH, "Documents/RackDroid/")
						})
					}.getOrNull() ?: break
					prefs.edit().putString(name, uri.toString()).apply()
				}
				// "wt" truncates: plain "w" would leave the tail of a previous,
				// longer log behind when the new one is shorter.
				val written = runCatching {
					contentResolver.openOutputStream(uri!!, "wt")?.use { out ->
						src.inputStream().use { it.copyTo(out) }
					} != null
				}.getOrDefault(false)
				if (written) break
				prefs.edit().remove(name).apply()
				uri = null
			}
		}
	}

	override fun onPause() {
		super.onPause()
		exportLogs()
		// Otherwise a note held when the app backgrounds -- finger lifting
		// off-screen never delivers a touch-up event -- would sustain
		// forever.
		pianoView?.releaseAll()
	}

	/** On-device log viewer: tails of user/java-log.txt (UI thread) and Rack's
	 * user/log.txt (includes the rackdroid.menu traces). Termux/logcat can't
	 * read other apps' logs, so this is the diagnosis channel on-device. */
	private fun showLogViewer() {
		val javaLog = File(filesDir, "user/java-log.txt")
		val nativeLog = File(filesDir, "user/log.txt")
		val text = buildString {
			append("== java ==\n")
			append(if (javaLog.exists())
				javaLog.readLines().takeLast(60).joinToString("\n") else "(empty)")
			append("\n\n== native (log.txt) ==\n")
			append(if (nativeLog.exists())
				nativeLog.readLines().takeLast(120).joinToString("\n") else "(no file)")
		}
		val tv = TextView(this).apply {
			typeface = android.graphics.Typeface.MONOSPACE
			textSize = 10f
			setTextIsSelectable(true)
			setPadding(dp(12), dp(12), dp(12), dp(12))
			this.text = text
		}
		val scroll = ScrollView(this).apply { addView(tv) }
		AlertDialog.Builder(this)
			.setTitle("Log (tail)")
			.setView(scroll)
			.setPositiveButton(android.R.string.ok, null)
			.setNeutralButton("Copy") { _, _ -> clipboardSet(text) }
			.show()
		scroll.post { scroll.fullScroll(View.FOCUS_DOWN) }
	}

	override fun onNewIntent(intent: Intent) {
		super.onNewIntent(intent)
		// The package installer reports the progress of a self-update through
		// an intent aimed here; without this the confirmation screen is never
		// shown and the update silently never happens.
		runCatching { AppUpdates.onNewIntent(this, intent) }
		handleImportIntent(intent)
	}

	/** Copies a .vcv opened/shared from another app into the patches dir. */
	private fun handleImportIntent(intent: Intent?) {
		val uri: Uri = when (intent?.action) {
			Intent.ACTION_VIEW -> intent.data
			Intent.ACTION_SEND -> IntentCompat.getParcelableExtra(intent, Intent.EXTRA_STREAM, Uri::class.java)
			else -> null
		} ?: return
		try {
			val name = safeIncomingName(queryDisplayName(uri), "imported.vcv", listOf(".vcv"))
			val dir = File(filesDir, "user/patches")
			dir.mkdirs()
			val dest = uniqueDestination(dir, name)
			copyUriAtomically(uri, dest, MAX_PATCH_IMPORT_BYTES)
			uiHandler.post {
				Toast.makeText(this, getString(R.string.toast_patch_imported, dest.name), Toast.LENGTH_LONG).show()
			}
		} catch (e: Exception) {
			uiHandler.post {
				Toast.makeText(this, getString(R.string.toast_import_failed, e.message), Toast.LENGTH_LONG).show()
			}
		}
	}

	private fun queryDisplayName(uri: Uri): String? {
		contentResolver.query(uri, null, null, null, null)?.use { cursor ->
			val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
			if (idx >= 0 && cursor.moveToFirst())
				return cursor.getString(idx)
		}
		return uri.lastPathSegment
	}

	private fun safeIncomingName(raw: String?, fallback: String, allowedExtensions: List<String>): String {
		val base = (raw ?: fallback).replace('\\', '/').substringAfterLast('/')
		var name = base.filter { it.code in 32..126 }
			.replace(Regex("[^A-Za-z0-9._ ()+-]"), "_")
			.trim().take(128)
		if (name.isEmpty() || name == "." || name == "..") name = fallback
		if (allowedExtensions.none { name.endsWith(it, ignoreCase = true) })
			name += allowedExtensions.first()
		return name
	}

	private fun uniqueDestination(dir: File, requestedName: String): File {
		val root = dir.canonicalFile
		val dot = requestedName.lastIndexOf('.')
		val stem = if (dot > 0) requestedName.substring(0, dot) else requestedName
		val extension = if (dot > 0) requestedName.substring(dot) else ""
		for (index in 0..9999) {
			val name = if (index == 0) requestedName else "$stem ($index)$extension"
			val candidate = File(root, name).canonicalFile
			if (candidate.parentFile != root)
				throw SecurityException("import path escapes destination")
			if (!candidate.exists()) return candidate
		}
		throw IllegalStateException("too many files with the same name")
	}

	private fun copyUriAtomically(uri: Uri, destination: File, maxBytes: Long) {
		val tmp = File(destination.parentFile, ".${destination.name}.${UUID.randomUUID()}.tmp")
		try {
			val input = contentResolver.openInputStream(uri)
				?: throw IllegalArgumentException("cannot open selected file")
			input.use { source ->
				tmp.outputStream().use { output ->
					val buffer = ByteArray(64 * 1024)
					var total = 0L
					while (true) {
						val count = source.read(buffer)
						if (count < 0) break
						total += count
						if (total > maxBytes) throw IllegalArgumentException("selected file is too large")
						output.write(buffer, 0, count)
					}
				}
			}
			if (!tmp.renameTo(destination)) throw IllegalStateException("cannot finalize imported file")
		} finally {
			if (tmp.exists()) tmp.delete()
		}
	}

	private val REQ_PICK_RDMOD = 4711
	private var moduleManagerDialog: AlertDialog? = null

	/** 📥 toolbar button: the module manager. Lists every installed .rdmod pack
	 * with an uninstall button, plus an "install from file" action that opens
	 * the system picker. Rebuilt in place after install/uninstall. */
	private fun showModuleManager() {
		val col = LinearLayout(this).apply {
			orientation = LinearLayout.VERTICAL
			setPadding(dp(20), dp(18), dp(20), dp(20))
		}
		col.addView(TextView(this).apply {
			text = getString(R.string.modules_manager_title)
			setTextColor(AppTheme.current.accent)
			textSize = 17f
			setTypeface(AppFont.get(this@MainActivity), Typeface.BOLD)
			setPadding(0, 0, 0, dp(12))
		})
		// Install-from-file action.
		col.addView(Button(this).apply {
			text = getString(R.string.install_from_file)
			isAllCaps = false
			setTextColor(AppTheme.current.onAccent)
			background = GradientDrawable().apply {
				cornerRadius = dp(14).toFloat(); setColor(AppTheme.current.accent)
			}
			setOnClickListener { moduleManagerDialog?.dismiss(); confirmPickModulePacks() }
		})

		val packs = ModuleInstaller.installedPacks(this)
		if (packs.isEmpty()) {
			col.addView(TextView(this).apply {
				text = getString(R.string.no_modules_installed)
				setTextColor(AppTheme.current.textSecondary)
				textSize = 14f
				setPadding(0, dp(16), 0, 0)
			})
		} else {
			col.addView(TextView(this).apply {
				text = getString(R.string.modules_installed_header, packs.size)
				setTextColor(AppTheme.current.textSecondary)
				textSize = 12f
				letterSpacing = 0.08f
				setTypeface(AppFont.get(this@MainActivity), Typeface.BOLD)
				setPadding(0, dp(16), 0, dp(6))
			})
			for (pack in packs) {
				col.addView(LinearLayout(this).apply {
					orientation = LinearLayout.HORIZONTAL
					gravity = Gravity.CENTER_VERTICAL
					setPadding(0, dp(8), 0, dp(8))
					addView(TextView(this@MainActivity).apply {
						text = pack.slug
						setTextColor(AppTheme.current.textPrimary)
						textSize = 15f
						setTypeface(AppFont.get(this@MainActivity))
						layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
					})
					addView(TextView(this@MainActivity).apply {
						text = "%.1f MB".format(pack.sizeBytes / 1048576.0)
						setTextColor(AppTheme.current.textSecondary)
						textSize = 12f
						setPadding(0, 0, dp(12), 0)
					})
					addView(Button(this@MainActivity).apply {
						text = getString(R.string.uninstall)
						isAllCaps = false
						setTextColor(AppTheme.current.danger)
						minWidth = 0; minimumWidth = 0; minHeight = 0; minimumHeight = 0
						setPadding(dp(14), dp(8), dp(14), dp(8))
						background = GradientDrawable().apply {
							cornerRadius = dp(12).toFloat()
							setStroke(dp(1), AppTheme.withAlpha(AppTheme.current.danger, 30))
							setColor(AppTheme.withAlpha(AppTheme.current.danger, 10))
						}
						setOnClickListener { confirmUninstall(pack.slug) }
					})
				})
			}
		}

		val scroll = ScrollView(this).apply { addView(col); isVerticalScrollBarEnabled = false }
		val dlg = AlertDialog.Builder(this).create()
		dlg.setView(scroll)
		trackTopWindow(dlg)
		dlg.window?.apply {
			setBackgroundDrawable(GradientDrawable().apply {
				cornerRadius = dp(24).toFloat(); setColor(glassCardColor())
				setStroke(dp(1), AppTheme.withAlpha(Color.WHITE, 18))
			})
			setDimAmount(0.4f)
		}
		glassify(dlg.window)
		moduleManagerDialog = dlg
		dlg.show()
	}

	/** After a theme change, the toolbar/menus recolor instantly but the rack
	 * (module panels, rail, background) only updates on the next launch, when
	 * the native side copies the theme's SVGs into place before loading them.
	 * Offer to restart now. */
	private fun promptRestartForRackTheme() {
		AlertDialog.Builder(this)
			.setMessage(getString(R.string.theme_restart_message))
			.setNegativeButton(getString(R.string.theme_restart_later), null)
			.setPositiveButton(getString(R.string.theme_restart_now)) { _, _ ->
				val intent = packageManager.getLaunchIntentForPackage(packageName)
				intent?.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TASK or Intent.FLAG_ACTIVITY_NEW_TASK)
				startActivity(intent)
				Runtime.getRuntime().exit(0)
			}
			.show()
	}

	/** Rebuilds the toolbar PopupWindow in place so it picks up the current
	 * AppTheme.current colors. Cheap and precedented: every Kotlin dialog
	 * (module manager, browser, credits) already rebuilds itself from
	 * scratch each time it's shown, so only the persistent toolbar needs an
	 * explicit rebuild -- recreate()'ing the whole Activity would restart
	 * the native engine (autosave + full re-init), which is unnecessary
	 * here. */
	private fun rebuildToolbar() {
		buttonPopup?.dismiss()
		buttonPopup = null
		addMidiButton()
	}

	/** 🎨 toolbar button: pick one of the touch UI's color themes. Rack's own
	 * module-panel light/dark setting is untouched -- this only covers the
	 * Kotlin-side chrome (toolbar, menus, palette, browser, dialogs). */
	private fun showThemePicker() {
		val col = LinearLayout(this).apply {
			orientation = LinearLayout.VERTICAL
			setPadding(dp(20), dp(18), dp(20), dp(20))
		}
		col.addView(TextView(this).apply {
			text = getString(R.string.theme_picker_title)
			setTextColor(AppTheme.current.accent)
			textSize = 17f
			setTypeface(AppFont.get(this@MainActivity), Typeface.BOLD)
			setPadding(0, 0, 0, dp(12))
		})
		lateinit var dlg: AlertDialog
		for (preset in AppTheme.all) {
			val isCurrent = preset.id == AppTheme.current.id
			col.addView(LinearLayout(this).apply {
				orientation = LinearLayout.HORIZONTAL
				gravity = Gravity.CENTER_VERTICAL
				setPadding(dp(4), dp(10), dp(4), dp(10))
				background = amberRippleRounded()
				addView(View(this@MainActivity).apply {
					background = GradientDrawable().apply {
						shape = GradientDrawable.OVAL
						setColor(preset.surface)
						setStroke(dp(2), preset.accent)
					}
					layoutParams = LinearLayout.LayoutParams(dp(30), dp(30)).apply { marginEnd = dp(16) }
				})
				addView(TextView(this@MainActivity).apply {
					text = getString(preset.nameRes)
					setTextColor(AppTheme.current.textPrimary)
					textSize = 15.5f
					setTypeface(AppFont.get(this@MainActivity))
					layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
				})
				if (isCurrent) addView(TextView(this@MainActivity).apply {
					text = "✔"
					setTextColor(AppTheme.current.accent)
					textSize = 15f
					setPadding(dp(10), 0, dp(4), 0)
				})
				setOnClickListener {
					val changed = preset.id != AppTheme.current.id
					AppTheme.set(this@MainActivity, preset)
					dlg.dismiss()
					rebuildToolbar()
					pianoView?.applyTheme()
					Toast.makeText(this@MainActivity,
						getString(R.string.toast_theme_applied, getString(preset.nameRes)), Toast.LENGTH_SHORT).show()
					// The chrome (toolbar/menus/palette) is themed live; the
					// rack panels/rail/background need the native startup copy,
					// so offer a restart.
					if (changed) promptRestartForRackTheme()
				}
			})
		}
		val scroll = ScrollView(this).apply { addView(col); isVerticalScrollBarEnabled = false }
		dlg = AlertDialog.Builder(this).create()
		dlg.setView(scroll)
		trackTopWindow(dlg)
		dlg.window?.apply {
			setBackgroundDrawable(GradientDrawable().apply {
				cornerRadius = dp(24).toFloat(); setColor(glassCardColor())
				setStroke(dp(1), AppTheme.withAlpha(Color.WHITE, 18))
			})
			setDimAmount(0.4f)
		}
		glassify(dlg.window)
		dlg.show()
	}

	private fun confirmUninstall(slug: String) {
		AlertDialog.Builder(this)
			.setTitle(getString(R.string.uninstall_title, slug))
			.setMessage(getString(R.string.uninstall_message))
			.setNegativeButton(android.R.string.cancel, null)
			.setPositiveButton(R.string.uninstall) { _, _ ->
				val ok = ModuleInstaller.uninstall(this, slug)
				if (ok) {
					ThumbnailCache.removePlugin(slug)
					// Drop it from the live registry so its modules leave the
					// palette immediately, then refresh the palette snapshot.
					runCatching { nativeBrowserUnloadPlugin(slug) }
					runCatching { modulePalette.reload() }
				}
				Toast.makeText(this,
					getString(if (ok) R.string.uninstalled_restart else R.string.install_failed),
					Toast.LENGTH_LONG).show()
				moduleManagerDialog?.dismiss()
				showModuleManager() // rebuild the list without the removed pack
			}
			.show()
	}

	/** Pick .rdmod pack file(s) from anywhere on the device (Downloads, etc.)
	 * with the system file picker. onActivityResult copies them into the
	 * Modules folder and loads them live -- no restart, no storage permission
	 * (the picker grants per-file read access). */
	private fun pickModulePacks() {
		val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
			addCategory(Intent.CATEGORY_OPENABLE)
			// .rdmod has no registered MIME type; accept anything, filter by name.
			type = "*/*"
			putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
		}
		try {
			startActivityForResult(intent, REQ_PICK_RDMOD)
		} catch (t: Throwable) {
			Toast.makeText(this, getString(R.string.install_failed), Toast.LENGTH_LONG).show()
		}
	}

	/** Deleting is the one toolbar action that destroys work, and on a touch
	 * screen the selection is easy to lose track of -- so it always asks, and
	 * says how many modules are going. Undo still covers a confirmed mistake. */
	private fun confirmDeleteSelection() {
		val count = runCatching { nativeSelectionCount() }.getOrDefault(0)
		if (count <= 0) {
			Toast.makeText(this, R.string.delete_modules_none, Toast.LENGTH_SHORT).show()
			return
		}
		AlertDialog.Builder(this)
			.setTitle(R.string.delete_modules_title)
			.setMessage(resources.getQuantityString(R.plurals.delete_modules_message, count, count))
			.setNegativeButton(android.R.string.cancel, null)
			.setPositiveButton(R.string.delete_action) { _, _ -> nativeDeleteSelection() }
			.show()
	}

	private fun confirmPickModulePacks() {
		AlertDialog.Builder(this)
			.setTitle(R.string.module_pack_security_title)
			.setMessage(R.string.module_pack_security_message)
			.setNegativeButton(android.R.string.cancel, null)
			.setPositiveButton(R.string.continue_action) { _, _ -> pickModulePacks() }
			.show()
	}

	@Deprecated("Deprecated in Java")
	override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
		super.onActivityResult(requestCode, resultCode, data)
		if (requestCode != REQ_PICK_RDMOD || resultCode != RESULT_OK || data == null) return
		val uris = ArrayList<Uri>()
		data.clipData?.let { clip -> for (i in 0 until clip.itemCount) uris.add(clip.getItemAt(i).uri) }
		data.data?.let { uris.add(it) }
		if (uris.isEmpty()) return
		Toast.makeText(this, getString(R.string.installing_modules), Toast.LENGTH_SHORT).show()
		// Copy off the UI thread (a pack can be 10+ MB); load back on it, so the
		// native registration path matches the startup loader exactly.
		Thread {
			val copied = uris.count { uri ->
				runCatching {
					val name = safeIncomingName(queryDisplayName(uri), "pack.rdmod", listOf(".rdmod", ".zip"))
					val dest = uniqueDestination(ModuleInstaller.modulesDir(this), name)
					copyUriAtomically(uri, dest, MAX_MODULE_PACK_BYTES)
					true
				}.getOrDefault(false)
			}
			uiHandler.post {
				if (copied > 0) {
					// loadUserPlugins imports+loads the new packs and toasts the
					// count; already-installed slugs are skipped harmlessly.
					ModuleInstaller.loadUserPlugins(this)
					runCatching { nativeBrowserRequestBuild() }
					runCatching { modulePalette.reload() }
				} else {
					Toast.makeText(this, getString(R.string.install_failed), Toast.LENGTH_LONG).show()
				}
			}
		}.start()
	}

	/** Called from native when a synthetic Help row is selected:
	 * 0 = guide sheet, 1 = tutorial library, 3 = the interface tour again.
	 * Re-running the tour passes no done-flag: it is an explicit request, and
	 * must not re-arm or clear the first-run state either way. */
	/** The one audio question the engine cannot answer by measuring: how much
	 * delay is worth trading for how much safety. It cannot know whether the
	 * person holding the phone is playing a keyboard or building a patch and
	 * listening, and those two want opposite things. Everything else about the
	 * audio path is measured and decided automatically -- this is the
	 * exception, which is why it is the only one that asks.
	 *
	 * The answer does not fix a size: it fixes how low the automatic tuning is
	 * allowed to go. The engine keeps finding the best point inside the band
	 * the user chose. */
	fun showLatencyPickerFromNative() {
		uiHandler.post { runCatching { showLatencyPicker() } }
	}

	private fun latencyMode(): Int =
		getSharedPreferences(LATENCY_PREFS, Context.MODE_PRIVATE).getInt(LATENCY_KEY, 1)

	/** Pushes the stored choice down to the audio side. Called at startup and
	 * whenever it changes -- the native side keeps it in memory only, so Java
	 * stays the single place it is remembered. */
	fun applyLatencyMode() {
		runCatching { nativeSetLatencyMode(latencyMode()) }
	}

	private fun showLatencyPicker() {
		val current = latencyMode()
		val labels = arrayOf(
			getString(R.string.latency_play),
			getString(R.string.latency_balanced),
			getString(R.string.latency_listen))
		val notes = arrayOf(
			getString(R.string.latency_play_note),
			getString(R.string.latency_balanced_note),
			getString(R.string.latency_listen_note))
		val col = LinearLayout(this).apply {
			orientation = LinearLayout.VERTICAL
			setPadding(dp(20), dp(18), dp(20), dp(8))
		}
		col.addView(TextView(this).apply {
			text = getString(R.string.menu_response)
			setTextColor(AppTheme.current.accent)
			textSize = 17f
			setTypeface(AppFont.get(this@MainActivity), Typeface.BOLD)
			setPadding(0, 0, 0, dp(10))
		})
		col.addView(TextView(this).apply {
			text = getString(R.string.latency_explain)
			setTextColor(AppTheme.current.textSecondary)
			textSize = 14f
			typeface = AppFont.get(this@MainActivity)
			setPadding(0, 0, 0, dp(14))
		})
		lateinit var dlg: AlertDialog
		for (i in labels.indices) {
			col.addView(LinearLayout(this).apply {
				orientation = LinearLayout.VERTICAL
				setPadding(dp(6), dp(10), dp(6), dp(10))
				background = amberRippleRounded()
				addView(TextView(this@MainActivity).apply {
					text = (if (i == current) "✔  " else "     ") + labels[i]
					setTextColor(if (i == current) AppTheme.current.accent
						else AppTheme.current.textPrimary)
					textSize = 16f
					setTypeface(AppFont.get(this@MainActivity),
						if (i == current) Typeface.BOLD else Typeface.NORMAL)
				})
				addView(TextView(this@MainActivity).apply {
					text = notes[i]
					setTextColor(AppTheme.current.textSecondary)
					textSize = 13f
					typeface = AppFont.get(this@MainActivity)
					setPadding(dp(28), dp(2), 0, 0)
				})
				setOnClickListener {
					getSharedPreferences(LATENCY_PREFS, Context.MODE_PRIVATE)
						.edit().putInt(LATENCY_KEY, i).apply()
					applyLatencyMode()
					Toast.makeText(this@MainActivity, labels[i], Toast.LENGTH_SHORT).show()
					dlg.dismiss()
				}
			})
		}
		// Same glass card the theme picker uses. A default AlertDialog comes up
		// white here, which put pale text on a pale background and made the
		// third option unreadable; and in landscape the card has to scroll, or
		// the last choice sits below the screen edge where it cannot be
		// reached at all.
		dlg = AlertDialog.Builder(this).create()
		dlg.setView(ScrollView(this).apply { addView(col) })
		trackTopWindow(dlg)
		dlg.window?.apply {
			setBackgroundDrawable(GradientDrawable().apply {
				cornerRadius = dp(24).toFloat(); setColor(glassCardColor())
				setStroke(dp(1), AppTheme.withAlpha(Color.WHITE, 18))
			})
			setDimAmount(0.4f)
		}
		glassify(dlg.window)
		dlg.show()
	}

	fun showHelpFromNative(which: Int) {
		uiHandler.post {
			runCatching {
				when (which) {
					0 -> GuideSheet(this).show()
					3 -> Tour(this, doneFlag = null).show()
					else -> TutorialLibrarySheet(this).show()
				}
			}
		}
	}

	private var wizard: Wizard? = null
	private var activeTutorial: Wizard? = null

	/** Bottom module palette (🧩): type chips + draggable thumbnails. */
	private val modulePalette by lazy {
		ModulePalette(this,
			getModelsJson = { nativeBrowserModelsJson() },
			getModelsGeneration = { nativeBrowserModelsGeneration() },
			requestBuild = { nativeBrowserRequestBuild() },
			chooseAt = { key, x, y -> runCatching { nativeBrowserChooseAt(key, x, y) } })
	}

	/** Start (or switch to) a tutorial from the library: only one floating
	 * card at a time. */
	fun startTutorial(t: Tutorial) {
		wizard?.close()
		activeTutorial?.close()
		val w = Wizard(this, t.title, t.steps, doneFlag = null)
		activeTutorial = w
		w.show()
	}

	// ---- Help-on-top window tracking ----
	// The wizard is a PopupWindow attached to a window's decor; any Dialog
	// shown later (module browser, menu sheet, guide) stacks ABOVE it. Every
	// help-aware dialog registers here; on attach/detach the wizard popup is
	// re-anchored to the topmost decor so the tutorial stays visible while
	// the user follows its instructions into the browser or the menus.
	private val helpTopWindows = ArrayList<View>()

	fun trackTopWindow(dialog: Dialog) {
		val decor = dialog.window?.decorView ?: return
		decor.addOnAttachStateChangeListener(object : View.OnAttachStateChangeListener {
			override fun onViewAttachedToWindow(v: View) { helpTopWindows.add(v); reanchorWizards() }
			override fun onViewDetachedFromWindow(v: View) { helpTopWindows.remove(v); reanchorWizards() }
		})
	}

	fun wizardAnchor(): View = helpTopWindows.lastOrNull() ?: window.decorView

	private fun reanchorWizards() {
		uiHandler.post {
			runCatching {
				val anchor = wizardAnchor()
				wizard?.reanchor(anchor)
				activeTutorial?.reanchor(anchor)
			}
		}
	}

	/** Called from native (menu_native.cpp processShare) with the path of
	 * the .vcv it just archived under user/share/. */
	fun sharePatchFromNative(path: String) {
		uiHandler.post { runCatching { sharePatch(File(path)) } }
	}

	private fun sharePatch(file: File) {
		val uri = FileProvider.getUriForFile(this, "$packageName.fileprovider", file)
		val send = Intent(Intent.ACTION_SEND)
			.setType("application/octet-stream")
			.putExtra(Intent.EXTRA_STREAM, uri)
			.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
		startActivity(Intent.createChooser(send, file.name))
	}

	override fun onDestroy() {
		if (isFinishing)
			markStartupReady()
		if (recordingFile != null) {
			// Finalize the WAV header so an in-flight recording stays playable.
			try { nativeRecordStop() } catch (_: Throwable) {}
			recordingFile = null
		}
		buttonPopup?.dismiss()
		buttonPopup = null
		audioFocusRequest?.let {
			(getSystemService(Context.AUDIO_SERVICE) as? AudioManager)?.abandonAudioFocusRequest(it)
		}
		audioFocusRequest = null
		RackService.stop(this)
		if (activeActivity?.get() === this)
			activeActivity = null
		super.onDestroy()
	}

	override fun onTrimMemory(level: Int) {
		super.onTrimMemory(level)
		ThumbnailCache.trim(level)
	}

	override fun onLowMemory() {
		super.onLowMemory()
		ThumbnailCache.clear()
	}

	/** Immersive fullscreen: without this the Android status bar overlaps
	 * Rack's menu bar (File/Edit/View...) and swallows its touches. System
	 * bars reappear transiently with an edge swipe. */
	private fun hideSystemBars() {
		WindowCompat.setDecorFitsSystemWindows(window, false)
		WindowInsetsControllerCompat(window, window.decorView).let {
			it.hide(WindowInsetsCompat.Type.statusBars() or WindowInsetsCompat.Type.navigationBars())
			it.systemBarsBehavior =
				WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
		}
	}

	override fun onWindowFocusChanged(hasFocus: Boolean) {
		super.onWindowFocusChanged(hasFocus)
		// Bars sticky-reappear after dialogs/app switches: re-hide.
		if (hasFocus) {
			hideSystemBars()
			// And the first moment the cutout is knowable. Asking during the
			// toolbar's decor.post is too early when the app STARTS in
			// landscape -- the insets come back zero, and nothing else asks
			// again, so the park bar stayed under the camera until the first
			// rotation. Focus arrives after the window is really laid out.
			publishCutoutToNative()
			publishToolbarBottomToNative()
		}
	}

	// ---- Clipboard (called from native, any thread) ----

	fun clipboardSet(text: String) {
		uiHandler.post {
			val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
			cm.setPrimaryClip(ClipData.newPlainText("RackDroid", text))
		}
	}

	fun clipboardGet(): String {
		var result = ""
		val latch = CountDownLatch(1)
		uiHandler.post {
			try {
				val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
				result = cm.primaryClip?.getItemAt(0)?.coerceToText(this)?.toString() ?: ""
			} finally {
				latch.countDown()
			}
		}
		latch.await()
		return result
	}

	// ---- Thermal status (called from native, any thread) ----

	/** Android's own throttling verdict (PowerManager.THERMAL_STATUS_*):
	NONE=0, LIGHT=1, MODERATE=2, SEVERE=3, CRITICAL=4, EMERGENCY=5,
	SHUTDOWN=6. Read synchronously like clipboardGet() above -- PowerManager
	wants no particular thread, but posting through uiHandler keeps every
	Android API call in this file on the one thread, which is one fewer thing
	to get wrong. */
	fun currentThermalStatus(): Int {
		var result = PowerManager.THERMAL_STATUS_NONE
		val latch = CountDownLatch(1)
		uiHandler.post {
			try {
				val pm = getSystemService(Context.POWER_SERVICE) as? PowerManager
				result = pm?.currentThermalStatus ?: PowerManager.THERMAL_STATUS_NONE
			} finally {
				latch.countDown()
			}
		}
		latch.await()
		return result
	}

	/** A one-off, non-blocking notice from native -- the same Toast the
	module installer already uses for "Loaded N extra module pack(s)", so a
	message from the engine looks like everything else the app already says
	unprompted, not like a new kind of interruption. */
	fun showToastFromNative(text: String) {
		uiHandler.post {
			Toast.makeText(this, text, Toast.LENGTH_LONG).show()
		}
	}

	/** kind: 0 = engine_maxed_out, 1 = engine_thermal_throttled -- see
	checkMaxedOutOverload() in main_android.cpp for when each fires. Native
	has no Android string resources of its own, so it passes which notice,
	not the text; this resolves and localizes it, then reuses the Toast
	above to show it. */
	fun showEngineNoticeFromNative(kind: Int) {
		// 1 = the phone is throttling itself, 2 = another app is eating the CPU,
		// 0 = the patch really is heavier than this device can manage. Telling
		// the last two apart matters: "simplify your patch" is bad advice when
		// the patch is fine and something else is the load.
		val res = when (kind) {
			1 -> R.string.engine_thermal_throttled
			2 -> R.string.engine_cpu_contended
			else -> R.string.engine_maxed_out
		}
		showToastFromNative(getString(res))
	}

	// ---- Audio focus (called from native, once, before the Oboe stream opens) ----

	/** RackDroid produces continuous audio via a MEDIA_PLAYBACK foreground
	service (RackService) -- the same category of app Android expects to hold
	audio focus, and requesting it is not just etiquette here: AAudio's audio
	policy service weighs the caller's focus/attributes state when deciding
	whether to grant an Exclusive (dedicated, low-latency) stream or fall back
	to Shared (mixed through AudioFlinger). A real OnePlus 8T logged
	"sharing=Shared" -- with materially worse, less consistent latency than
	the Exclusive path the block-size and thread-priority tuning elsewhere in
	this codebase were built around -- while another app was also audible and
	RackDroid had never once asked for focus. No amount of tuning on our side
	of that stream fixes a mode Android chose not to grant us.
	AUDIOFOCUS_GAIN (not _TRANSIENT): this is not a one-off sound, and Rack's
	own design keeps the engine and its audio running even in the background
	(see APP_CMD_LOST_FOCUS in main_android.cpp) -- deliberately unlike a
	normal media player, we do NOT pause or duck on AUDIOFOCUS_LOSS below,
	only log it, so this cannot make that existing behavior any less
	surprising than it already was. Requested once and held for the process's
	life; released in onDestroy(). Returns false (native logs it) if the
	system denied focus outright -- the Oboe stream still opens either way. */
	fun requestAudioFocusFromNative(): Boolean {
		var granted = false
		val latch = CountDownLatch(1)
		uiHandler.post {
			try {
				val am = getSystemService(Context.AUDIO_SERVICE) as? AudioManager
				if (am == null) return@post
				val attrs = AudioAttributes.Builder()
					.setUsage(AudioAttributes.USAGE_MEDIA)
					.setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
					.build()
				val request = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
					.setAudioAttributes(attrs)
					.setOnAudioFocusChangeListener { change ->
						jlog("AudioManager focus change: $change (engine keeps running regardless -- see requestAudioFocusFromNative)")
					}
					.build()
				granted = am.requestAudioFocus(request) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
				if (granted)
					audioFocusRequest = request
			} finally {
				latch.countDown()
			}
		}
		latch.await()
		return granted
	}

	// ---- Async dialogs (called from the native glue thread) ----
	// The native caller does NOT block on a latch: it keeps pumping its
	// event loop and receives the outcome via nativeDialogInt/String
	// (jni_bridge.cpp). Blocking the glue thread starved NativeActivity's
	// input queue and triggered input-dispatch ANRs.

	/** osdialog levels: 0=info 1=warning 2=error; buttons: 0=ok 1=ok/cancel 2=yes/no
	 *  The dismiss listener also fires after a button click; `answered`
	 *  guards ensure the native side sees exactly one result. */
	fun dialogMessageAsync(level: Int, buttons: Int, message: String) {
		uiHandler.post {
			var answered = false
			fun answer(r: Int) { if (!answered) { answered = true; nativeDialogInt(r) } }
			// The port layer replaces prompts whose wording does not survive the
			// trip to Android with a marker; the text lives here, where it is
			// translated. See NEW_PATCH_MARKER in osdialog_android.cpp.
			val text = if (message == "@rackdroid:new_patch_confirm")
				getString(R.string.patch_new_confirm) else message
			val b = AlertDialog.Builder(this)
				.setMessage(text)
				.setCancelable(false)
			val positive = if (buttons == 2) getString(android.R.string.yes) else getString(android.R.string.ok)
			b.setPositiveButton(positive) { _, _ -> answer(1) }
			if (buttons != 0) {
				val negative = if (buttons == 2) getString(android.R.string.no) else getString(android.R.string.cancel)
				b.setNegativeButton(negative) { _, _ -> answer(0) }
			}
			b.setOnDismissListener { answer(0) }
			b.show()
		}
	}

	fun dialogPromptAsync(title: String, text: String) {
		uiHandler.post {
			val edit = EditText(this)
			edit.inputType = InputType.TYPE_CLASS_TEXT
			edit.setText(text)
			edit.setSelectAllOnFocus(true)
			var answered = false
			AlertDialog.Builder(this)
				.setTitle(title)
				.setView(edit)
				.setPositiveButton(android.R.string.ok) { _, _ ->
					answered = true; nativeDialogString(edit.text.toString())
				}
				.setNegativeButton(android.R.string.cancel) { _, _ ->
					answered = true; nativeDialogString(null)
				}
				.setOnDismissListener { if (!answered) { answered = true; nativeDialogString(null) } }
				.show()
			edit.requestFocus()
		}
	}

	/** save=false: single-choice list of .vcv files in dir. save=true: filename prompt. */
	fun dialogFileAsync(save: Boolean, dir: String, filename: String) {
		if (save) {
			uiHandler.post {
				val edit = EditText(this)
				edit.inputType = InputType.TYPE_CLASS_TEXT
				edit.setText(filename.ifEmpty { "patch.vcv" })
				edit.setSelectAllOnFocus(true)
				var answered = false
				AlertDialog.Builder(this)
					.setTitle("Save patch")
					.setView(edit)
					.setPositiveButton(android.R.string.ok) { _, _ ->
						answered = true
						var name = edit.text.toString()
						if (name.isEmpty()) {
							nativeDialogString(null)
						} else {
							if (!name.endsWith(".vcv")) name += ".vcv"
							nativeDialogString(File(dir, name).absolutePath)
						}
					}
					.setNegativeButton(android.R.string.cancel) { _, _ ->
						answered = true; nativeDialogString(null)
					}
					.setOnDismissListener { if (!answered) { answered = true; nativeDialogString(null) } }
					.show()
				edit.requestFocus()
			}
			return
		}

		val files = File(dir).listFiles { f -> f.isFile && f.name.endsWith(".vcv") }
			?.sortedBy { it.name } ?: emptyList()
		if (files.isEmpty()) {
			uiHandler.post {
				android.widget.Toast.makeText(this, getString(R.string.toast_no_patches, dir), android.widget.Toast.LENGTH_SHORT).show()
			}
			nativeDialogString(null)
			return
		}
		uiHandler.post {
			var answered = false
			val dialog = AlertDialog.Builder(this)
				.setTitle("Open patch  (long-press to share)")
				.setItems(files.map { it.name }.toTypedArray()) { _, which ->
					answered = true; nativeDialogString(files[which].absolutePath)
				}
				.setNegativeButton(android.R.string.cancel) { _, _ ->
					answered = true; nativeDialogString(null)
				}
				.setOnDismissListener { if (!answered) { answered = true; nativeDialogString(null) } }
				.create()
			dialog.show()
			dialog.listView?.setOnItemLongClickListener { _, _, position, _ ->
				sharePatch(files[position])
				true
			}
		}
	}

	// ---- Native Android menus (bottom sheet) ----
	// Flags must match menu_native.cpp
	private val ROW_DISABLED = 1
	private val ROW_LABEL = 4
	private val ROW_SEPARATOR = 8
	private val ROW_BACK = 16
	private val ROW_SHARE = 32
	private val ROW_GUIDE = 64
	private val ROW_WIZARD = 128
	private val ROW_WIZARD_PRO = 256
	private val ROW_SLIDER_TENSION = 512
	private val ROW_SLIDER_OPACITY = 1024
	private val ROW_PRESET_COPY = 2048
	private val ROW_PRESET_PASTE = 4096
	private val ROW_TOUR = 8192
	private val ROW_LATENCY = 16384
	// Rack's own fixed, non-localized markers (ui/common.hpp) for a
	// submenu's current-value display and a checkbox's checked state.
	private val RIGHT_ARROW = "▸"
	private val CHECKMARK = "✔"

	private var menuDialog: Dialog? = null

	private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

	/** Glassmorphism for sheet/dialog windows: blur what's behind (the GL
	 * rack) and let a translucent warm-dark card sit on top -- the look of
	 * the reference design. Cross-window blur can be disabled system-wide
	 * (battery saver, developer setting); isCrossWindowBlurEnabled picks a
	 * nearly-opaque fallback so the card never turns to unreadable mud. */
	fun glassify(window: android.view.Window?, radiusDp: Int = 48) {
		window ?: return
		if (crossWindowBlurEnabled(windowManager)) {
			window.addFlags(android.view.WindowManager.LayoutParams.FLAG_BLUR_BEHIND)
			window.attributes = window.attributes.apply { blurBehindRadius = dp(radiusDp) }
		}
	}

	fun glassCardColor(): Int =
		if (crossWindowBlurEnabled(windowManager)) AppTheme.withAlpha(AppTheme.current.surface, 80)
		else AppTheme.withAlpha(AppTheme.current.surface, 96)

	/** Dismisses menuDialog, if any, without telling native (its dismissal
	 * was either native-initiated already, or is about to be replaced by a
	 * new sheet). Nulling the listener -- instead of a suppress-flag held
	 * only across the dismiss() call -- is what makes this safe: Dialog's
	 * onDismiss can be posted to the message queue rather than firing
	 * synchronously, so a flag reset immediately after dismiss() can race
	 * it and let a stale dismiss signal through. That signal, arriving
	 * after a *different*, newly-captured menu has already reset its own
	 * shown/active state, is exactly what tripped the "did not show"
	 * fallback (menu_native.cpp) and permanently disabled native menus for
	 * the rest of the session -- reported live as "the original menus
	 * showing under the android ones" once every subsequent menu fell back
	 * to canvas rendering. Nulling the listener has no such window: once
	 * cleared, that dialog instance can never call back again, regardless
	 * of when Android actually tears it down. */
	private fun closeMenuDialogSilently() {
		menuDialog?.setOnDismissListener(null)
		menuDialog?.dismiss()
		menuDialog = null
	}

	/** Called from native (jni_bridge) to show a menu as a bottom sheet. */
	fun showNativeMenu(labels: Array<String>, rights: Array<String>, flags: IntArray) {
		uiHandler.post {
			try {
				jlog("showNativeMenu: ${labels.size} rows")
				closeMenuDialogSilently()
				menuDialog = buildMenuSheet(labels, rights, flags).also {
					it.show()
					if (tourMenuDemo) it.window?.addFlags(
						android.view.WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or
						android.view.WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE)
				}
				nativeMenuShown() // tell native the sheet is up (hide canvas)
				jlog("showNativeMenu: sheet up")
			} catch (t: Throwable) {
				jlog("showNativeMenu FAILED: ${android.util.Log.getStackTraceString(t)}")
				// Fall back to the canvas menu; never crash.
				runCatching { nativeMenuDismiss() }
			}
		}
	}

	fun dismissNativeMenu() {
		uiHandler.post { closeMenuDialogSilently() }
	}

	/** Called from native (browser_native.cpp) when Rack's own module
	 * browser opens; the canvas widget is already hidden by the time this
	 * arrives.
	 *
	 * This used to raise a full-screen browser sheet. There is now a single
	 * module picker -- the bottom palette -- so the gesture opens that instead,
	 * on its ALL category (the palette pulls the model list itself via
	 * nativeBrowserModelsJson, cached natively and built once). */
	fun showNativeBrowser() {
		uiHandler.post {
			try {
				jlog("showNativeBrowser -> palette")
				modulePalette.showAll()
			} catch (t: Throwable) {
				jlog("showNativeBrowser FAILED: ${android.util.Log.getStackTraceString(t)}")
			}
		}
	}

	/** A ScrollView that lets the user swipe the whole sheet DOWN to dismiss it
	 * (standard bottom-sheet gesture). The drag only engages when the content
	 * is already scrolled to the top, so normal list scrolling still works. */
	private inner class SwipeDismissScrollView(val onDismiss: () -> Unit)
		: ScrollView(this@MainActivity) {
		private var startRawY = 0f
		private var dragging = false
		private val slop = android.view.ViewConfiguration.get(context).scaledTouchSlop

		override fun onInterceptTouchEvent(ev: android.view.MotionEvent): Boolean {
			when (ev.actionMasked) {
				android.view.MotionEvent.ACTION_DOWN -> { startRawY = ev.rawY; dragging = false }
				android.view.MotionEvent.ACTION_MOVE ->
					if (scrollY == 0 && ev.rawY - startRawY > slop) { dragging = true; return true }
			}
			return super.onInterceptTouchEvent(ev)
		}

		override fun onTouchEvent(ev: android.view.MotionEvent): Boolean {
			when (ev.actionMasked) {
				android.view.MotionEvent.ACTION_DOWN -> { startRawY = ev.rawY; return true }
				android.view.MotionEvent.ACTION_MOVE -> if (dragging || scrollY == 0) {
					dragging = true
					translationY = (ev.rawY - startRawY).coerceAtLeast(0f)
					return true
				}
				android.view.MotionEvent.ACTION_UP, android.view.MotionEvent.ACTION_CANCEL ->
					if (dragging) {
						dragging = false
						if (translationY > height * 0.22f) {
							animate().translationY(height.toFloat()).setDuration(180L)
								.withEndAction { onDismiss() }.start()
						} else {
							animate().translationY(0f).setDuration(160L)
								.setInterpolator(android.view.animation.DecelerateInterpolator()).start()
						}
						return true
					}
			}
			return super.onTouchEvent(ev)
		}
	}

	private fun buildMenuSheet(labels: Array<String>, rights: Array<String>, flags: IntArray): Dialog {
		val col = LinearLayout(this).apply {
			orientation = LinearLayout.VERTICAL
			// Rounded-top glass card with a grab handle: translucent over
			// the blurred rack (glassify below), drawn by hand.
			background = GradientDrawable().apply {
				setColor(glassCardColor())
				cornerRadii = floatArrayOf(
					dp(28).toFloat(), dp(28).toFloat(), dp(28).toFloat(), dp(28).toFloat(),
					0f, 0f, 0f, 0f)
				// Liquid-glass rim: a whisper of white along the edge.
				setStroke(dp(1), AppTheme.withAlpha(Color.WHITE, 18))
			}
			setPadding(dp(8), 0, dp(8), dp(14))
		}
		col.addView(View(this).apply {
			background = GradientDrawable().apply {
				setColor(AppTheme.current.surfaceInset)
				cornerRadius = dp(2).toFloat()
			}
			layoutParams = LinearLayout.LayoutParams(dp(36), dp(4)).apply {
				gravity = Gravity.CENTER_HORIZONTAL
				topMargin = dp(10); bottomMargin = dp(8)
			}
		})

		// AlertDialog is the same path used by the working save/open dialogs.
		val dlg = AlertDialog.Builder(this).create()

		for (i in labels.indices) {
			val f = flags[i]
			when {
				f and ROW_SEPARATOR != 0 -> col.addView(View(this).apply {
					setBackgroundColor(AppTheme.withAlpha(Color.WHITE, 8))
					layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1))
						.apply { topMargin = dp(7); bottomMargin = dp(7); leftMargin = dp(12); rightMargin = dp(12) }
				})
				f and ROW_LABEL != 0 -> col.addView(TextView(this).apply {
					text = labels[i].uppercase()
					setTextColor(AppTheme.current.accent)
					textSize = 11f
					letterSpacing = 0.1f
					setTypeface(AppFont.get(this@MainActivity), Typeface.BOLD)
					setPadding(dp(16), dp(12), dp(16), dp(4))
				})
				f and ROW_SLIDER_TENSION != 0 -> col.addView(sliderRow(
					getString(R.string.cable_tension), nativeGetCableTension()) { nativeSetCableTension(it) })
				f and ROW_SLIDER_OPACITY != 0 -> col.addView(sliderRow(
					getString(R.string.cable_opacity), nativeGetCableOpacity()) { nativeSetCableOpacity(it) })
				else -> col.addView(menuRow(labels[i], rights[i], f, i))
			}
		}

		val scroll = SwipeDismissScrollView({ dlg.dismiss() }).apply {
			addView(col)
			isVerticalScrollBarEnabled = false
		}
		dlg.setView(scroll)
		trackTopWindow(dlg)
		// Staggered entrance: rows drift up and fade in one after another.
		dlg.setOnShowListener {
			for (i in 0 until col.childCount) {
				val v = col.getChildAt(i)
				v.alpha = 0f
				v.translationY = dp(16).toFloat()
				v.animate().alpha(1f).translationY(0f)
					.setStartDelay((i * 14).toLong()).setDuration(190L)
					.setInterpolator(android.view.animation.DecelerateInterpolator()).start()
			}
		}
		// Only reached for a genuine user dismissal (tap outside, back):
		// every programmatic close goes through closeMenuDialogSilently(),
		// which detaches this listener first.
		dlg.setOnDismissListener { nativeMenuDismiss() }
		// Anchor as a bottom sheet, full width; transparent window so the
		// card's rounded corners actually show. All of this must happen
		// BEFORE show(): styling from an OnShowListener let the dialog's
		// default light background paint for a frame or two (reported as a
		// white menu flashing open), and the default fade animation read
		// as lag -- the keyboard's quick slide-up matches a bottom sheet.
		dlg.window?.apply {
			setBackgroundDrawable(android.graphics.drawable.ColorDrawable(Color.TRANSPARENT))
			setGravity(Gravity.BOTTOM)
			setDimAmount(0.3f) // lighter: the blur already separates layers
			attributes = attributes.apply {
				width = ViewGroup.LayoutParams.MATCH_PARENT
				height = ViewGroup.LayoutParams.WRAP_CONTENT
				windowAnimations = android.R.style.Animation_InputMethod
			}
		}
		glassify(dlg.window)
		return dlg
	}

	/** Desktop keyboard shortcuts ("Ctrl+N", "F11", "Backspace/Delete") are
	 * meaningless on a device with no physical keyboard -- strip them.
	 * Right-side text carrying real information (a submenu's current value,
	 * e.g. "60 Hz ▸", or a checkbox's checked state, "✔") must stay, and
	 * both are safe to recognize by Rack's own fixed, non-localized marker
	 * characters (ui/common.hpp RIGHT_ARROW/CHECKMARK_STRING): a shortcut
	 * string built by widget::getKeyCommandName() never contains either. */
	private fun displayRightText(right: String): String {
		if (right.contains(RIGHT_ARROW)) return right // value (+ arrow): keep as-is
		return if (right.contains(CHECKMARK)) CHECKMARK else "" // drop the shortcut, keep the check
	}

	/** A titled SeekBar row (0..100%) for a global cable setting. `initial` is
	 * the native 0..1 value; `onSet` writes the new 0..1 value back live. */
	private fun sliderRow(label: String, initial: Float, onSet: (Float) -> Unit): View {
		val amber = android.content.res.ColorStateList.valueOf(AppTheme.current.accent)
		val pct = Math.round(initial * 100).coerceIn(0, 100)
		val valueLabel = TextView(this).apply {
			setTextColor(AppTheme.current.accent)
			textSize = 13f
			setTypeface(AppFont.get(this@MainActivity), Typeface.BOLD)
			text = "$pct%"
		}
		val head = LinearLayout(this).apply {
			orientation = LinearLayout.HORIZONTAL
			gravity = Gravity.CENTER_VERTICAL
			addView(TextView(this@MainActivity).apply {
				text = label
				setTextColor(AppTheme.current.textPrimary)
				textSize = 15f
				setTypeface(AppFont.get(this@MainActivity))
				layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
			})
			addView(valueLabel)
		}
		val bar = android.widget.SeekBar(this).apply {
			max = 100
			progress = pct
			progressTintList = amber
			thumbTintList = amber
			setOnSeekBarChangeListener(object : android.widget.SeekBar.OnSeekBarChangeListener {
				override fun onProgressChanged(sb: android.widget.SeekBar, p: Int, fromUser: Boolean) {
					valueLabel.text = "$p%"
					if (fromUser) onSet(p / 100f)
				}
				override fun onStartTrackingTouch(sb: android.widget.SeekBar) {}
				override fun onStopTrackingTouch(sb: android.widget.SeekBar) {}
			})
		}
		return LinearLayout(this).apply {
			orientation = LinearLayout.VERTICAL
			setPadding(dp(16), dp(6), dp(16), dp(8))
			addView(head)
			addView(bar)
		}
	}

	private fun menuRow(label: String, right: String, flags: Int, index: Int): View {
		val disabled = flags and ROW_DISABLED != 0
		val back = flags and ROW_BACK != 0

		val row = LinearLayout(this).apply {
			orientation = LinearLayout.HORIZONTAL
			gravity = Gravity.CENTER_VERTICAL
			minimumHeight = dp(50)
			setPadding(dp(16), dp(11), dp(14), dp(11))
			layoutParams = LinearLayout.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
			if (!disabled) background = amberRippleRounded()
		}
		val left = TextView(this).apply {
			text = when {
				back -> "‹   " + getString(R.string.menu_back)
				flags and ROW_SHARE != 0 -> getString(R.string.menu_share_patch)
				flags and ROW_LATENCY != 0 -> getString(R.string.menu_response)
				flags and ROW_GUIDE != 0 -> getString(R.string.menu_guide)
				flags and ROW_WIZARD != 0 -> getString(R.string.menu_wizard)
					flags and ROW_TOUR != 0 -> getString(R.string.menu_tour)
				flags and ROW_WIZARD_PRO != 0 -> getString(R.string.menu_wizard_pro)
					// Upstream labels these just "Copy"/"Paste" -- the same
					// words the toolbar uses for the modules themselves. They
					// carry only this module's settings, so say that.
					flags and ROW_PRESET_COPY != 0 -> getString(R.string.menu_preset_copy)
					flags and ROW_PRESET_PASTE != 0 -> getString(R.string.menu_preset_paste)
				else -> label
			}
			setTextColor(when {
				disabled -> AppTheme.current.textDisabled
				back -> AppTheme.current.accent
				else -> AppTheme.current.textPrimary
			})
			textSize = 16f
			setTypeface(AppFont.get(this@MainActivity), if (back) Typeface.BOLD else Typeface.NORMAL)
			layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
		}
		row.addView(left)
		val shownRight = displayRightText(right)
		if (shownRight.isNotEmpty()) {
			// Split a submenu's "value ▸" so the value reads quiet grey and
			// the chevron gets the muted modern "›"; a bare ✔ turns amber.
			val isCheck = shownRight == CHECKMARK
			val value = shownRight.removeSuffix(RIGHT_ARROW).trim()
			if (value.isNotEmpty() && !isCheck) row.addView(TextView(this).apply {
				text = value
				typeface = AppFont.get(this@MainActivity)
				setTextColor(AppTheme.current.textSecondary)
				textSize = 14f
				setPadding(dp(12), 0, 0, 0)
			})
			row.addView(TextView(this).apply {
				text = if (isCheck) CHECKMARK else "›"
				setTextColor(if (isCheck) AppTheme.current.accent else AppTheme.current.textDisabled)
				textSize = if (isCheck) 15f else 19f
				setPadding(dp(10), 0, dp(2), if (isCheck) 0 else dp(2))
			})
		}
		if (!disabled) row.setOnClickListener {
			nativeMenuSelect(index)
			closeMenuDialogSilently()
		}
		return row
	}

	/** Amber-tinted ripple clipped to a rounded pill: the pressed state that
	 * matches the app accent, replacing the stock grey edge-to-edge ripple. */
	private fun amberRippleRounded(): android.graphics.drawable.Drawable {
		val mask = GradientDrawable().apply {
			cornerRadius = dp(12).toFloat()
			setColor(Color.WHITE)
		}
		return android.graphics.drawable.RippleDrawable(
			android.content.res.ColorStateList.valueOf(AppTheme.withAlpha(AppTheme.current.accent, 20)), null, mask)
	}

	private external fun nativeMenuSelect(index: Int)
	private external fun nativeMenuDismiss()
	private external fun nativeMenuShown()
	private external fun nativeBackPressed()
	private external fun nativeToolbarTap(index: Int)
	private external fun nativeBrowserModelsJson(): String
	private external fun nativeBrowserModelsGeneration(): Long
	private external fun nativeBrowserChooseAt(key: String, x: Float, y: Float)
	private external fun nativeBrowserRequestBuild(): Long
	private external fun nativeBrowserUnloadPlugin(slug: String)
	private external fun nativeLoadUserPlugin(dir: String, soname: String): Boolean
	private external fun nativeIsPluginLoaded(slug: String): Boolean

	/** Called from native (jni_bridge) during startup, BEFORE the autosaved
	 * patch is restored; the native side blocks until nativeUserPluginsLoaded()
	 * comes back.
	 *
	 * Ordering matters more than it looks. A pack's .so can only be brought in
	 * by Java System.load(), so this cannot be done natively -- but if the
	 * patch is restored first, Rack finds its side-loaded modules missing and
	 * puts up a modal "this patch includes modules that are not installed"
	 * prompt. That prompt blocks the UI thread, which is exactly the thread
	 * that would have loaded the packs: the module the dialog calls missing is
	 * the one whose loading the dialog itself is preventing. The patch then
	 * loses those modules on every single launch. */
	fun loadUserPluginsFromNative() {
		runOnUiThread {
			try {
				ModuleInstaller.loadUserPlugins(this)
			} catch (t: Throwable) {
				jlog("loading side-loaded module packs failed: $t")
			} finally {
				// Must fire whatever happened, or startup stalls until the
				// native watchdog gives up.
				runCatching { nativeUserPluginsLoaded() }
			}
		}
	}

	private external fun nativeUserPluginsLoaded()
	private external fun nativeSetCableParkVisible(visible: Boolean)
	private external fun nativeSetStartupOptions(safeMode: Boolean, skipUserPlugins: Boolean)
	private external fun nativeSetChosenLanguage(code: String)

	/** Called from native once the patch has been restored and the engine is
	 * running. Building the model list needs every plugin registered, and the
	 * palette needs that list -- both were previously fired on 3.4 s / 4 s
	 * timers, which is a guess about how long startup takes, not a fact about
	 * it. Slow device or slow patch and the palette came up empty. */
	fun patchReadyFromNative() {
		uiHandler.post {
			markStartupReady()
			runCatching { nativeBrowserRequestBuild() }
			runCatching { modulePalette.show() }
			showStartupRecoveryDialog()
		}
	}

	private fun styleCableParkButton() {
		val b = cableParkButton ?: return
		b.alpha = if (cableParkOn) 1f else 0.55f
		b.imageTintList = android.content.res.ColorStateList.valueOf(
			if (cableParkOn) AppTheme.current.accent else AppTheme.current.textPrimary)
	}

	/** Called by ModuleInstaller after Java System.load()'s a pack's .so. */
	fun loadUserPluginNative(dir: String, soname: String): Boolean =
		runCatching { nativeLoadUserPlugin(dir, soname) }.getOrDefault(false)

	/** Called by ModuleInstaller to skip packs whose slug is already
	 * registered, avoiding a redundant System.load + dlopen per pack on
	 * every re-import. */
	fun isPluginLoadedNative(slug: String): Boolean =
		runCatching { nativeIsPluginLoaded(slug) }.getOrDefault(false)
	private external fun nativeKeyboardPress(key: Int)
	private external fun nativeKeyboardRelease(key: Int)
	private external fun nativeDialogInt(result: Int)
	private external fun nativeDialogString(s: String?)
	private external fun nativeRecordStart(path: String): Boolean
	private external fun nativeRecordStop()
	private external fun nativeHistoryAction(action: Int)
	private external fun nativeSetLockMode(mode: Int)
	private external fun nativeSetLatencyMode(mode: Int)
	private external fun nativeSetMultiSelect(on: Boolean)
	private external fun nativeCopySelection()
	private external fun nativePasteSelection()
	private external fun nativeDeleteSelection()
	private external fun nativeSelectionCount(): Int
	private external fun nativeCableParkBounds(): IntArray?
	private external fun nativeCableParkLeftInset(px: Int)
	private external fun nativeCableParkTopInset(px: Int)
	private external fun nativeTourDemo(what: Int)
	private external fun nativeTourStage(x: Int, y: Int, w: Int, h: Int)
	private external fun nativeRackModuleCount(): Int
	private external fun nativeGetCableTension(): Float
	private external fun nativeSetCableTension(v: Float)
	private external fun nativeGetCableOpacity(): Float
	private external fun nativeSetCableOpacity(v: Float)

	/** A native Dialog (menu/browser sheet) already consumes back on its own
	 * window before this is ever called -- this only fires when the canvas
	 * itself has something open (e.g. the module browser), which otherwise
	 * exits the whole Activity since NativeActivity's default handling
	 * treats an unconsumed back as "finish". Always consume: closing
	 * whatever's open, never leaving via back, is safer than losing a patch
	 * to an accidental tap.
	 */
	override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
		if (keyCode == KeyEvent.KEYCODE_BACK) {
			nativeBackPressed()
			return true
		}
		return super.onKeyDown(keyCode, event)
	}

	// ---- MIDI (MidiManager -> native AMidi driver) ----

	private fun initMidi() {
		if (!packageManager.hasSystemFeature(PackageManager.FEATURE_MIDI))
			return
		val mm = getSystemService(Context.MIDI_SERVICE) as MidiManager
		midiManager = mm
		for (info in mm.devices)
			openMidiDevice(info)
		mm.registerDeviceCallback(object : MidiManager.DeviceCallback() {
			override fun onDeviceAdded(info: MidiDeviceInfo) = openMidiDevice(info)
			override fun onDeviceRemoved(info: MidiDeviceInfo) {
				val toRemove = midiDevices.filterValues { it.info.id == info.id }
				for ((id, dev) in toRemove) {
					midiDevices.remove(id)
					nativeMidiDeviceRemoved(id)
					runCatching { dev.close() }
				}
			}
		}, uiHandler)
	}

	private fun openMidiDevice(info: MidiDeviceInfo) {
		val mm = midiManager ?: return
		mm.openDevice(info, { device -> if (device != null) registerOpenedDevice(device) }, uiHandler)
	}

	/** Registers an opened MidiDevice (USB or BLE) with the native driver. */
	private fun registerOpenedDevice(device: MidiDevice) {
		val info = device.info
		val id = nextMidiId.getAndIncrement()
		midiDevices[id] = device
		val props = info.properties
		val name = props.getString(MidiDeviceInfo.PROPERTY_NAME)
			?: props.getString(MidiDeviceInfo.PROPERTY_PRODUCT) ?: "MIDI device $id"
		nativeMidiDeviceAdded(id, name, device, info.inputPortCount, info.outputPortCount)
		uiHandler.post { Toast.makeText(this, getString(R.string.toast_midi_connected, name), Toast.LENGTH_SHORT).show() }
	}

	// ---- Bluetooth LE MIDI ----

	private val midiServiceUuid = ParcelUuid(
		UUID.fromString("03B80E5A-EDE8-4B33-A751-6CE34EC4C700"))
	private var bleScanner: BluetoothLeScanner? = null
	private var bleCallback: ScanCallback? = null

	private fun showBleMidiScanner() {
		// BLUETOOTH_SCAN/CONNECT don't exist before API 31 -- requesting an
		// unrecognized permission is silently auto-denied there, so BLE scan
		// would never work on API 29/30 without this branch. Below 31, a BLE
		// scan instead needs ACCESS_FINE_LOCATION at runtime (BLUETOOTH/
		// BLUETOOTH_ADMIN are normal, install-time only -- see manifest).
		val required = if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.S)
			arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
		else
			arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
		val need = required.filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
		if (need.isNotEmpty()) {
			requestPermissions(need.toTypedArray(), 2)
			return
		}
		val adapter = (getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
		if (adapter == null || !adapter.isEnabled) {
			Toast.makeText(this, getString(R.string.toast_enable_bluetooth), Toast.LENGTH_LONG).show()
			return
		}
		val scanner = adapter.bluetoothLeScanner ?: return
		bleScanner = scanner

		val found = LinkedHashMap<String, BluetoothDevice>()
		val names = ArrayList<String>()
		val adapterList = android.widget.ArrayAdapter(this,
			android.R.layout.simple_list_item_1, names)

		val dialog = AlertDialog.Builder(this)
			.setTitle("Bluetooth MIDI  (scanning…)")
			.setAdapter(adapterList) { _, which ->
				stopBleScan()
				connectBleDevice(found.values.toList()[which])
			}
			.setNegativeButton("Close") { _, _ -> stopBleScan() }
			.setOnDismissListener { stopBleScan() }
			.create()
		dialog.show()

		bleCallback = object : ScanCallback() {
			@Suppress("MissingPermission")
			override fun onScanResult(callbackType: Int, result: ScanResult) {
				val dev = result.device ?: return
				val addr = dev.address ?: return
				if (found.put(addr, dev) == null) {
					names.add(dev.name ?: addr)
					adapterList.notifyDataSetChanged()
				}
			}
			override fun onScanFailed(errorCode: Int) {
				uiHandler.post { Toast.makeText(this@MainActivity,
					getString(R.string.toast_ble_scan_failed, errorCode), Toast.LENGTH_LONG).show() }
			}
		}
		val filter = ScanFilter.Builder().setServiceUuid(midiServiceUuid).build()
		val settings = ScanSettings.Builder()
			.setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
		try {
			scanner.startScan(listOf(filter), settings, bleCallback!!)
		} catch (e: SecurityException) {
			Toast.makeText(this, "Bluetooth permission denied", Toast.LENGTH_LONG).show()
		}
		// Auto-stop after 15s to save battery.
		uiHandler.postDelayed({ stopBleScan() }, 15000)
	}

	private fun stopBleScan() {
		val cb = bleCallback ?: return
		try {
			bleScanner?.stopScan(cb)
		} catch (_: SecurityException) {}
		bleCallback = null
	}

	private fun connectBleDevice(device: BluetoothDevice) {
		val mm = midiManager ?: (getSystemService(Context.MIDI_SERVICE) as MidiManager).also { midiManager = it }
		Toast.makeText(this, "Connecting…", Toast.LENGTH_SHORT).show()
		mm.openBluetoothDevice(device, { opened ->
			if (opened != null) registerOpenedDevice(opened)
			else uiHandler.post { Toast.makeText(this, "BLE MIDI connection failed", Toast.LENGTH_LONG).show() }
		}, uiHandler)
	}

	override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
		super.onRequestPermissionsResult(requestCode, permissions, grantResults)
		if (requestCode == 2 && grantResults.all { it == PackageManager.PERMISSION_GRANTED })
			showBleMidiScanner()
	}

	private external fun nativeMidiDeviceAdded(id: Int, name: String, device: MidiDevice, inputPorts: Int, outputPorts: Int)
	private external fun nativeMidiDeviceRemoved(id: Int)

	companion object {
		private const val LATENCY_PREFS = "audio"
		private const val LATENCY_KEY = "latencyMode"
		private const val MAX_PATCH_IMPORT_BYTES = 256L * 1024 * 1024
		private const val MAX_MODULE_PACK_BYTES = 256L * 1024 * 1024
		@Volatile private var activeActivity: WeakReference<MainActivity>? = null

		/** Notification action entry point. Finishing the task, rather than only
		 * stopping the service, guarantees the native audio stream is closed too. */
		fun requestStopFromNotification() {
			val activity = activeActivity?.get() ?: return
			activity.runOnUiThread { activity.finishAndRemoveTask() }
		}

		init {
			// Load rack_engine explicitly so the JVM registers it for JNI
			// method resolution: the native menu callbacks (nativeMenuSelect/
			// Dismiss) live there. Loading only rackdroid would pull it in as
			// a linker dependency but NOT register it for external methods.
			System.loadLibrary("rack_engine")
			// rackdroid carries the MIDI callbacks and the app entry point.
			System.loadLibrary("rackdroid")
		}
	}
}
