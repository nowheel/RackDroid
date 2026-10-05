/* What the host UI harness needs from touch_input.cpp, which is Android-only:
 * window_android.cpp asks whether a pinch is in progress, and on a host there
 * are no fingers. Without this the harness had not linked since the pinch
 * freeze went in, so scripts/host_smoke.sh was failing before it ran a test. */
#include "../port/touch_input.hpp"

namespace rackdroid {

double touchSecondsSincePinch() {
	return 1e9;
}

} // namespace rackdroid
