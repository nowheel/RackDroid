#pragma once

namespace rackdroid {

/** Driver ID registered in rack::audio. Must not collide with RtAudio's
driver IDs (RtAudio::Api values, < 16) since it is stored in patch files. */
static const int OBOE_DRIVER_ID = 777;

/** Registers the Oboe driver with rack::audio. Call once after audio::init(). */
void oboeInit();

/** True once the stream has underrun while its buffer was already at the
ceiling the tuner can grow it to -- the signature of running out of CPU rather
than of a buffer being too small. Set by the audio thread, read by the render
thread, which is the only one that may touch the engine. Never cleared: it
records that this session hit the wall, not that it is at it right now. */
bool audioOverloaded();

} // namespace rackdroid
