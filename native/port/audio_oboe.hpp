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

/** Percentage of the audio callback's deadline that producing its frames
actually used, since this was last called -- peak and mean. Reading resets
both, so there can only be one reader. 100 means a callback used every
microsecond it had.

The two are not interchangeable and the difference is load-bearing. The PEAK is
jitter: one callback preempted by the system, which is exactly what the buffer
exists to absorb, and at a 96-frame callback (a 2 ms deadline) a single
scheduling hiccup reads several hundred percent while the audio stays clean.
The MEAN is throughput: a mean above 100 means frames are not being produced as
fast as they are consumed, which no buffer depth can fix. Judge a configuration
on the mean; report the peak.

Unlike the underrun counters this is a continuous measurement, available 47
times a second rather than whenever something goes wrong, which is what lets
the thread search in main_android.cpp reject a count in a fraction of a second
instead of waiting five for an underrun that may or may not arrive. Written by
the audio thread, read by the render thread. Either pointer may be null. */
/** The core the audio callback last ran on (-1 before the first callback). */
int audioCallbackCpu();
/** Open the frequency file of a core for the callback to read (render
thread), and what the callback last read there, in kHz (0 = nothing yet). */
void audioOpenCoreFreq(int cpu);
int audioCallbackFreqKHz();
/** The rate the output stream runs at, which is what a callback's frames are
counted in -- not the engine's, which the user can set lower. 0 with no stream. */
float audioDeviceSampleRate();
/** The engine's sample rate, told by the render thread for the Java side to read. */
void audioNoteEngineRate(int rate);
void audioEngineLoadTake(int32_t* peak, int32_t* mean, int32_t* callbacks = nullptr,
	int32_t* clippedMean = nullptr);

/** Frames the last audio callback was actually asked to produce, or 0 before
the first one. NOT the engine's block size: alignToBurst() rounds the request to
a whole number of the device's bursts, so a 64-frame block arrives as a 96-frame
callback on a 96-burst phone. Anything deriving a DEADLINE must use this --
computing one from the block size understates it by the alignment ratio, which
is how ADPF came to be told 1.3 ms for a callback that had 2.0. */
int32_t audioCallbackFrames();

/** What callback size a given engine block size would actually produce on this
device, or 0 before the first stream has opened. Ask before spending a stream
reopen on a block-size change: where the device's burst is larger than the
block, every size at or below it collapses to the same single burst, so the
change is a reopen that buys nothing and reports a latency it did not achieve.
Seen on a Lenovo TB-X306X, whose burst is 960 frames. */
int32_t audioAlignedCallbackFrames(int blockSize);

/** Writes the callbacks that ran at least twice their deadline to the log,
with their CPU time, context switches and what the render thread was doing.
Frame loop only, for the same reason as audioReportUnderruns(). */
/** Returns how many it wrote to the log this time. */
int audioReportSlowCallbacks();

/** Writes any new underruns to the log. Call from the frame loop, never from
the audio callback: Rack's logger locks a mutex and fflush()es, which is the
last thing a late callback should be made to wait for. */
void audioReportUnderruns();

/** Logs the stream's real round-trip latency once per stream, a few seconds
after it opens (a timestamp is not available before then). Frame loop only. */
void audioReportLatency();

/** Walks the stream buffer back down while the stream is quiet. Oboe's tuner
only ever grows it, including for the underruns every patch load makes, and
never gives that latency back. Changing it does NOT reopen the stream, so this
costs no gap -- and if it goes too far the tuner grows it again by itself. */
void audioTrimBuffer();

/** How much delay the user is willing to trade for safety: 0 playing (lowest
delay, an occasional click forgiven), 1 balanced, 2 listening (safest). It sets
how low the automatic tuning may go, not a fixed size -- everything else keeps
working inside that band. The one audio decision here that is a preference
rather than a measurement, which is why it is the one that is asked. */
void audioSetLatencyMode(int mode);
int audioLatencyMode();

/** The audio callback thread's id, or 0 before the first callback has run.
Only that thread can read it, so it publishes it the first time round. */
int audioCallbackThreadTid();

/** Seconds since the output stream was last opened, or a large number if it
never has. The seconds right after a reopen underrun whatever the patch is
doing; they are not a measurement of anything. */
double audioSecondsSinceStreamOpen();

/** True while a master WAV recording is running. Reopening the stream costs
roughly half a second of callbacks, which in a recording is a hole in the file
with nothing to mark it -- so the block-size ladder waits for the recording to
finish rather than spending the user's take on headroom. */
bool audioIsRecording();

/** A block size already found not to hold on this install, or 0. Kept beside
the remembered block size so the step-down at startup does not spend a stream
reopen rediscovering the same answer on every launch. */
int audioKnownTooSmallBlock();

/** How many more launches that verdict is to be taken on trust, counting this
one; 0 means it has served its time and the size is worth trying again.

Calling this SPENDS one launch of the wait, so call it once per launch and only
where the answer is about to be acted on. The verdict expires because it used
to be permanent: one passing disturbance -- another app, a thermal moment --
condemned a block size for the life of the install, and every launch after it
opened at a higher latency than the device needed. A size that keeps failing is
re-asked at four launches, then eight, and so on to a cap. */
int audioTooSmallLaunchesLeft();

/** Records that this block size did not hold, and starts (or doubles) the wait
before it is tried again. */
void audioNoteBlockTooSmall(int bs);

/** Engine > Audio block: 0 when the app chooses the block size, otherwise the
size the user fixed. A fixed size is remembered across launches, is not moved
by a patch file's own value, and is never changed by the app. */
int audioBlockChoice();

/** Asks for a new choice from any thread; audioApplyBlockChoice() carries it
out. */
void audioRequestBlockChoice(int choice);

/** Render thread, once a frame: applies a requested choice. Reopens the stream
when the size changes. */
void audioApplyBlockChoice();

/** Seconds since the last underrun, or a large number if there has been none. */
double audioSecondsSinceUnderrun();

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
/** `ladderRaise`: the overload ladder buying headroom. Such a size is not
remembered for the next launch until stepping back down has been tried and has
failed (see g_raisedFrom in audio_oboe.cpp). */
bool audioSetBlockSize(int blockSize, bool ladderRaise = false);

/** Counts audio ports arriving. A change means a patch has been loaded. */
int audioPortEpoch();

/** Keeps the start-up silence on (up to its limit) while the caller is still
deciding how the patch should be run. */
void audioWarmupHold(bool hold);
void audioWarmupBegin();

/** True while another app has the audio focus and for five seconds after it
changes hands: underruns then are the system's, not the patch's. */
bool audioFocusDisturbed();

/** The largest block size worth asking the live stream for: the biggest power
of two that still fits inside its buffer capacity, capped at 1024 (the
largest size DEFAULT_BLOCK_SIZE's measurements actually cover). Bigger than
the capacity is not a latency-for-headroom trade but a guaranteed late
callback -- see the comment on the implementation for what that cost on real
hardware. 0 if no device is open. Render-thread only, same contract as the
engine itself (see CLAUDE.md). */
int audioMaxUsefulBlockSize();

/** Closes the audio device once nothing has been subscribed to it for a
couple of seconds. The driver deliberately does NOT close it the instant the
last port leaves: Rack rewrites a port's driver, device and channel count one
after another while a patch loads, and each rewrite is an unsubscribe followed
immediately by a subscribe. Closing in between cost three full stream
open/close cycles per startup, each blocking ~170 ms in stop(). Call once per
frame from the render thread.  */
void audioReleaseIdleDevice();

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
