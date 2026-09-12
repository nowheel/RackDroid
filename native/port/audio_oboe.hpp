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

/** The live Oboe device's current block size (frames per callback), or 0 if
no device is open yet. Render-thread only, same contract as the engine
itself (see CLAUDE.md). */
int audioBlockSize();

/** Changes the live Oboe device's block size, same call the Audio module's
own widget makes when the user picks one by hand -- closes and reopens the
stream, so this is NOT free: it is an audible gap, not a silent tweak, and
exists for checkBlockSizeOverload() in main_android.cpp to reach for only
once, after thread count is already maxed out and still not enough. Returns
false (no-op) if no device is open. */
bool audioSetBlockSize(int blockSize);

} // namespace rackdroid
