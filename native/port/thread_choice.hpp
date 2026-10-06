/* Which thread count a patch is played on, from what each count measured.
 *
 * The decision only: checkThreadCount() in main_android.cpp does the measuring
 * (each count played in silence for a moment, read as a share of the audio
 * deadline) and asks these two questions between readings. Nothing here knows
 * about Rack, Android or time, so it runs on a desk:
 * thread_choice_test.cpp holds the readings real devices gave and what each
 * should come to, and its first line says how to run it. Change a number here
 * and run it -- three builds in a row each broke the one before on a phone
 * before this file existed.
 *
 * loads[c] is the reading for c threads in percent of the deadline, 0 for a
 * count not measured; the counts considered are floor..top.
 */
#pragma once

#include <cstdint>
#include <initializer_list>


namespace rackdroid {


struct ThreadChoice {
	/** A count is comfortable under this share of its deadline, and the
	fewest comfortable threads are what is wanted -- not the lowest load,
	which more Workers can always buy a few points of.
	Eighty, not sixty: this is measured with nobody touching the screen,
	and what a count costs once they do grows with the Workers the callback
	has to wait for. A Nothing A024 read 1:72% 2:94% 3:65% 4:61% 5:51%,
	took five as the only one under sixty, and a pinch then held callbacks
	of 2 ms of work for 5 to 15 ms -- 400 underruns in a window -- where
	the build before, on two threads at 84%, had got through the same
	zooming with four short bursts. Kept under the 85 that counts as close
	to the limit in checkThreadCount(), or the choice would be argued with
	at once. */
	static constexpr int32_t COMFORT_PERCENT = 80;
	/** Of two counts this close, the smaller. One point must not decide it:
	4:81% 5:79% on an SM-S901E took five. */
	static constexpr int32_t NEAR_POINTS = 5;
	/** A count this much worse than the lightest is the far side of the
	curve's one bottom, and nothing past it is measured: seven Workers on a
	tablet that is happiest with four measured 872% of the deadline for the
	half second it took to find that out. */
	static constexpr int32_t FAR_SIDE_POINTS = 25;

	/** The fewest comfortable count, or 0 where none is. */
	static int comfortable(const int32_t* loads, int floor, int top) {
		int comfy = 0;
		for (int c = floor; c <= top && comfy == 0; c++)
			if (loads[c] > 0 && loads[c] < COMFORT_PERCENT)
				comfy = c;
		for (int c = floor; c < comfy; c++)
			if (loads[c] > 0 && loads[c] <= loads[comfy] + NEAR_POINTS)
				return c;
		return comfy;
	}

	/** The count to measure next, or -1 when enough is known.
	`walking` is false only for a patch whose first reading was light and has
	had no other: that one is not walked through the ladder at all. */
	static int next(const int32_t* loads, int floor, int top, bool walking) {
		int comfy = comfortable(loads, floor, top);
		if (comfy > 0) {
			// Something fits: then the question is how few threads will do,
			// asked from the bottom -- one first, which is where a light patch
			// belongs and is then settled in two readings instead of a walk
			// down through every count above it (six seconds of silence for
			// a 45-module patch coming after a heavy one, on an SM-S901E).
			for (int c = floor; c < comfy; c++)
				if (loads[c] == 0)
					return c;
			return -1;
		}
		if (!walking)
			return -1;
		// Outwards from the lightest count found so far, nearest first.
		int low = 0;
		for (int c = floor; c <= top; c++)
			if (loads[c] > 0 && (low == 0 || loads[c] < loads[low]))
				low = c;
		for (int dir : {-1, 1}) {
			for (int c = low + dir; c >= floor && c <= top; c += dir) {
				if (loads[c] == 0)
					return c;
				if (loads[c] > loads[low] + FAR_SIDE_POINTS)
					break;
			}
		}
		return -1;
	}

	/** The count to play on: the fewest the patch is comfortable on; where it
	is comfortable on none, the lightest, and of two within a few points the
	smaller. 0 where nothing was measured at all. */
	static int best(const int32_t* loads, int floor, int top) {
		int best = comfortable(loads, floor, top);
		if (best > 0)
			return best;
		for (int c = floor; c <= top; c++)
			if (loads[c] > 0 && (best == 0 || loads[c] < loads[best]))
				best = c;
		for (int c = floor; c < best; c++)
			if (loads[c] > 0 && loads[c] <= loads[best] + NEAR_POINTS)
				return c;
		return best;
	}
};


} // namespace rackdroid
