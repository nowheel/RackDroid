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

	/** A window with this many callbacks held up waiting for a Worker (see
	audioWaitedStalls()), this many windows running, and the count is one
	Worker too many -- if one fewer was measured to fit with room. The load
	does not show it: the callback is asleep, not working. Under 90, not under
	100: a count measured at 97% is no place to send anyone on a hint. */
	static constexpr int32_t STALLS_PER_WINDOW = 3;
	static constexpr int32_t STALL_WINDOWS = 2;
	static constexpr int32_t FEWER_FITS_PERCENT = 90;

	/** The count to step down to when the callback keeps waiting for its
	Workers, or 0 to stay. */
	static int fewerForStalls(const int32_t* loads, int floor, int current, int stallWindows) {
		int lower = current - 1;
		if (stallWindows < STALL_WINDOWS || lower < floor || lower < 1)
			return 0;
		return (loads[lower] > 0 && loads[lower] < FEWER_FITS_PERCENT) ? lower : 0;
	}

	/** One thread is comfortable only under this. With no Workers the callback
	does all the work on its own core and never sleeps, the governor holds
	that core at a speed where the reading looks fine, and the kernel stalls
	a thread that fills a core: a Nothing A024 read 1:71% in four slices
	running, was started on one thread, and played at 109% with a 100 ms hole
	every 1.35 s -- 450 underruns a second -- as it had each of the three
	times before that one thread read between 68 and 77. With Workers the
	callback spends most of each block waiting, and 65% on three was clean. */
	static constexpr int32_t SOLO_COMFORT_PERCENT = 60;
	/** And two threads only under this. The fewer the threads, the more of
	the work is the callback's own and the less of each block it spends
	asleep waiting: the same phone read 1:93% 2:81% 3:76%, was started on two
	as "within five points of three", ran at 78% for eight seconds and then at
	106% with the 96 ms holes, 450 underruns a second, until it was moved to
	three -- where it ran clean at 81%. Seventy-five, not seventy: a Lenovo
	TB-X306X is right on two threads at 70%. */
	static constexpr int32_t PAIR_COMFORT_PERCENT = 75;

	static int32_t comfortOf(int c) {
		return c == 1 ? SOLO_COMFORT_PERCENT : c == 2 ? PAIR_COMFORT_PERCENT : COMFORT_PERCENT;
	}

	static bool fits(const int32_t* loads, int c) {
		return loads[c] > 0 && loads[c] < comfortOf(c);
	}

	/** The fewest comfortable count, or 0 where none is. */
	static int comfortable(const int32_t* loads, int floor, int top) {
		int comfy = 0;
		for (int c = floor; c <= top && comfy == 0; c++)
			if (fits(loads, c))
				comfy = c;
		// A lower count within a few points wins, and may be those few points
		// over the line for three threads and up (4:82% against 5:79% is four)
		// -- but one thread or two only where comfortable by their own,
		// stricter measure.
		for (int c = floor; c < comfy; c++)
			if (loads[c] > 0 && loads[c] <= loads[comfy] + NEAR_POINTS
					&& loads[c] < comfortOf(c) + (c > 2 ? NEAR_POINTS : 0))
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

	/** The same two questions for a patch the engine is stepping as islands
	(engine_islands.inc), where they have other answers.

	Everything above is about Workers the callback has to wait for twice a
	sample: fewest is safest there. With islands it waits for none of them --
	a thread takes an island when it is free, and one that is not running
	holds nobody up -- so a thread more costs nothing and buys room for the
	moments the system takes cores away. A Galaxy A52s played a patch of six
	parts on two threads at 64% "as the fewest that fit" when four read half
	that; a Nothing A024 with twelve parts read 3:94% 4:81%.

	So: the lightest count, the smaller of two within a few points. And the
	walk goes up first, from the lightest so far to the next count that can
	matter (usefulForIslands): past the fast cores the curve only rises
	(5:119% on that Nothing, 5:165% on the A52s), and every count measured is
	a second of silence. */
	static int bestIslands(const int32_t* loads, int floor, int top) {
		int low = 0;
		for (int c = floor; c <= top; c++)
			if (loads[c] > 0 && (low == 0 || loads[c] < loads[low]))
				low = c;
		for (int c = floor; c < low; c++)
			if (loads[c] > 0 && loads[c] <= loads[low] + NEAR_POINTS)
				return c;
		return low;
	}

	/** Whether `count` threads can finish `parts` islands sooner than one
	thread fewer: the block ends when the busiest thread does, and that
	thread has ceil(parts / count) of them. Four parts gain nothing from a
	third thread (a TB-X306X read 2:110% 3:110% 4:65%), six nothing from a
	fourth or fifth (a Galaxy A52s: 3:50% 4:49% 5:50%).
	ponytail: as if the parts were all one size; a patch with one large part
	and several small is measured at counts that cannot help it. */
	static bool usefulForIslands(int count, int parts) {
		if (count <= 1)
			return true;
		return (parts + count - 1) / count < (parts + count - 2) / (count - 1);
	}

	static int nextIslands(const int32_t* loads, int floor, int top, bool walking, int parts) {
		if (!walking)
			return -1;
		int low = 0;
		for (int c = floor; c <= top; c++)
			if (loads[c] > 0 && (low == 0 || loads[c] < loads[low]))
				low = c;
		if (low == 0)
			return -1;
		// The nearest count each way that can make a difference, upward
		// first. One that has been measured and is not the lightest ends
		// that direction.
		for (int dir : {1, -1}) {
			int c = low + dir;
			while (c >= floor && c <= top && !usefulForIslands(c, parts))
				c += dir;
			if (c >= floor && c <= top && loads[c] == 0)
				return c;
		}
		return -1;
	}
};


} // namespace rackdroid
