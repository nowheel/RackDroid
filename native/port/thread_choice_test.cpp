/* Check for thread_choice.hpp, on the host:
 *   g++ -std=c++17 -Wall native/port/thread_choice_test.cpp -o /tmp/thread_choice_test && /tmp/thread_choice_test
 * Each case is a set of readings a real device gave (percent of the audio
 * deadline per thread count, from the logs) and the count it should be played
 * on. The measurement is walked the way checkThreadCount() walks it, so a case
 * also fails if the walk stops before it has seen the count that matters. */
#include "thread_choice.hpp"

#include <cstdio>
#include <vector>

using rackdroid::ThreadChoice;

static int failures = 0;

/** truth[c - 1] is what c threads would read. Returns the count chosen and
leaves in `readings` how many counts had to be measured to choose it. */
static int measure(const std::vector<int32_t>& truth, int start, int* readings) {
	const int floor = 1, top = (int) truth.size();
	std::vector<int32_t> loads(top + 2, 0);
	int current = start;
	*readings = 0;
	for (;;) {
		loads[current] = truth[current - 1];
		(*readings)++;
		// As in checkThreadCount(): a first reading under 60% is waved through.
		bool walking = *readings > 1 || loads[current] >= 60;
		int next = ThreadChoice::next(loads.data(), floor, top, walking);
		if (next < 0)
			return ThreadChoice::best(loads.data(), floor, top);
		current = next;
	}
}

static void expect(const char* what, std::vector<int32_t> truth, int start, int want, int maxReadings = 99) {
	int readings = 0;
	int got = measure(truth, start, &readings);
	bool ok = got == want && readings <= maxReadings;
	if (!ok)
		failures++;
	std::printf("%s  %s: chose %d (wanted %d) after %d readings%s\n", ok ? "ok  " : "FAIL", what, got, want,
		readings, readings > maxReadings ? " -- too many" : "");
}

int main() {
	// Nothing A024, 177 modules, 0.1.2.67: five threads at 51% underran 400
	// times in a window of pinching. One thread fits and waits for nobody.
	expect("A024 heavy, the day five was chosen", {72, 94, 65, 61, 51, 80, 90}, 2, 1);
	expect("same, coming from five threads", {72, 94, 65, 61, 51, 80, 90}, 5, 1);
	// The same patch on other days.
	expect("A024 heavy, 0.1.2.66", {110, 95, 125, 130, 140, 150, 160}, 1, 2);
	expect("A024 heavy, 0.1.2.69", {114, 100, 81, 68, 70, 75, 90}, 2, 4);
	// SM-S901E, 133 modules: one point between four and five.
	expect("S901E heavy, 4:82 5:79", {97, 105, 97, 82, 79, 87, 103}, 4, 4);
	expect("S901E heavy, 4:79 5:75", {95, 97, 87, 79, 75, 80, 90}, 4, 4);
	// The same phone throttled: nothing fits, the lightest it is.
	expect("S901E throttled", {152, 144, 119, 113, 100, 101, 108}, 4, 5);
	// Lenovo TB-X306X, 45 modules.
	expect("TB-X306X", {107, 70, 75, 80, 90, 120, 150}, 1, 2, 2);
	// OnePlus 8T at thermal status 3: flat, so the fewest.
	expect("8T throttled, flat curve", {89, 101, 90, 89, 92, 126, 140}, 1, 1);
	// A light patch is settled at once, and in two readings when the count
	// remembered from a heavier patch was high.
	expect("light patch on one thread", {21, 30, 35, 40, 45, 50, 55}, 1, 1, 1);
	expect("light patch, five remembered", {40, 35, 30, 28, 25, 30, 35}, 5, 1, 2);
	// A count far worse than the lightest ends the walk in that direction:
	// seven threads must never be played to find out they are hopeless.
	{
		int readings = 0;
		measure({300, 140, 95, 90, 130, 400, 872}, 4, &readings);
		bool ok = readings <= 5;
		if (!ok)
			failures++;
		std::printf("%s  the walk stops at the far side: %d readings of 7\n", ok ? "ok  " : "FAIL", readings);
	}
	std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
	return failures ? 1 : 0;
}
