#pragma once
#include <cstdint>

namespace rackdroid {

/** Driver ID registered in rack::audio. Must not collide with RtAudio's
driver IDs (RtAudio::Api values, < 16) since it is stored in patch files. */
static const int OBOE_DRIVER_ID = 777;

/** Registers the Oboe driver with rack::audio. Call once after audio::init(). */
void oboeInit();

/** Cumulative count of underruns that happened while the stream's buffer was
already at the ceiling the tuner can grow it to -- the signature of running
out of CPU rather than of a buffer being too small. Set by the audio thread,
read by the render thread, which is the only one that may touch the engine.
A live counter, not a latch: watching it stop moving for a while is how the
engine notices the load has eased again, symmetric to watching it move at
all to notice it got heavy (see checkEngineOverload/checkEngineUnderload in
main_android.cpp). */
int32_t audioCeilingUnderrunCount();

} // namespace rackdroid
