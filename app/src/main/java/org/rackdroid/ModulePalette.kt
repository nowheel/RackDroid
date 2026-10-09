package org.rackdroid

import android.app.Activity
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.PopupWindow
import android.widget.ScrollView
import android.widget.TextView
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import org.json.JSONArray

/** Quick module palette: a bottom bar of type chips (VCO, VCF, VCA, …) that
 * opens a horizontal, scrollable strip of module thumbnails. Tap a tile to
 * place the module at the center of the screen; LONG-PRESS and DRAG it up
 * onto the rack to drop it exactly where the finger lands (the native side
 * moves the virtual cursor to the drop point before placing).
 *
 * Toggled by the 🧩 button in the bottom tools bar; sits just above it. */
class ModulePalette(
	private val activity: Activity,
	private val getModelsJson: () -> String,
	private val getModelsGeneration: () -> Long,
	private val requestBuild: () -> Long,
	private val chooseAt: (String, Float, Float) -> Unit,
) {
	companion object {
		/** Catch-all chips, kept as constants because [showAll] has to find the
		 * ALL chip again in the row to open it. */
		private const val ALL_CHIP = "ALL"
		private const val MISC_CHIP = "MISC"
	}

	private fun dp(v: Int) = (v * activity.resources.displayMetrics.density).toInt()

	private data class Entry(val key: String, val name: String, val brand: String,
		val tags: List<String>, val description: String, val plugin: String)

	/** The tag-based categories, in chip order. Labels are the universal
	 * modular shorthand, deliberately not localized. */
	private val tagCategories: List<Pair<String, (Entry) -> Boolean>> = listOf(
		"VCO" to { e -> "Oscillator" in e.tags && "Low-frequency oscillator" !in e.tags },
		"LFO" to { e -> "Low-frequency oscillator" in e.tags },
		"VCF" to { e -> "Filter" in e.tags },
		"VCA" to { e -> "Voltage-controlled amplifier" in e.tags },
		"ENV" to { e -> "Envelope generator" in e.tags },
		"SEQ" to { e -> "Sequencer" in e.tags },
		"DRUM" to { e -> "Drum" in e.tags },
		"MIX" to { e -> "Mixer" in e.tags },
		"FX" to { e -> e.tags.any { it in setOf("Effect", "Delay", "Reverb", "Chorus", "Distortion", "Flanger", "Phaser") } },
		"NOISE" to { e -> "Noise" in e.tags },
		"QNT" to { e -> "Quantizer" in e.tags },
		"MIDI" to { e -> "MIDI" in e.tags },
		"UTIL" to { e -> "Utility" in e.tags },
	)

	/** Chip label -> predicate, as shown in the bar.
	 *
	 * The palette is the ONLY module picker (there is no full-screen browser
	 * any more), so it must be able to reach EVERY registered model. The tag
	 * categories above cannot guarantee that: a module whose tags match none
	 * of them -- untagged, or tagged something we don't list -- would be
	 * unreachable. Hence the two bookends:
	 *   ALL  first, the complete list, so nothing is ever more than one tap away
	 *   MISC last, exactly the models no tag category claims, so browsing
	 *        category by category is exhaustive rather than quietly lossy. */
	private val categories: List<Pair<String, (Entry) -> Boolean>> =
		listOf<Pair<String, (Entry) -> Boolean>>(ALL_CHIP to { _ -> true }) +
		tagCategories +
		listOf<Pair<String, (Entry) -> Boolean>>(
			MISC_CHIP to { e -> tagCategories.none { (_, pred) -> pred(e) } })

	private var popup: PopupWindow? = null
	private var ghost: PopupWindow? = null
	private var ghostW = 0
	private var ghostH = 0
	private var ghostView: ImageView? = null
	/** Whether the finger is currently over a valid drop target, so the visual
	 * state is only re-animated when it actually changes. */
	private var ghostValid = true
	private var ghostLastX = 0f
	private var ghostLastY = 0f
	/** Screen centre of the tile the drag started from, for the fly-back. */
	private var dragOriginX = 0f
	private var dragOriginY = 0f
	private var entries: List<Entry> = emptyList()
	private lateinit var strip: RecyclerView
	/** A palette dimension, cut by a third in landscape -- the same reduction
	 * the toolbar takes there, and for the same reason: the bar and the card
	 * between them were leaving the rack a slot. The thumbnails are rendered to
	 * the height asked for, so a smaller tile is a smaller render, not a
	 * scaled-down one. */
	private fun pDp(v: Int): Int {
		val land = activity.resources.configuration.orientation ==
			android.content.res.Configuration.ORIENTATION_LANDSCAPE
		return if (land) dp((v * 0.7f).toInt().coerceAtLeast(2)) else dp(v)
	}

	private lateinit var chipRow: LinearLayout
	private var activeChip: TextView? = null
	/** The chip the interface tour folded away, so it can be put back on. */
	private var foldedChip: String? = null
	private val adapter = TileAdapter()

	// ---- search / brand filter ----
	// Its own window pinned to the TOP of the screen, not a panel inside the
	// bottom bar: the soft keyboard takes the bottom half, so a search field
	// down there is typed into blind. Results sit right under the field, two
	// rows deep, using the same tiles as the palette.
	private var searchPopup: PopupWindow? = null
	private val searchAdapter = TileAdapter(inSearch = true)
	private lateinit var searchInput: EditText
	private lateinit var searchGrid: RecyclerView
	private lateinit var brandRow: LinearLayout
	/** Stands in for the result grid when nothing matches, so the popup keeps
	 * its height instead of collapsing under the finger mid-typing. */
	private lateinit var emptyLabel: TextView
	private var searchButton: TextView? = null
	/** Predicate of the open category chip; null when none is open. */
	private var activePred: ((Entry) -> Boolean)? = null
	private var query: String = ""
	private var brandFilter: String? = null
	private var pendingSearchAnnouncement: Runnable? = null
	/** Invalidates an older rebuild waiter when another pack operation arrives. */
	private var rebuildToken = 0

	private fun filtersActive() = query.isNotBlank() || brandFilter != null

	fun toggle() {
		if (popup != null) hide() else show()
	}

	/** Open the bar with the complete module list already showing. This is what
	 * Rack's "add module" gesture (long-press on empty rack) now lands on --
	 * it replaces the old full-screen browser sheet, so it has to present the
	 * whole catalogue, not a category the user has to guess at. */
	fun showAll() {
		show()
		// show() may still be laying the bar out (it posts to the decor view),
		// and the chips do not exist until it has; open ALL once they do.
		chipRow.post {
			runCatching {
				val chip = (0 until chipRow.childCount)
					.map { chipRow.getChildAt(it) as TextView }
					.firstOrNull { it.text == ALL_CHIP } ?: return@runCatching
				if (activeChip !== chip) chip.performClick()
			}
		}
	}

	/** Re-read the model list after new packs are installed at runtime. The
	 * native generation advances only after the render thread has published a
	 * complete JSON snapshot, so this never guesses how long a rebuild takes. */
	fun reload() {
		entries = emptyList()
		val token = ++rebuildToken
		val before = runCatching { requestBuild() }.getOrElse { return }
		val deadline = android.os.SystemClock.uptimeMillis() + 10_000L

		fun awaitPublishedSnapshot() {
			if (token != rebuildToken || activity.isFinishing || activity.isDestroyed)
				return
			val ready = runCatching { getModelsGeneration() > before }.getOrDefault(false)
			if (ready) {
				if (runCatching { loadEntries() }.getOrDefault(false)) {
					// A newly installed pack brings its own brand with it.
					runCatching { buildBrandChips() }
					// A category/search may be open: apply the new snapshot now.
					runCatching { refresh() }
					runCatching { refreshSearch() }
				}
				return
			}
			if (android.os.SystemClock.uptimeMillis() < deadline)
				activity.window.decorView.postOnAnimation { awaitPublishedSnapshot() }
		}
		activity.window.decorView.postOnAnimation { awaitPublishedSnapshot() }
	}

	fun hide() {
		// Closing the bar closes its search window too, keyboard included --
		// otherwise it would float over a rack with no palette behind it.
		closeSearch()
		// Detach the shared adapter before the window goes: show() builds a new
		// RecyclerView every time and setAdapter registers a data observer on the
		// adapter, which a dismissed popup never removes. Housekeeping, not a
		// known bug -- it keeps notifications from reaching lists that are gone.
		// Same for the chip reference, whose view tree is about to disappear.
		if (::strip.isInitialized)
			strip.adapter = null
		activeChip = null
		popup?.dismiss()
		popup = null
	}

	/** Screen bounds of the palette as it currently stands, or null while it is
	 * not laid out. Its height depends on whether the tile strip is open, so the
	 * interface tour has to measure it rather than assume a size. */
	fun bounds(): android.graphics.Rect? {
		val v = popup?.contentView ?: return null
		if (!v.isAttachedToWindow || v.width == 0 || v.height == 0) return null
		val loc = IntArray(2)
		v.getLocationOnScreen(loc)
		return android.graphics.Rect(loc[0], loc[1], loc[0] + v.width, loc[1] + v.height)
	}

	/** Fold the tile strip away for the interface tour, remembering the chip
	 * that was on. In landscape the toolbar and an open palette leave no rack
	 * between them, and a demonstration behind the tiles is one nobody sees --
	 * but hiding the whole bar and reopening it with showAll() would hand the
	 * user back a palette on ALL when they had left it on ENV. Folding keeps
	 * the chip bar, and unfold puts their category back on. Returns true if
	 * there was anything to fold. */
	fun foldForTour(): Boolean {
		val chip = activeChip ?: return false
		foldedChip = chip.text?.toString()
		collapseStrip()
		return true
	}

	/** Re-open the chip the tour folded, if it folded one. */
	fun unfoldForTour() {
		val want = foldedChip ?: return
		foldedChip = null
		reopenChip(want)
	}

	/** Rebuild the bar for a new screen orientation. EVERY size it has is
	 * decided while show() runs -- the tile height, the thumbnail the cache is
	 * asked for, how far up from the bottom edge the window sits -- and a
	 * rotation runs none of it. Left alone, a bar opened in portrait keeps
	 * portrait tiles in landscape, where they are tall enough to cover half the
	 * rack and hide two of the cable park bar's three holes behind them.
	 *
	 * Rebuilding is what show() already does, so this is the cheap fix rather
	 * than a second set of measurements to keep in step with the first. The one
	 * thing worth carrying across is the category that was open -- the same
	 * thing the tour's fold/unfold pair takes care to preserve. */
	fun relayoutForOrientation() {
		if (popup == null) return
		val want = activeChip?.text?.toString()
		hide()
		show()
		if (want != null) reopenChip(want)
	}

	/** Click the chip with this label once the bar it belongs to exists. */
	private fun reopenChip(want: String) {
		chipRow.post {
			runCatching {
				val chip = (0 until chipRow.childCount)
					.map { chipRow.getChildAt(it) as TextView }
					.firstOrNull { it.text == want } ?: return@runCatching
				if (activeChip !== chip) chip.performClick()
			}
		}
	}

	/** Collapse only the open tile strip (the "modules menu"), keeping the chip
	 * bar itself visible -- same as tapping the active chip again. */
	private fun collapseStrip() {
		val chip = activeChip ?: return
		chip.background = chipBg(false)
		chip.setTextColor(AppTheme.current.textPrimary)
		chip.isSelected = false
		activeChip = null
		activePred = null
		if (::strip.isInitialized) strip.visibility = View.GONE
		if (::emptyLabel.isInitialized) emptyLabel.visibility = View.GONE
	}

	/** Palette container: a downward swipe closes the OPEN tile strip (not the
	 * whole bar). Engages only when a category is open and only for a clearly
	 * DOWNWARD drag past the touch slop, so tapping/scrolling chips and
	 * long-press-drag of a tile UP onto the rack all keep working. */
	private inner class SwipeDownPalette(ctx: android.content.Context) : android.widget.FrameLayout(ctx) {
		private var startRawY = 0f
		private var startRawX = 0f
		private var dragging = false
		private val slop = android.view.ViewConfiguration.get(ctx).scaledTouchSlop

		override fun onInterceptTouchEvent(ev: MotionEvent): Boolean {
			when (ev.actionMasked) {
				MotionEvent.ACTION_DOWN -> { startRawY = ev.rawY; startRawX = ev.rawX; dragging = false }
				MotionEvent.ACTION_MOVE -> {
					if (activeChip == null) return false // nothing open to close
					val dy = ev.rawY - startRawY
					if (dy > slop * 1.5f && dy > kotlin.math.abs(ev.rawX - startRawX)) {
						dragging = true; return true
					}
				}
			}
			return false
		}

		override fun onTouchEvent(ev: MotionEvent): Boolean {
			when (ev.actionMasked) {
				MotionEvent.ACTION_MOVE -> if (dragging) {
					translationY = (ev.rawY - startRawY).coerceAtLeast(0f); return true
				}
				MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> if (dragging) {
					dragging = false
					if (translationY > dp(36)) collapseStrip()
					animate().translationY(0f).setDuration(150L)
						.setInterpolator(android.view.animation.DecelerateInterpolator()).start()
					return true
				}
			}
			return false
		}
	}

	/** Show the persistent palette bar (idempotent). */
	fun show() {
		if (popup != null)
			return
		if (entries.isEmpty())
			reload()

		val font = AppFont.get(activity)
		strip = RecyclerView(activity).apply {
			layoutManager = LinearLayoutManager(activity, LinearLayoutManager.HORIZONTAL, false)
			this.adapter = this@ModulePalette.adapter
			setBackgroundColor(Color.TRANSPARENT)
			setPadding(dp(6), dp(8), dp(6), dp(4))
			clipToPadding = false
			visibility = View.GONE
			layoutAnimation = android.view.animation.LayoutAnimationController(
				android.view.animation.AnimationSet(true).apply {
					addAnimation(android.view.animation.AlphaAnimation(0f, 1f))
					addAnimation(android.view.animation.TranslateAnimation(
						dp(24).toFloat(), 0f, 0f, 0f))
					duration = 180
					interpolator = android.view.animation.DecelerateInterpolator()
				}, 0.04f)
		}
		chipRow = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
		// Leading 🔍 chip: opens the panel above. Kept inside the same scrolling
		// row so it scrolls away with the categories instead of eating width.
		searchButton = TextView(activity).apply {
			text = "🔍"
			contentDescription = activity.getString(R.string.palette_search_modules)
			textSize = 12f
			setTypeface(font, Typeface.BOLD)
			setTextColor(AppTheme.current.textPrimary)
			background = chipBg(false)
			setPadding(dp(12), dp(7), dp(12), dp(7))
			layoutParams = LinearLayout.LayoutParams(
				ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT
			).apply { setMargins(dp(4), dp(6), dp(4), dp(8)) }
			setOnClickListener { toggleSearch() }
		}
		chipRow.addView(searchButton)
		for ((label, pred) in categories) {
			chipRow.addView(TextView(activity).apply {
				text = label
				contentDescription = activity.getString(R.string.palette_category, label)
				textSize = 12f
				setTypeface(font, Typeface.BOLD)
				setTextColor(AppTheme.current.textPrimary)
				background = chipBg(false)
				setPadding(dp(14), dp(7), dp(14), dp(7))
				layoutParams = LinearLayout.LayoutParams(
					ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT
				).apply { setMargins(dp(4), dp(6), dp(4), dp(8)) }
				setOnClickListener { selectChip(this, label, pred) }
			})
		}
		val chipScroll = HorizontalScrollView(activity).apply {
			isHorizontalScrollBarEnabled = false
			addView(chipRow)
		}
		val card = LinearLayout(activity).apply {
			orientation = LinearLayout.VERTICAL
			background = GradientDrawable().apply {
				cornerRadius = dp(20).toFloat()
				setColor(AppTheme.withAlpha(AppTheme.current.surface, 85))
				setStroke(dp(1), AppTheme.edge(15))
			}
			clipToOutline = true
			addView(strip, LinearLayout.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
			addView(chipScroll)
		}
		val content = SwipeDownPalette(activity).apply {
			setPadding(dp(8), 0, dp(8), 0)
			addView(card)
		}
		val p = PopupWindow(content,
			ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
		p.isFocusable = false
		popup = p
		val decor = activity.window.decorView
		decor.post {
			runCatching {
				// The bar is shown one frame late (insets are only known then),
				// and hide() can land in between -- a second tap on the palette
				// button, say. Showing it anyway would leave a window nobody
				// holds a reference to any more, so it could never be dismissed.
				if (popup !== p) return@runCatching
				// Landscape sits the bar on the bottom edge. The gesture inset
				// plus six points left a strip of rack showing underneath it,
				// which reads as the bar not reaching the bottom of the screen
				// -- and in landscape the gesture bar runs along that edge
				// anyway, so the room it asks for is room the bar already
				// leaves by being a bar.
				val land = activity.resources.configuration.orientation ==
					android.content.res.Configuration.ORIENTATION_LANDSCAPE
				val bottom = if (land) 0 else (ViewCompat.getRootWindowInsets(decor)
					?.getInsets(WindowInsetsCompat.Type.systemBars())?.bottom ?: 0) + dp(6)
				p.showAtLocation(decor, Gravity.BOTTOM or Gravity.START, 0, bottom)
				card.alpha = 0f
				card.translationY = dp(32).toFloat()
				card.animate().alpha(1f).translationY(0f).setDuration(260L)
					.setInterpolator(android.view.animation.DecelerateInterpolator()).start()
			}
		}
	}

	private fun chipBg(active: Boolean) = GradientDrawable().apply {
		cornerRadius = dp(16).toFloat()
		setColor(if (active) Color.WHITE else AppTheme.current.surfaceInset)
		if (!active) setStroke(dp(1), AppTheme.edge(12))
	}

	private fun selectChip(chip: TextView, label: String, pred: (Entry) -> Boolean) {
		if (activeChip === chip) {
			// Tap the active chip again: collapse the strip.
			chip.background = chipBg(false)
			chip.setTextColor(AppTheme.current.textPrimary)
			chip.isSelected = false
			activeChip = null
			activePred = null
			strip.visibility = View.GONE
			if (::emptyLabel.isInitialized) emptyLabel.visibility = View.GONE
			return
		}
		activeChip?.background = chipBg(false)
		activeChip?.setTextColor(AppTheme.current.textPrimary)
		activeChip?.isSelected = false
		activeChip = chip
		activePred = pred
		chip.background = chipBg(true)
		chip.setTextColor(AppTheme.current.onAccent)
		chip.isSelected = true
		refresh()
	}

	/** Bottom bar: the open category, nothing else. Search and brand live in
	 * their own window now, so this no longer has to compose three filters. */
	private fun refresh() {
		val pred = activePred ?: return
		if (entries.isEmpty()) {
			reload()
			return
		}
		val items = entries.asSequence().filter(pred)
			// Beginner-friendly ordering: the stock VCV modules first, then A-Z.
			.sortedWith(compareBy({ it.brand != "VCV" }, { it.name }))
			.toList()
		adapter.submit(items)
		strip.visibility = if (items.isEmpty()) View.GONE else View.VISIBLE
		if (items.isNotEmpty()) strip.scheduleLayoutAnimation()
	}

	/** Search window: whatever matches the text and the brand chip, across the
	 * whole catalogue. Deliberately NOT intersected with the category open in
	 * the bottom bar -- typing a name should find it wherever it lives. */
	private fun refreshSearch() {
		if (!::searchGrid.isInitialized)
			return
		if (entries.isEmpty()) {
			reload()
			return
		}
		val q = query.trim().lowercase()
		val items = entries.asSequence()
			.filter { brandFilter == null || it.brand == brandFilter }
			.filter {
				q.isEmpty() || it.name.lowercase().contains(q) ||
					it.brand.lowercase().contains(q) ||
					it.tags.any { t -> t.lowercase().contains(q) }
			}
			.sortedWith(compareBy({ it.brand != "VCV" }, { it.name }))
			.toList()
		searchAdapter.submit(items)
		// Crossfade grid <-> "no matches" instead of snapping between them.
		crossfade(searchGrid, items.isNotEmpty())
		crossfade(emptyLabel, items.isEmpty())
		if (items.isNotEmpty()) searchGrid.scheduleLayoutAnimation()
		announceSearchResults(items.size)
	}

	private fun announceSearchResults(count: Int) {
		if (!filtersActive()) return
		pendingSearchAnnouncement?.let(searchGrid::removeCallbacks)
		val announcement = Runnable {
			if (searchPopup != null)
				searchGrid.announceForAccessibility(
					activity.getString(R.string.palette_results_count, count))
		}
		pendingSearchAnnouncement = announcement
		searchGrid.postDelayed(announcement, 300L)
	}

	/** Fades a view in or out instead of flipping visibility, so swapping the
	 * grid for the "no matches" line does not flash. */
	private fun crossfade(v: View, show: Boolean) {
		if (show && v.visibility == View.VISIBLE) return
		if (!show && v.visibility == View.GONE) return
		v.animate().cancel()
		if (show) {
			v.alpha = 0f
			v.visibility = View.VISIBLE
			v.animate().alpha(1f).setDuration(130L).start()
		} else {
			v.animate().alpha(0f).setDuration(110L)
				.withEndAction { v.visibility = View.GONE }.start()
		}
	}

	private fun toggleSearch() {
		if (searchPopup != null) closeSearch() else openSearch()
	}

	/** Raises the search window at the top of the screen.
	 *
	 * Focusable, unlike the palette bar: a window that cannot hold the text
	 * cursor gives the soft keyboard nothing to type into. That is also why it
	 * sits up here -- the keyboard owns the bottom half of the screen, so a
	 * field down there would be typed into blind. */
	private fun openSearch() {
		if (popup == null) show() // filters need the entries the bar loads
		if (entries.isEmpty()) reload()
		val font = AppFont.get(activity)

		searchInput = EditText(activity).apply {
			hint = activity.getString(R.string.palette_search_hint)
			textSize = 15f
			typeface = font
			setTextColor(AppTheme.current.textPrimary)
			setHintTextColor(AppTheme.current.textSecondary)
			background = GradientDrawable().apply {
				cornerRadius = dp(14).toFloat()
				setColor(AppTheme.current.surfaceInset)
				setStroke(dp(1), AppTheme.edge(12))
			}
			setPadding(dp(14), dp(10), dp(14), dp(10))
			isSingleLine = true
			imeOptions = android.view.inputmethod.EditorInfo.IME_ACTION_SEARCH
			setText(query)
			setSelection(query.length)
			addTextChangedListener(object : android.text.TextWatcher {
				override fun afterTextChanged(s: android.text.Editable?) {
					query = s?.toString().orEmpty()
					refreshSearch()
				}
				override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
				override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
			})
		}
		val closeButton = TextView(activity).apply {
			text = "✕"
			textSize = 15f
			gravity = Gravity.CENTER
			setTypeface(font, Typeface.BOLD)
			setTextColor(AppTheme.current.textPrimary)
			background = chipBg(false)
			setPadding(dp(13), dp(10), dp(13), dp(10))
			layoutParams = LinearLayout.LayoutParams(
				ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT
			).apply { setMargins(dp(8), 0, 0, 0) }
			// Clears the filters when there is something to clear, closes the
			// window when there is not -- one button, no dead taps.
			setOnClickListener { if (filtersActive()) clearFilters() else closeSearch() }
		}
		brandRow = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
		// Two rows of tiles, scrolling sideways: same tile as the palette, twice
		// the hit surface per screenful.
		searchGrid = RecyclerView(activity).apply {
			layoutManager = androidx.recyclerview.widget.GridLayoutManager(
				activity, 2, androidx.recyclerview.widget.GridLayoutManager.HORIZONTAL, false)
			adapter = searchAdapter
			setBackgroundColor(Color.TRANSPARENT)
			setPadding(dp(6), dp(6), dp(6), dp(6))
			clipToPadding = false
			// Same staggered entrance the category strip uses, so results
			// arriving as you type read as one family with the rest of the UI.
			layoutAnimation = android.view.animation.LayoutAnimationController(
				android.view.animation.AnimationSet(true).apply {
					addAnimation(android.view.animation.AlphaAnimation(0f, 1f))
					addAnimation(android.view.animation.ScaleAnimation(
						0.88f, 1f, 0.88f, 1f,
						android.view.animation.Animation.RELATIVE_TO_SELF, 0.5f,
						android.view.animation.Animation.RELATIVE_TO_SELF, 0.5f))
					duration = 150
					interpolator = android.view.animation.DecelerateInterpolator()
				}, 0.03f)
			layoutParams = LinearLayout.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT, dp(292))
		}
		emptyLabel = TextView(activity).apply {
			text = activity.getString(R.string.palette_no_matches)
			textSize = 13f
			typeface = font
			gravity = Gravity.CENTER
			setTextColor(AppTheme.current.textSecondary)
			visibility = View.GONE
			// Matches the grid's height so the window does not jump while typing.
			layoutParams = LinearLayout.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT, dp(292))
		}
		val card = LinearLayout(activity).apply {
			orientation = LinearLayout.VERTICAL
			background = GradientDrawable().apply {
				cornerRadius = dp(20).toFloat()
				setColor(AppTheme.withAlpha(AppTheme.current.surface, 92))
				setStroke(dp(1), AppTheme.edge(15))
			}
			clipToOutline = true
			setPadding(dp(10), dp(10), dp(10), dp(6))
			addView(LinearLayout(activity).apply {
				orientation = LinearLayout.HORIZONTAL
				addView(searchInput, LinearLayout.LayoutParams(0,
					ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
				addView(closeButton)
			})
			addView(HorizontalScrollView(activity).apply {
				isHorizontalScrollBarEnabled = false
				addView(brandRow)
				layoutParams = LinearLayout.LayoutParams(
					ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
				).apply { topMargin = dp(8) }
			})
			addView(searchGrid)
			addView(emptyLabel)
		}
		val holder = FrameLayout(activity).apply {
			setPadding(dp(8), 0, dp(8), 0)
			addView(card)
		}
		val p = PopupWindow(holder,
			ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
		p.isFocusable = true
		p.setOnDismissListener {
			searchPopup = null
			syncSearchButton()
		}
		searchPopup = p
		buildBrandChips()
		refreshSearch()
		syncSearchButton()

		val decor = activity.window.decorView
		val top = ViewCompat.getRootWindowInsets(decor)
			?.getInsets(WindowInsetsCompat.Type.systemBars())?.top ?: 0
		runCatching {
			p.showAtLocation(decor, Gravity.TOP or Gravity.CENTER_HORIZONTAL, 0, top + dp(8))
			card.alpha = 0f
			card.translationY = dp(-24).toFloat()
			card.animate().alpha(1f).translationY(0f).setDuration(200L)
				.setInterpolator(android.view.animation.DecelerateInterpolator()).start()
			searchInput.requestFocus()
			searchInput.post {
				runCatching {
					(activity.getSystemService(android.content.Context.INPUT_METHOD_SERVICE)
						as android.view.inputmethod.InputMethodManager)
						.showSoftInput(searchInput, android.view.inputmethod.InputMethodManager.SHOW_IMPLICIT)
				}
			}
		}
	}

	private fun closeSearch() {
		if (::searchInput.isInitialized) runCatching {
			(activity.getSystemService(android.content.Context.INPUT_METHOD_SERVICE)
				as android.view.inputmethod.InputMethodManager)
				.hideSoftInputFromWindow(searchInput.windowToken, 0)
		}
		searchPopup?.dismiss()
		searchPopup = null
		syncSearchButton()
	}

	private fun syncSearchButton() {
		searchButton?.let {
			val on = searchPopup != null || filtersActive()
			it.background = chipBg(on)
			it.setTextColor(if (on) AppTheme.current.onAccent else AppTheme.current.textPrimary)
		}
	}

	private fun clearFilters() {
		query = ""
		brandFilter = null
		if (::searchInput.isInitialized) searchInput.setText("")
		buildBrandChips()
		refreshSearch()
		syncSearchButton()
	}

	/** Brand chips, derived from whatever is actually installed -- a side-loaded
	 * pack must show up here without any hard-coded list to update. */
	private fun buildBrandChips() {
		if (!::brandRow.isInitialized) return
		if (entries.isEmpty()) return
		brandRow.removeAllViews()
		val font = AppFont.get(activity)
		val brands = entries.map { it.brand }.filter { it.isNotBlank() }.distinct()
			.sortedWith(compareBy({ it != "VCV" }, { it }))
		fun brandChip(label: String, value: String?) = TextView(activity).apply {
			text = label
			textSize = 11f
			setTypeface(font, Typeface.BOLD)
			val on = brandFilter == value
			isSelected = on
			background = chipBg(on)
			setTextColor(if (on) AppTheme.current.onAccent else AppTheme.current.textPrimary)
			setPadding(dp(12), dp(6), dp(12), dp(6))
			layoutParams = LinearLayout.LayoutParams(
				ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT
			).apply { setMargins(dp(3), dp(2), dp(3), dp(2)) }
			setOnClickListener {
				brandFilter = if (brandFilter == value) null else value
				buildBrandChips()
				refreshSearch()
				syncSearchButton()
			}
		}
		brandRow.addView(brandChip(activity.getString(R.string.palette_all_brands), null))
		for (b in brands) brandRow.addView(brandChip(b, b))
	}

	private fun loadEntries(): Boolean {
		val json = getModelsJson()
		if (json.length < 4)
			return false
		val arr = try { JSONArray(json) } catch (_: Exception) { JSONArray() }
		val list = ArrayList<Entry>(arr.length())
		for (i in 0 until arr.length()) {
			val o = arr.getJSONObject(i)
			val tagsArr = o.getJSONArray("tags")
			val tags = ArrayList<String>(tagsArr.length())
			for (j in 0 until tagsArr.length()) tags.add(tagsArr.getString(j))
			list.add(Entry(o.getString("key"), o.getString("name"), o.optString("brand"),
				tags, o.optString("description"), o.optString("plugin")))
		}
		entries = list
		return true
	}

	/** Info popup for a module (the ⓘ badge on each tile): what it is and what
	 * it does, from the model's own metadata (name, brand, tags, description). */
	private fun showInfo(e: Entry) {
		val ctx = activity
		val font = AppFont.get(ctx)
		val col = LinearLayout(ctx).apply {
			orientation = LinearLayout.VERTICAL
			setPadding(dp(22), dp(20), dp(22), dp(20))
		}
		col.addView(TextView(ctx).apply {
			text = e.name
			setTextColor(AppTheme.current.accent)
			textSize = 19f
			setTypeface(font, Typeface.BOLD)
		})
		val sub = listOf(e.brand, e.plugin).filter { it.isNotBlank() }.distinct().joinToString(" • ")
		if (sub.isNotEmpty()) col.addView(TextView(ctx).apply {
			text = sub
			setTextColor(AppTheme.current.textSecondary)
			textSize = 13f
			setPadding(0, dp(3), 0, 0)
		})
		if (e.tags.isNotEmpty()) col.addView(TextView(ctx).apply {
			text = e.tags.joinToString("   ") { "#$it" }
			setTextColor(Color.parseColor("#8FB98F"))
			textSize = 12f
			setPadding(0, dp(12), 0, 0)
		})
		col.addView(TextView(ctx).apply {
			text = e.description.ifBlank { ctx.getString(R.string.module_no_description) }
			setTextColor(AppTheme.current.textPrimary)
			textSize = 15f
			setPadding(0, dp(14), 0, 0)
			setLineSpacing(dp(3).toFloat(), 1f)
		})
		val dlg = android.app.AlertDialog.Builder(ctx).create()
		dlg.setView(ScrollView(ctx).apply { addView(col); isVerticalScrollBarEnabled = false })
		dlg.setButton(android.content.DialogInterface.BUTTON_POSITIVE,
			ctx.getString(android.R.string.ok)) { d, _ -> d.dismiss() }
		dlg.window?.setBackgroundDrawable(GradientDrawable().apply {
			cornerRadius = dp(22).toFloat()
			setColor(AppTheme.withAlpha(AppTheme.current.surface, 95))
			setStroke(dp(1), AppTheme.edge(18))
		})
		(ctx as? MainActivity)?.let { runCatching { it.trackTopWindow(dlg); it.glassify(dlg.window) } }
		dlg.show()
	}

	// ---- drag & drop ----

	/** True when a finger at `rawY` is clear of the window the drag started in,
	 * i.e. over the rack. The bottom bar is below the rack, the search window
	 * above it, so the two look opposite ways. Shared by the ghost's live
	 * feedback and the drop itself, so what you see is what you get. */
	private fun isOverRack(rawY: Float, inSearch: Boolean, fallback: View): Boolean {
		val owner = (if (inSearch) searchPopup else popup)?.contentView ?: fallback
		val loc = IntArray(2)
		owner.getLocationOnScreen(loc)
		return if (inSearch) rawY > loc[1] + owner.height else rawY < loc[1]
	}

	private fun showGhost(key: String, atX: Float, atY: Float, valid: Boolean) {
		val bmp = ThumbnailCache.get(activity.filesDir, key, dp(120)) ?: return
		ghostH = dp(120)
		ghostW = if (bmp.height > 0) ghostH * bmp.width / bmp.height else dp(40)
		val iv = ImageView(activity).apply {
			setImageBitmap(bmp)
			scaleType = ImageView.ScaleType.FIT_CENTER
			background = GradientDrawable().apply {
				cornerRadius = dp(6).toFloat()
				setStroke(dp(2), AppTheme.withAlpha(AppTheme.current.accent, 40))
			}
		}
		val g = PopupWindow(iv, ghostW, ghostH)
		g.isTouchable = false
		ghost = g
		ghostView = iv
		ghostValid = valid
		ghostLastX = atX
		ghostLastY = atY
		runCatching {
			// Ghost CENTERED on the finger, so what you see is where it lands.
			g.showAtLocation(activity.window.decorView, Gravity.TOP or Gravity.START,
				atX.toInt() - ghostW / 2, atY.toInt() - ghostH / 2)
			iv.alpha = if (valid) 0.92f else 0.4f
			iv.scaleX = 0.6f; iv.scaleY = 0.6f
			iv.animate().scaleX(1f).scaleY(1f).setDuration(140L)
				.setInterpolator(android.view.animation.OvershootInterpolator()).start()
		}
	}

	private fun moveGhost(x: Float, y: Float, valid: Boolean, view: View) {
		// Position tracks the finger 1:1 -- no smoothing. Interpolating a
		// dragged object's position reads as lag, not as smoothness.
		ghost?.update(x.toInt() - ghostW / 2, y.toInt() - ghostH / 2, -1, -1)
		val iv = ghostView ?: return
		// Tilt with horizontal speed, like something being carried. Small and
		// clamped: this is weight, not a flourish.
		val tilt = ((x - ghostLastX) * 0.5f).coerceIn(-9f, 9f)
		ghostLastX = x
		ghostLastY = y
		iv.rotation = iv.rotation * 0.7f + tilt * 0.3f
		if (valid == ghostValid)
			return
		// Crossing the drop boundary is the one thing the user cannot guess:
		// say it with opacity AND a tick, before they commit by lifting.
		ghostValid = valid
		iv.animate().alpha(if (valid) 0.92f else 0.4f)
			.scaleX(if (valid) 1f else 0.9f).scaleY(if (valid) 1f else 0.9f)
			.setDuration(110L).start()
		runCatching {
			view.performHapticFeedback(android.view.HapticFeedbackConstants.CLOCK_TICK)
		}
	}

	/** Ends the drag with an outcome the user can read: a placed module shrinks
	 * into the rack, a refused one falls back to where it came from. Silently
	 * dismissing the ghost either way left "did that work?" unanswered. */
	private fun dropGhost(placed: Boolean, originX: Float, originY: Float) {
		val g = ghost ?: return
		val iv = ghostView
		ghost = null
		ghostView = null
		if (iv == null) {
			runCatching { g.dismiss() }
			return
		}
		if (placed) {
			iv.animate().alpha(0f).scaleX(0.65f).scaleY(0.65f).rotation(0f)
				.setDuration(130L)
				.withEndAction { runCatching { g.dismiss() } }.start()
			return
		}
		// Refused: slide the window back to the tile, then fade.
		val startX = ghostLastX - ghostW / 2f
		val startY = ghostLastY - ghostH / 2f
		val anim = android.animation.ValueAnimator.ofFloat(0f, 1f).setDuration(180L)
		anim.interpolator = android.view.animation.DecelerateInterpolator()
		anim.addUpdateListener { a ->
			val t = a.animatedValue as Float
			runCatching {
				g.update((startX + (originX - ghostW / 2f - startX) * t).toInt(),
					(startY + (originY - ghostH / 2f - startY) * t).toInt(), -1, -1)
			}
		}
		iv.animate().alpha(0f).rotation(0f).setDuration(180L).start()
		anim.addListener(object : android.animation.AnimatorListenerAdapter() {
			override fun onAnimationEnd(a: android.animation.Animator) { runCatching { g.dismiss() } }
		})
		anim.start()
	}

	/** @param inSearch tiles living in the top search window rather than the
	 * bottom bar. Only the drop test differs: the rack is BELOW that window and
	 * ABOVE this one. */
	private inner class TileAdapter(private val inSearch: Boolean = false)
			: RecyclerView.Adapter<TileAdapter.Holder>() {
		private var items: List<Entry> = emptyList()

		fun submit(list: List<Entry>) {
			items = list
			notifyDataSetChanged()
		}

		inner class Holder(val root: LinearLayout, val image: ImageView,
			val name: TextView, val info: TextView)
			: RecyclerView.ViewHolder(root)

		override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): Holder {
			// The thumbnail fills a FIXED-height box (MATCH_PARENT into a
			// dp(110)-tall frame) so every tile is exactly the same height and
			// the row stays perfectly aligned -- the badge, being an overlay,
			// can never push the box taller (that skewed the strip before).
			val image = ImageView(activity).apply {
				importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
				adjustViewBounds = true
				scaleType = ImageView.ScaleType.FIT_CENTER
				layoutParams = FrameLayout.LayoutParams(
					ViewGroup.LayoutParams.WRAP_CONTENT,
					ViewGroup.LayoutParams.MATCH_PARENT)
			}
			// ⓘ info badge, top-right corner of the thumbnail.
			val info = TextView(activity).apply {
				text = "ⓘ"
				textSize = 12f
				gravity = Gravity.CENTER
				setTextColor(AppTheme.current.onAccent)
				background = GradientDrawable().apply {
					shape = GradientDrawable.OVAL
					setColor(AppTheme.withAlpha(AppTheme.current.accent, 90))
					setStroke(dp(1), AppTheme.withAlpha(Color.BLACK, 40))
				}
				layoutParams = FrameLayout.LayoutParams(pDp(22), pDp(22)).apply {
					gravity = Gravity.TOP or Gravity.END
					topMargin = dp(3); rightMargin = dp(3)
				}
			}
			val imageWrap = FrameLayout(activity).apply {
				// Fixed height + a minimum width keep tiles uniform even when a
				// module has no thumbnail (its image collapses to zero width).
				minimumWidth = pDp(56)
				layoutParams = LinearLayout.LayoutParams(
					ViewGroup.LayoutParams.WRAP_CONTENT, pDp(110))
				addView(image)
				addView(info)
			}
			// The label must NOT widen the tile: a long name (kept at up to
			// dp(96) before) made the tile wider than its thumbnail, and the
			// centered thumbnail + corner badge then looked off-centre. Fill
			// the thumbnail's width instead and ellipsize anything longer.
			val name = TextView(activity).apply {
				importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
				textSize = 10f
				typeface = AppFont.get(activity)
				setTextColor(AppTheme.current.textPrimary)
				gravity = Gravity.CENTER_HORIZONTAL
				// Wrap onto a second line rather than truncating; still bound to
				// the thumbnail's width so it can't widen (and off-centre) the
				// tile. Only a name longer than two lines gets an ellipsis.
				maxLines = 2
				ellipsize = android.text.TextUtils.TruncateAt.END
				setLineSpacing(0f, 0.95f)
				layoutParams = LinearLayout.LayoutParams(
					ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
			}
			val root = LinearLayout(activity).apply {
				orientation = LinearLayout.VERTICAL
				gravity = Gravity.CENTER_HORIZONTAL
				setPadding(dp(6), dp(2), dp(6), dp(4))
				addView(imageWrap)
				addView(name)
			}
			return Holder(root, image, name, info)
		}

		override fun onBindViewHolder(holder: Holder, position: Int) {
			val e = items[position]
			holder.image.setImageBitmap(ThumbnailCache.get(activity.filesDir, e.key, pDp(110)))
			holder.name.text = e.name
			holder.root.contentDescription = activity.getString(
				R.string.palette_module_description, e.name, e.brand)
			holder.root.isFocusable = true
			holder.info.contentDescription = activity.getString(R.string.palette_module_info, e.name)
			holder.info.isFocusable = true
			holder.info.setOnClickListener {
				it.animate().scaleX(1.3f).scaleY(1.3f).setDuration(90L)
					.withEndAction { it.animate().scaleX(1f).scaleY(1f).setDuration(120L).start() }.start()
				showInfo(e)
			}
			var dragging = false
			// A drag that showed the ghost must swallow the click that Android
			// still delivers on release: otherwise a refused drop places the
			// module at screen centre anyway, contradicting the ghost we just
			// animated back to the tile.
			var dragged = false
			holder.root.setOnLongClickListener { v ->
				dragging = true
				v.parent?.requestDisallowInterceptTouchEvent(true)
				v.performHapticFeedback(android.view.HapticFeedbackConstants.LONG_PRESS)
				false // keep receiving the touch stream below
			}
			holder.root.setOnTouchListener { v, ev ->
				when (ev.actionMasked) {
					// Every new gesture starts clean: leaving this set from a
					// previous drag would swallow the NEXT tap (the click never
					// fires after a long drag, so it cannot self-clear).
					MotionEvent.ACTION_DOWN -> dragged = false
					MotionEvent.ACTION_MOVE -> if (dragging) {
						val valid = isOverRack(ev.rawY, inSearch, v)
						if (ghost == null) {
							dragged = true
							// Remember where the tile sits: a refused drop flies back
							// to it rather than vanishing.
							val loc = IntArray(2)
							v.getLocationOnScreen(loc)
							dragOriginX = loc[0] + v.width / 2f
							dragOriginY = loc[1] + v.height / 2f
							showGhost(e.key, ev.rawX, ev.rawY, valid)
						}
						else
							moveGhost(ev.rawX, ev.rawY, valid, v)
					}
					MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
						if (dragging) {
							dragging = false
							val placed = ev.actionMasked == MotionEvent.ACTION_UP &&
								isOverRack(ev.rawY, inSearch, v)
							// Module top-left lands at the ghost's top-left (aim point).
							if (placed) {
								chooseAt(e.key, ev.rawX - ghostW / 2f, ev.rawY - ghostH / 2f)
								runCatching {
									v.performHapticFeedback(
										android.view.HapticFeedbackConstants.CONTEXT_CLICK)
								}
								if (inSearch) closeSearch()
							}
							dropGhost(placed, dragOriginX, dragOriginY)
						}
					}
				}
				false // let clicks and RecyclerView scrolling work normally
			}
			holder.root.setOnClickListener {
				if (dragged) return@setOnClickListener
				it.animate().scaleX(0.88f).scaleY(0.88f).setDuration(90L)
					.withEndAction { it.animate().scaleX(1f).scaleY(1f).setDuration(140L)
						.setInterpolator(android.view.animation.OvershootInterpolator()).start() }
					.start()
				// Quick tap: place at the center of the screen.
				val dm = activity.resources.displayMetrics
				chooseAt(e.key, dm.widthPixels / 2f, dm.heightPixels / 2.4f)
				// Searching is a "find this one thing" flow: once it is placed,
				// get the window and the keyboard out of the way so the user can
				// actually see what landed. The bottom bar keeps its strip open.
				if (inSearch) closeSearch()
			}
		}

		override fun getItemCount() = items.size
	}
}
