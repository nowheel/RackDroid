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

/** The same for a patch stepped as islands. */
static void expectIslands(const char* what, std::vector<int32_t> truth, int start, int want, int maxReadings = 99) {
	const int floor = 1, top = (int) truth.size();
	std::vector<int32_t> loads(top + 2, 0);
	int current = start, readings = 0, got = 0;
	for (;;) {
		loads[current] = truth[current - 1];
		readings++;
		bool walking = readings > 1 || loads[current] >= 60;
		int next = ThreadChoice::nextIslands(loads.data(), floor, top, walking);
		if (next < 0) {
			got = ThreadChoice::bestIslands(loads.data(), floor, top);
			break;
		}
		current = next;
	}
	bool ok = got == want && readings <= maxReadings;
	if (!ok)
		failures++;
	std::printf("%s  islands, %s: chose %d (wanted %d) after %d readings%s\n", ok ? "ok  " : "FAIL", what, got, want,
		readings, readings > maxReadings ? " -- too many" : "");
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
	// Three, not one: one thread at 72% is the reading that then played at
	// 109% (see SOLO_COMFORT_PERCENT), four times on this phone.
	expect("A024 heavy, the day five was chosen", {72, 94, 65, 61, 51, 80, 90}, 2, 3);
	expect("same, coming from five threads", {72, 94, 65, 61, 51, 80, 90}, 5, 3);
	expect("A024 0.1.2.87, 1:71% 3:65%", {71, 94, 65, 61, 51, 80, 90}, 3, 3);
	// Two threads at 81% are not "as good as three at 76%": see
	// PAIR_COMFORT_PERCENT.
	expect("A024 0.1.2.95, 2:81% 3:76%", {93, 81, 76, 70, 72, 80, 90}, 3, 3);
	expect("8T, 2:80% 3:78%", {103, 80, 78, 80, 85, 90, 95}, 1, 3);
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
	// Nothing A024, 0.1.2.82: 1:125% 2:97% 3:85% 4:78% took four threads, and
	// the callback then waited for a Worker three to eight times every five
	// seconds, at 61-68% of its deadline, for a minute before it underran.
	// Three ran clean. Down to three after two such windows; not on one, and
	// not from three to two, which measured 97%.
	{
		const int32_t loads[] = {0, 125, 97, 85, 78};
		bool ok = ThreadChoice::fewerForStalls(loads, 1, 4, 2) == 3
			&& ThreadChoice::fewerForStalls(loads, 1, 4, 1) == 0
			&& ThreadChoice::fewerForStalls(loads, 1, 3, 2) == 0
			&& ThreadChoice::fewerForStalls(loads, 1, 1, 5) == 0;
		if (!ok)
			failures++;
		std::printf("%s  A024 waiting for a fourth thread steps down to three\n", ok ? "ok  " : "FAIL");
	}
	// Patches stepped as islands (0.1.2.110-118). The lightest count, found
	// in few readings; never the far side.
	// Nothing A024, twelve parts: 1:197% 2:141% 3:94% 4:81% 5:119%.
	expectIslands("A024 x12 from three", {197, 141, 94, 81, 119, 150, 200}, 3, 4, 4);
	expectIslands("A024 x12 from four", {197, 141, 94, 81, 119, 150, 200}, 4, 4, 3);
	// Galaxy A52s: six parts 1:129% 2:68%, and four cores that are fast;
	// twelve parts 1:261% 2:133% 3:90% 4:84% 5:165%.
	expectIslands("A52s x6 from two", {129, 68, 47, 36, 80, 120, 160}, 2, 4, 4);
	expectIslands("A52s x12 from four", {261, 133, 90, 84, 165, 200, 250}, 4, 4, 3);
	// TB-X306X, four parts: 1:218% 2:110% 3:110% 4:65%, more threads than
	// parts never asked for (the caller caps the top at the parts).
	expectIslands("TB-X306X x4 from two", {218, 110, 110, 65}, 2, 4, 3);
	// Nothing A024, four parts: one thread at 47% is light and is left alone.
	expectIslands("A024 x4, light on one", {47, 42, 40, 41}, 1, 1, 1);
	std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
	return failures ? 1 : 0;
}
