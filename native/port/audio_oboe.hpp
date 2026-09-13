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

/** True if the audio stream underran in the last couple of seconds. Lets the
renderer notice it is crowding the audio callback and back off (see
Window::step in window_android.cpp); deliberately a short, self-clearing
window rather than a latch, so smoothness returns as soon as the sound does.
Safe from any thread. */
bool audioUnderrunsRecently();

/** Every underrun, including the ones the buffer tuner could still answer by
growing -- the count that matches what a listener actually hears, as opposed
to the narrower "the engine is out of CPU" signal above. Same threading
contract: written by the audio thread, read by the render thread. */
int32_t audioUnderrunCount();

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

/** The largest block size worth asking the live stream for: the biggest power
of two that still fits inside its buffer capacity, capped at 1024 (the
largest size DEFAULT_BLOCK_SIZE's measurements actually cover). Bigger than
the capacity is not a latency-for-headroom trade but a guaranteed late
callback -- see the comment on the implementation for what that cost on real
hardware. 0 if no device is open. Render-thread only, same contract as the
engine itself (see CLAUDE.md). */
int audioMaxUsefulBlockSize();

/** True if the live Oboe device's output stream was granted Shared sharing
mode instead of the Exclusive mode requested -- see the "asked for Exclusive
... got Shared" warning in openStreams() (audio_oboe.cpp) for the full story.
A Shared stream is mixed through AudioFlinger and needs materially more
headroom than the dedicated Exclusive/MMAP path checkBlockSizeOverload() in
main_android.cpp was originally sized for, which is what this exists to let
it detect. False (not true) if no device is open yet. Render-thread only,
same contract as the engine itself (see CLAUDE.md). */
bool audioIsSharedMode();

} // namespace rackdroid
