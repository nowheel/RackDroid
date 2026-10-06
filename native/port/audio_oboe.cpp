/* rack::audio driver backed by Google Oboe (AAudio/OpenSL under the hood).
 *
 * Replaces src/rtaudio.cpp on Android. One logical device ("Default") maps to
 * the system default output + input. The output stream is the clock: Rack's
 * audio ports are driven from its data callback, which is how the desktop
 * RtAudio driver behaves as well. Input is a second stream read non-blocking
 * inside the output callback (standard Oboe full-duplex pattern); if the input
 * stream can't be opened (e.g. RECORD_AUDIO permission not granted), the
 * device degrades to output-only.
 */
#include "audio_oboe.hpp"
#include "window_android.hpp"
#include "menu_native.hpp"

#include <vector>
#include <memory>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <iterator>
#include <thread>
#include <cstdio>
#include <cstring>
#include <cmath>

#include <jni.h>
#include <android/log.h>

#include <unistd.h>
#include <ctime>
#include <sched.h>
#include <sys/resource.h>

#include <oboe/Oboe.h>
#include <oboe/LatencyTuner.h>

/* Underruns are the one thing a user is asked to report, so the count has to
reach the file they can actually send (user/log.txt, via Rack's WARN) as well
as logcat, which only someone with a cable ever sees. Same both-sinks rule the
rest of the port layer follows. */
#define AUDIO_WARN(...) do { \
	__android_log_print(ANDROID_LOG_WARN, "rackdroid.audio", __VA_ARGS__); \
	WARN(__VA_ARGS__); \
} while (0)

#include <audio.hpp>
#include <settings.hpp>
#include <asset.hpp>
#include <system.hpp>
#include <context.hpp>
#include <engine/Engine.hpp>
#include <common.hpp>

#include "adpf.hpp"


// ---- Master WAV recorder ------------------------------------------------
// The audio callback taps the final output buffer into a single-producer/
// single-consumer ring; a writer thread drains it to a 16-bit PCM WAV.
// File I/O never happens on the audio thread; on ring overflow (writer
// stalled) frames are dropped rather than blocking the callback.

namespace {

struct WavRecorder {
	static const size_t RING_FLOATS = 1 << 20; // ~5.5s stereo @48k
	std::atomic<size_t> droppedFloats{0};
	std::vector<float> ring;
	// One cache line each. Adjacent, they share one, and every push from the
	// audio thread then invalidates the line the writer thread is reading --
	// false sharing, paid on the one thread in the process that must never
	// wait. Sixty-four bytes of padding is a cheap price for that.
	alignas(64) std::atomic<size_t> head{0}; // producer (audio thread)
	alignas(64) std::atomic<size_t> tail{0}; // consumer (writer thread)
	std::atomic<bool> active{false};
	std::thread writer;
	FILE* file = NULL;
	uint32_t dataBytes = 0;
	int sampleRate = 48000;
	int channels = 2;

	bool start(const std::string& path, int sr, int ch) {
		if (active)
			return false;
		file = std::fopen(path.c_str(), "wb");
		if (!file)
			return false;
		sampleRate = sr;
		channels = ch;
		dataBytes = 0;
		writeHeader(); // placeholder sizes, fixed on stop()
		ring.assign(RING_FLOATS, 0.f);
		head = tail = 0;
		active = true;
		writer = std::thread([this] { run(); });
		return true;
	}

	void stop() {
		if (!active)
			return;
		active = false;
		if (writer.joinable())
			writer.join();
		// Patch RIFF/data sizes now that the length is known.
		std::fseek(file, 4, SEEK_SET);
		uint32_t riff = 36 + dataBytes;
		std::fwrite(&riff, 4, 1, file);
		std::fseek(file, 40, SEEK_SET);
		std::fwrite(&dataBytes, 4, 1, file);
		std::fclose(file);
		file = NULL;
	}

	void writeHeader() {
		uint16_t fmt = 1, ch = channels, bits = 16;
		uint32_t sr = sampleRate;
		uint32_t byteRate = sr * ch * bits / 8;
		uint16_t blockAlign = ch * bits / 8;
		uint32_t zero = 0;
		std::fwrite("RIFF", 1, 4, file);
		std::fwrite(&zero, 4, 1, file);
		std::fwrite("WAVEfmt ", 1, 8, file);
		uint32_t fmtSize = 16;
		std::fwrite(&fmtSize, 4, 1, file);
		std::fwrite(&fmt, 2, 1, file);
		std::fwrite(&ch, 2, 1, file);
		std::fwrite(&sr, 4, 1, file);
		std::fwrite(&byteRate, 4, 1, file);
		std::fwrite(&blockAlign, 2, 1, file);
		std::fwrite(&bits, 2, 1, file);
		std::fwrite("data", 1, 4, file);
		std::fwrite(&zero, 4, 1, file);
	}

	/** Audio thread: push interleaved floats; drops on overflow. */
	void push(const float* samples, size_t n) {
		if (!active)
			return;
		size_t h = head.load(std::memory_order_relaxed);
		size_t t = tail.load(std::memory_order_acquire);
		size_t freeSpace = RING_FLOATS - (h - t);
		if (n > freeSpace) {
			droppedFloats.fetch_add(n - freeSpace, std::memory_order_relaxed);
			n = freeSpace;
		}
		for (size_t i = 0; i < n; i++)
			ring[(h + i) % RING_FLOATS] = samples[i];
		head.store(h + n, std::memory_order_release);
	}

	void run() {
		std::vector<int16_t> chunk;
		while (active || tail.load() != head.load()) {
			size_t h = head.load(std::memory_order_acquire);
			size_t t = tail.load(std::memory_order_relaxed);
			if (t == h) {
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
				continue;
			}
			size_t n = h - t;
			chunk.resize(n);
			for (size_t i = 0; i < n; i++) {
				float v = ring[(t + i) % RING_FLOATS];
				v = std::fmax(-1.f, std::fmin(1.f, v));
				chunk[i] = (int16_t) (v * 32767.f);
			}
			std::fwrite(chunk.data(), 2, n, file);
			dataBytes += n * 2;
			tail.store(t + n, std::memory_order_release);
			// The writer stalled for longer than the ring holds and audio was
			// left out of the file. Said here, never from the callback.
			size_t lost = droppedFloats.exchange(0, std::memory_order_relaxed);
			if (lost > 0)
				AUDIO_WARN("Recorder: the file writer fell behind; %.2f s of audio is missing "
					"from the recording", (double) lost / (channels > 0 ? channels : 2)
					/ (sampleRate > 0 ? sampleRate : 48000));
		}
	}
};

WavRecorder gRecorder;

} // namespace


namespace rackdroid {


/** Bumped by the audio thread on every underrun that happens with the buffer
already at its ceiling. Read by the render thread, which is the only one
allowed to touch the engine. A live count, not a one-way latch: it needs to
be able to go quiet again so checkEngineUnderload() can notice and undo an
escalation checkEngineOverload() made earlier in the session. */
static std::atomic<int32_t> g_ceilingUnderruns{0};

/** Asks the callback to play silence until the engine has found its feet, then
fade in. Set whenever a stream starts and whenever a port arrives, which is
what loading a patch looks like from here.

The first second of a patch is not the patch. Its Workers have just been
created and do not have their priority or their cores yet, the caches are
cold, the buffer is at the two bursts every stream opens with, and the
callback is late over and over: 504 underruns in the first second on a
Nothing A024 with a 177-module patch, 47 on an SM-S901E and 6 on a OnePlus
8T with a light one, at every launch and every File > Open. Each of those is
a click, because a sequencer is already running and there is sound to cut
holes in. A hole in silence is not heard. So nothing is let out until a fifth
of a second of callbacks has come in on time, or two seconds have gone by --
a patch too heavy for the device never has an on-time fifth of a second, and
it still has to be heard. */
static std::atomic<bool> g_warmupWanted{true};
/** How long the silence may last, in seconds: long after a patch arrives, when
the thread tuner has a search ahead of it, short after a mere reopen. */
static std::atomic<int> g_warmupLimitSec{6};
/** Held by the thread tuner while it measures a new patch at each count: the
silence then does not end on the first on-time fifth of a second, which any
count can have, but when the tuner has picked one -- or the limit runs out. */
static std::atomic<bool> g_warmupHold{false};

/** Starts the silence by hand: the thread tuner about to measure again. */
void audioWarmupBegin() {
	g_warmupLimitSec.store(8, std::memory_order_relaxed);
	g_warmupHold.store(true, std::memory_order_relaxed);
	g_warmupWanted.store(true, std::memory_order_relaxed);
}

/** Audio focus, as Android reports it to MainActivity: whether it is ours at
the moment, and when that last changed. Losing it for a moment is what a
volume key does on a Nothing A024 -- the system plays its own tick -- and for
those three seconds the callback there was held up 30 ms at a time and
underran four hundred times a second. Nothing in this app causes that and
nothing in it can be tuned to prevent it; what it can do is not mistake it for
the patch, which it did: the thread tuner answered with ten more seconds of
trying counts out loud and the block ladder with a step up. */
static std::atomic<bool> g_focusLost{false};
static std::atomic<double> g_focusChangedAt{-1e9};

bool audioFocusDisturbed() {
	return g_focusLost.load(std::memory_order_relaxed)
		|| rack::system::getTime() - g_focusChangedAt.load(std::memory_order_relaxed) < 5.0;
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeAudioFocusChanged(JNIEnv*, jobject, jint change) {
	// AudioManager.AUDIOFOCUS_GAIN and its variants are positive, every loss
	// negative.
	g_focusLost.store(change < 0, std::memory_order_relaxed);
	g_focusChangedAt.store(rack::system::getTime(), std::memory_order_relaxed);
}

void audioWarmupHold(bool hold) {
	// Whoever holds it has a few seconds of measuring to do, whatever opened
	// the stream: a reopen alone would have allowed two.
	if (hold)
		g_warmupLimitSec.store(10, std::memory_order_relaxed);
	g_warmupHold.store(hold, std::memory_order_relaxed);
}
/** The longest any callback has taken since audioTrimBuffer() last looked, in
ns. What the buffer is sized from before anything underruns. */
static std::atomic<int64_t> g_callbackPeakNanos{0};
/** The share of its deadline the callback has been using, smoothed over about
half a second: what the toolbar's load meter shows. */
static std::atomic<int32_t> g_loadForMeter{0};
/** Audio ports on the device, for threads that may not look at the device. */
static std::atomic<int> g_portCount{0};
/** Goes up each time a port arrives: from outside, a patch being loaded. */
static std::atomic<int> g_portEpoch{0};

int audioPortEpoch() {
	return g_portEpoch.load(std::memory_order_relaxed);
}
/** How long the last silence lasted, in ms, for the log; -1 once reported. */
static std::atomic<int32_t> g_warmupEndedMs{-1};
static std::atomic<int32_t> g_warmupStartUnderruns{0};

int32_t audioCeilingUnderrunCount() {
	return g_ceilingUnderruns.load(std::memory_order_relaxed);
}

/** Every underrun, not just the ones at the buffer ceiling. The distinction
matters: the count above deliberately ignores underruns the LatencyTuner can
still answer by growing the buffer, because those do not prove the engine is
short of anything. But the listener hears all of them, and asking "is this
configuration actually clean?" is a different question from "is this engine
overloaded?" -- checkThreadSearch() in main_android.cpp judges its rungs on
this one after a rung that crackled audibly passed as clean on the other. */
static std::atomic<int32_t> g_totalUnderruns{0};

int32_t audioUnderrunCount() {
	return g_totalUnderruns.load(std::memory_order_relaxed);
}

/** How full the callback's deadline was, as a percentage, peak since last read.

An underrun is a rare event: to know whether a thread count works you have to
wait long enough for one to happen, or not happen, and that is why the tuner's
windows are five seconds and why walking down a ladder of them took an A024
forty-five audible seconds to find its answer.

This is the same question asked the other way round. Every callback has to
produce `numFrames` frames in numFrames/sampleRate -- 21.3 ms at a 1024-frame
block -- and how long it actually took is knowable the moment it finishes, 47
times a second. 20% means four fifths of the budget was left over; 98% means
the next jittery callback breaks; over 100% means the audio is already broken
and no amount of waiting will make it otherwise. It answers in a single
callback what counting underruns needs seconds to guess at.

Both the peak and the mean, because they answer different questions and the
first version published only the peak. A peak over the deadline is JITTER --
one callback preempted by the system -- and absorbing jitter is precisely what
the buffer is for; at a 96-frame callback the deadline is 2 ms and a single
scheduling hiccup reads 300% while the audio is perfectly clean. A MEAN over
the deadline is a production deficit: the engine is not making frames as fast
as they are consumed, and no buffer depth saves that, it only postpones it.

Getting this wrong cost a real regression. The rule that rejects a thread count
early was written against the peak, on the reasoning that one late callback is
what a listener hears. That is true of underruns and false of load, and once
the block-size ladder reached 64 frames the tuner began declaring perfectly
clean configurations hopeless -- "0 underruns in 1.2s ... it cannot keep up" --
and thrashing through every rung, which then caused the first real underruns of
the session.

Read-and-reset, so each reader gets the interval since it last looked; there is
only ever one reader, the tuner. */
static std::atomic<int32_t> g_loadPeakPercent{0};
static std::atomic<int64_t> g_loadSumPercent{0};
static std::atomic<int32_t> g_loadCount{0};
/** The same sum with each callback counted as no more than three deadlines.
For the silent measurement of thread counts only. A thread that keeps its core
busy at real-time priority is taken off it by the kernel for a fixed slice of
every period -- on a Nothing A024, 95 to 120 ms every 1.35 s, with 2 to 4 ms of
work in the callback and no voluntary switch; on an SM-S901E 65 to 80 ms every
second -- and one of those in a 0.2 s slice of the measurement adds fifty
points to it. The same 177-module patch read 1:72% 3:65% one day and 1:201%
3:136% the next, on a phone that was running the light patch at the same 22%
both days: the two counts that looked hopeless were the two a stall had
landed in, and the count left standing was the one at 95%. */
static std::atomic<int64_t> g_loadSumClipped{0};
static const int32_t LOAD_CLIP_PERCENT = 300;

/** Frames the last callback was actually asked to produce.

Not the same number as the engine's block size, and the difference is the whole
reason this exists: alignToBurst() rounds the request to a whole number of the
device's bursts, so a 64-frame block becomes a 96-frame callback on a 96-burst
phone and a 1024-frame block becomes 1056. Anything that needs to know how long
a callback has must use this, not audioBlockSize() -- deriving a deadline from
64 frames when 96 arrive understates it by a third.

Read from the callback rather than from the builder because it is the only
figure that cannot be wrong: it is what the stream handed us, after whatever
the device decided to do with the request. Zero until the first callback. */
static std::atomic<int32_t> g_callbackFrames{0};
static int32_t callbackFloorFrames(oboe::AudioStream* stream);

int32_t audioCallbackFrames() {
	return g_callbackFrames.load(std::memory_order_relaxed);
}


void audioEngineLoadTake(int32_t* peak, int32_t* mean, int32_t* callbacks, int32_t* clippedMean) {
	int64_t clipped = g_loadSumClipped.exchange(0, std::memory_order_relaxed);
	int32_t p = g_loadPeakPercent.exchange(0, std::memory_order_relaxed);
	int64_t sum = g_loadSumPercent.exchange(0, std::memory_order_relaxed);
	int32_t n = g_loadCount.exchange(0, std::memory_order_relaxed);
	if (peak)
		*peak = p;
	if (mean)
		*mean = (n > 0) ? (int32_t) (sum / n) : 0;
	if (callbacks)
		*callbacks = n;
	if (clippedMean)
		*clippedMean = (n > 0) ? (int32_t) (clipped / n) : 0;
}

/** Callbacks that ran badly late, kept for the frame loop to write down.

The load figures above say THAT a block took a second; they cannot say why,
and the two candidate answers call for opposite fixes. A callback that was
computing the whole time (CPU time close to wall time -- spinning at the
engine's barriers counts as computing) is short of cores or of workers. One
that was not (CPU time far below wall time) was waiting: blocked on something,
or runnable and not given a core, which the context-switch counts tell apart --
voluntary switches are waits, involuntary ones are preemption. Beside that goes
what the render thread was doing when the callback started and when it ended,
since that is the other party in almost every theory of a stall here.

Single producer (the callback), single consumer (the frame loop). The callback
never waits: it overwrites the oldest record, and the reader counts what it
missed. A record overwritten mid-read can come out garbled; that is the price
of never making the callback wait, and it takes a thirty-two-deep backlog. */
struct SlowCallback {
	double at;          // rack::system::getTime() when the callback ended
	int32_t wallUs;
	int32_t cpuUs;      // this thread's CPU time over the same stretch
	int32_t percent;    // of the callback's deadline
	int16_t volSwitches;
	int16_t involSwitches;
	int8_t cpuStart, cpuEnd;
	int8_t phaseStart, phaseEnd;
	int32_t threads;    // settings::threadCount at the time
};
static const int SLOW_RING = 32;
static SlowCallback g_slowRing[SLOW_RING];
static std::atomic<uint32_t> g_slowWrite{0};
/** Twice the deadline: one late block is absorbed by the buffer, and below
this the log would fill with scheduling noise at small block sizes. */
static const int32_t SLOW_CALLBACK_PERCENT = 200;

int audioReportSlowCallbacks() {
	static uint32_t read = 0;
	int written = 0;
	uint32_t write = g_slowWrite.load(std::memory_order_acquire);
	uint32_t dropped = 0;
	if (write - read > (uint32_t) SLOW_RING) {
		dropped = write - read - SLOW_RING; // lapped; keep the newest
		read = write - SLOW_RING;
	}
	// Same log-budget reasoning as audioReportUnderruns(): a stall is a run of
	// these, and the first few of each second carry the story.
	static double budgetFrom = 0.0;
	static int budget = 0;
	double now = rack::system::getTime();
	if (now - budgetFrom >= 1.0) {
		budgetFrom = now;
		budget = 8;
	}
	int skipped = 0;
	for (; read != write; read++) {
		const SlowCallback& c = g_slowRing[read % SLOW_RING];
		if (budget <= 0) {
			skipped++;
			continue;
		}
		budget--;
		written++;
		AUDIO_WARN("Oboe: slow callback at %.3f: %.1f ms wall (%d%% of the deadline), "
			"%.1f ms cpu, switches %d voluntary / %d involuntary, cpu%d->cpu%d, "
			"render %s->%s, %d threads",
			c.at, c.wallUs / 1000.0, c.percent, c.cpuUs / 1000.0,
			c.volSwitches, c.involSwitches, c.cpuStart, c.cpuEnd,
			windowPhaseName(c.phaseStart), windowPhaseName(c.phaseEnd), c.threads);
	}
	if (skipped > 0 || dropped > 0)
		AUDIO_WARN("Oboe: %d more slow callbacks not written (%u lost to a full ring)",
			skipped + (int) dropped, dropped);
	return written;
}


/** When the last underrun happened, as rack::system::getTime(). */
static std::atomic<double> g_lastUnderrunAt{0.0};

double audioSecondsSinceUnderrun() {
	double at = g_lastUnderrunAt.load(std::memory_order_relaxed);
	return at <= 0.0 ? 1e9 : rack::system::getTime() - at;
}

bool audioUnderrunsRecently() {
	double at = g_lastUnderrunAt.load(std::memory_order_relaxed);
	if (at <= 0.0)
		return false;
	return rack::system::getTime() - at < 2.0;
}

/** The audio callback thread, as gettid() -- readable only from inside the
callback, so it is published from there the first time each callback thread
runs. Zero until then. ADPF wants it at the head of its thread list, and the
core pinning in main_android.cpp keeps the Workers off its core. */
static std::atomic<int> g_audioThreadTid{0};

int audioCallbackThreadTid() {
	return g_audioThreadTid.load(std::memory_order_relaxed);
}

/** When the output stream was last (re)opened. The first seconds after a
reopen underrun regardless of the patch -- the device is settling and the
engine has just been handed a new block size -- so anything judging the engine
by underruns has to skip them. */
static double g_lastStreamOpen = 0.0;

double audioSecondsSinceStreamOpen() {
	if (g_lastStreamOpen <= 0.0)
		return 1e9;
	return rack::system::getTime() - g_lastStreamOpen;
}

/* What the callback saw when the count last moved, so the report can be
written from somewhere it is allowed to block. Packed into one word because
two separate atomics could be read a callback apart and describe no buffer
that ever existed: size in the low 16 bits, capacity in the high 16. */
static std::atomic<uint32_t> g_underrunBuffer{0};

void audioReportUnderruns() {
	// Said from here because the callback must not log: see onAudioReady.
	int32_t warm = g_warmupEndedMs.exchange(-1, std::memory_order_relaxed);
	if (warm >= 0)
		AUDIO_WARN("Oboe: audio let out after %d ms of silence (%d underruns fell in it)%s",
			warm, g_totalUnderruns.load(std::memory_order_relaxed)
				- g_warmupStartUnderruns.load(std::memory_order_relaxed),
			warm >= g_warmupLimitSec.load(std::memory_order_relaxed) * 1000
				? " -- it never settled; playing anyway" : "");

	static int32_t lastReported = 0;
	static double nextReportAt = 0.0;
	int32_t now = g_totalUnderruns.load(std::memory_order_relaxed);
	if (now == lastReported)
		return;
	// One line per second, carrying everything since the last one. Underruns
	// arrive one per callback, so reporting each frame wrote a hundred lines a
	// second into a log file with a size cap -- the evidence of what went
	// wrong would push itself out of the file it is meant to be found in.
	double t = rack::system::getTime();
	if (t < nextReportAt)
		return;
	nextReportAt = t + 1.0;
	uint32_t packed = g_underrunBuffer.load(std::memory_order_relaxed);
	AUDIO_WARN("Oboe: %d underruns (%d total), buffer now %d frames of %d",
		now - lastReported, now, (int) (packed & 0xffff), (int) (packed >> 16));
	lastReported = now;
}


static const int NUM_OUTPUTS = 2;
static const int NUM_INPUTS = 2;
static const int DEFAULT_SAMPLE_RATE = 48000;
// 256 upstream. Measured on hardware with a 224-module patch at 48 kHz,
// underruns over 30 s: 96 frames -> 6574, 256 -> 2381, 512 -> 1218,
// 1024 -> 652. Every doubling roughly halves them, so this is not a knob
// with a sweet spot, it is a straight trade of latency for headroom, and
// 256 was picked for a desktop that has the CPU to spare. 512 costs
// 10.7 ms at 48 kHz and buys back half the dropouts; 1024 would buy half
// again for 21 ms, which is too much to play through. Users who want that
// can pick it in the Audio module -- this only moves the starting point.
static const int DEFAULT_BLOCK_SIZE = 512;

/** Remembers the block size across launches, because the device is built
before anyone tells it which one to use. Rack stores the real value per audio
port in the patch and applies it just after construction, so a device that
opens at DEFAULT_BLOCK_SIZE is then closed and reopened at the patch's value --
measured on an S22 at 452 ms to close plus 222 ms to open, 671 ms of startup
and an audible gap, paid on every single launch to arrive at the same number as
last time. Starting from the remembered value makes Rack's setBlockSize() a
no-op in the common case (same patch, same settings) and costs nothing when the
guess is wrong: that is exactly today's behaviour. */
static std::string blockSizeMemoPath() {
	return rack::asset::user("audio-blocksize");
}

/** Sizes below this one are known not to have held, so the step-down at
startup does not try them again every launch. Zero means nothing is known.
Stored beside the block size, cleared whenever the block size itself changes
for any other reason -- a different patch, or the ladder moving -- because what
failed was this size on that workload, not for ever.

"Not for ever" was the intention and not the behaviour. The verdict was written
once and then believed for the life of the install, so a single passing
disturbance -- another app, a thermal moment, a phone answering a call --
condemned a block size permanently and every launch after it opened at a higher
latency than the device needed. Observed on an S22 during this session's own
testing: an artificial CPU load made 512 frames fail once, and the device then
opened at 1024 on every launch afterwards with nothing able to undo it.

So the verdict expires, the same way the thread tuner's scores do, and with the
same doubling backoff: it is skipped for a few launches, then asked again, and
a size that keeps failing is asked about progressively less often rather than
never again. Counted in launches, not seconds -- the probe happens once per
launch, so that is the unit in which "ask again later" actually means
something, and it needs no clock that the user can change. */
static int g_driverBlockSize = 0;
static int g_knownTooSmall = 0;
/** Launches still to skip before the verdict above is worth re-testing. */
static int g_tooSmallSkips = 0;
/** How many were skipped last time, so the next failure can ask for twice as
many. Doubling from four: a size that genuinely does not suit the device is
retried four launches later, then eight, and so on, and the cost of being wrong
about it stays bounded at one stream reopen per retry. */
static int g_tooSmallPenalty = 0;
// One launch, doubling to four. It was four doubling to sixty-four, and a
// verdict reached during a stall that had nothing to do with the block size --
// Workers born on the callback's core -- then held a Nothing A024 at 1024
// frames, 57 ms of delay, for launch after launch. A wrong verdict now costs
// one launch; a right one still spares most launches the failed attempt.
static const int TOO_SMALL_SKIPS_MIN = 1;
static const int TOO_SMALL_SKIPS_MAX = 4;

/** The size the block was at before the app itself raised it, or 0 while it
has not. A raise answers trouble that may be gone in a minute -- a heavy
moment, a warm phone -- and is tried lower again once things are quiet, so
until that has been tried and has failed it is not something to remember:
the next launch opens where this one did. It used to be remembered at once,
and the patch saved it too, so a OnePlus 8T that had been through an overload
opened a light patch at 512 frames, and a Lenovo TB-X306X at 512 as well,
launch after launch, until the step-down got round to them. */
static int g_raisedFrom = 0;
/** True while the app's own tuner, not a patch or a menu, is changing the size;
and whether that change is the overload ladder reaching for headroom, which is
the only kind that counts as a raise. Going back up after a smaller size failed
its trial is not one: that size has just been shown to be needed. */
static bool g_ownBlockChange = false;
static bool g_ladderRaise = false;

static void writeBlockSizeMemo() {
	if (g_driverBlockSize <= 0)
		return;
	FILE* f = std::fopen(blockSizeMemoPath().c_str(), "w");
	if (!f)
		return;
	std::fprintf(f, "%d %d %d %d\n", g_raisedFrom > 0 ? g_raisedFrom : g_driverBlockSize, g_knownTooSmall,
		g_tooSmallSkips, g_tooSmallPenalty);
	std::fclose(f);
}

int audioKnownTooSmallBlock() {
	return g_knownTooSmall;
}

int audioTooSmallLaunchesLeft() {
	if (g_tooSmallSkips <= 0)
		return 0;
	g_tooSmallSkips--;
	writeBlockSizeMemo();
	return g_tooSmallSkips + 1; // what this launch was told to wait out
}

void audioNoteBlockTooSmall(int bs) {
	// Failing again doubles the wait; failing a different size starts over.
	if (bs == g_knownTooSmall && g_tooSmallPenalty > 0)
		g_tooSmallPenalty = (g_tooSmallPenalty * 2 > TOO_SMALL_SKIPS_MAX)
			? TOO_SMALL_SKIPS_MAX : g_tooSmallPenalty * 2;
	else
		g_tooSmallPenalty = TOO_SMALL_SKIPS_MIN;
	g_knownTooSmall = bs;
	g_tooSmallSkips = g_tooSmallPenalty;
	// The way back down was tried and did not hold: the raise was not a
	// passing thing after all, and the size it led to is the one to keep.
	g_raisedFrom = 0;
	writeBlockSizeMemo();
}

/** Engine > Audio block, as stored: -1 not read yet, 0 automatic, else the
user's size. In a file of its own beside the block-size memo, because it is a
setting and that is a measurement. */
static std::atomic<int> g_blockChoice{-1};
static std::atomic<int> g_blockChoicePending{-1};

static bool validBlockSize(int v) {
	return v >= 64 && v <= 1024 && (v & (v - 1)) == 0;
}

static std::string blockChoicePath() {
	return rack::asset::user("audio-block-choice");
}

int audioBlockChoice() {
	int choice = g_blockChoice.load(std::memory_order_relaxed);
	if (choice >= 0)
		return choice;
	choice = 0;
	if (FILE* f = std::fopen(blockChoicePath().c_str(), "r")) {
		int v = 0;
		if (std::fscanf(f, "%d", &v) == 1 && validBlockSize(v))
			choice = v;
		std::fclose(f);
	}
	g_blockChoice.store(choice, std::memory_order_relaxed);
	return choice;
}

static void storeBlockChoice(int choice) {
	g_blockChoice.store(choice, std::memory_order_relaxed);
	if (FILE* f = std::fopen(blockChoicePath().c_str(), "w")) {
		std::fprintf(f, "%d\n", choice);
		std::fclose(f);
	}
	AUDIO_WARN("Oboe: audio block is now %s%s", choice > 0 ? "fixed at " : "automatic",
		choice > 0 ? std::to_string(choice).c_str() : "");
}

void audioRequestBlockChoice(int choice) {
	if (choice == 0 || validBlockSize(choice))
		g_blockChoicePending.store(choice, std::memory_order_relaxed);
}


static int rememberedBlockSize() {
	FILE* f = std::fopen(blockSizeMemoPath().c_str(), "r");
	if (!f)
		return DEFAULT_BLOCK_SIZE;
	int v = 0, tooSmall = 0, skips = -1, penalty = 0;
	int n = std::fscanf(f, "%d %d %d %d", &v, &tooSmall, &skips, &penalty);
	std::fclose(f);
	if (n >= 2 && tooSmall >= 64 && tooSmall <= 4096 && (tooSmall & (tooSmall - 1)) == 0) {
		g_knownTooSmall = tooSmall;
		// A memo written before the verdict could expire carries no counts.
		// Treat it as one that has just been made rather than one that has
		// already served its time: the size did fail, once, and the point of
		// this is to re-ask eventually, not immediately.
		// Clamped rather than rejected: a memo from before the limits came
		// down can ask for up to sixty-four.
		g_tooSmallSkips = (n >= 3 && skips >= 0)
			? (skips > TOO_SMALL_SKIPS_MAX ? TOO_SMALL_SKIPS_MAX : skips)
			: TOO_SMALL_SKIPS_MIN;
		g_tooSmallPenalty = (n >= 4 && penalty >= TOO_SMALL_SKIPS_MIN
				&& penalty <= TOO_SMALL_SKIPS_MAX) ? penalty : TOO_SMALL_SKIPS_MIN;
	}
	// Only sizes Rack itself offers; anything else is a stale or corrupt file.
	if (n < 1 || v < 64 || v > 4096 || (v & (v - 1)) != 0)
		v = DEFAULT_BLOCK_SIZE;
	// A size the user fixed is where the device opens, whatever it last ran at.
	if (audioBlockChoice() > 0)
		v = audioBlockChoice();
	// The size in use from here, which is what the memo has to be written with.
	// Only setBlockSize() used to publish this, so on a launch where nothing
	// changed the block size it stayed zero and every write was silently
	// dropped -- the launch countdown then read 4 for ever, six launches
	// running, which is how this was found.
	g_driverBlockSize = v;
	return v;
}

/** The device's own burst, remembered across launches.

Rack offers block sizes in powers of two -- 64, 128, 256, 512, 1024 -- because
that is what desktop audio hardware works in. Phones do not all agree. A Nothing
A024 reports a 96-frame burst, so a 512-frame callback straddles 5.33 of them
and every single callback lands mid-burst; the stream's own numbers in the log
(buffer 192, later 2976) are multiples of 96, and ours was the one number that
was not. On that device the result was continuous underruns that no amount of
thread tuning touched -- dropping from seven workers to two changed nothing,
which is the signature of a cost paid per callback rather than per sample.

So ask for the nearest whole number of bursts instead. The engine does not care:
onAudioReady() processes whatever frame count it is handed. The burst is only
knowable after a stream is open, hence the memo: the first open on a new device
may be misaligned, every one after it is not. */
static std::string burstMemoPath() {
	return rack::asset::user("audio-burst");
}

static int g_framesPerBurst = 0;

static int rememberedBurst() {
	FILE* f = std::fopen(burstMemoPath().c_str(), "r");
	if (!f)
		return 0;
	int v = 0;
	int n = std::fscanf(f, "%d", &v);
	std::fclose(f);
	if (n < 1 || v < 8 || v > 4096)
		return 0;
	return v;
}

static void rememberBurst(int burst) {
	FILE* f = std::fopen(burstMemoPath().c_str(), "w");
	if (!f)
		return;
	std::fprintf(f, "%d\n", burst);
	std::fclose(f);
}

/** Rounds a block size to a whole number of bursts, never below one burst.
A burst we do not know yet leaves the size alone. */
static int alignToBurst(int bs, int burst) {
	if (burst <= 0 || bs <= 0)
		return bs;
	int bursts = (bs + burst / 2) / burst;
	if (bursts < 1)
		bursts = 1;
	return bursts * burst;
}
/** What callback size a given engine block size would actually produce here,
or 0 before the device's burst is known.

Asked by the block-size ladder before it spends a stream reopen. On most phones
the answer tracks the request, but a Lenovo TB-X306X reports a 960-FRAME burst
-- ten times an S22's -- and there every block size at or below 960 aligns to
the same single burst. The ladder could not tell: it stepped 512 down to 256,
paid the reopen and an underrun, got the same 960-frame callback back, measured
the latency as 9 ms WORSE than before, and then told the user "a 256-frame
block holds; keeping the lower latency". Wasted work is forgivable; the app
claiming an improvement it did not make is not. */
int32_t audioAlignedCallbackFrames(int blockSize) {
	if (g_framesPerBurst <= 0)
		return 0;
	return alignToBurst(blockSize, g_framesPerBurst);
}

static void rememberBlockSize(int bs) {
	g_driverBlockSize = bs;
	writeBlockSizeMemo();
}


struct OboeDevice : rack::audio::Device, oboe::AudioStreamDataCallback, oboe::AudioStreamErrorCallback {
	std::shared_ptr<oboe::AudioStream> outputStream;
	std::shared_ptr<oboe::AudioStream> inputStream;
	float sampleRate = DEFAULT_SAMPLE_RATE;
	int blockSize = rememberedBlockSize();
	std::vector<float> inputBuffer;
	/** Guards stream open/close against the data callback. */
	std::mutex streamMutex;
	/** Grows the buffer when the device underruns. Oboe opens with the
	smallest buffer it thinks will hold, which on a phone under a heavy patch
	is regularly one burst too few -- and nothing here used to notice: the
	stream just clicked, forever, at whatever size it started with. The tuner
	trades a burst of latency for stability, and only on the devices that
	actually need it. Owned by the stream it tunes, so it is rebuilt on every
	open and dropped before the stream closes. */
	std::unique_ptr<oboe::LatencyTuner> latencyTuner;
	/** Last underrun count reported, so a change can be logged once instead of
	every callback. A user saying "it crackles" and a log saying "xruns 0 -> 37"
	are not the same bug report. */
	int32_t lastXRuns = 0;
	// Callback-thread state of the start-up silence; see g_warmupWanted.
	bool warming = true;
	int32_t warmGoodFrames = 0;
	int32_t warmFrames = 0;
	float warmGain = 0.f;
	float meterLoad = 0.f;

	OboeDevice() {
		openStreams("device created");
	}

	~OboeDevice() override {
		closeStreams();
	}

	/** `why` names the caller in the log. Reopening is not cheap -- closing the
	output stream alone waits ~170 ms for the callback to stop -- and a startup
	that does it five times over looks identical, from the log, whether it is
	the sample rate settling, the block size being repaired, or the HAL
	disconnecting underneath. Saying which removes the guesswork. */
	void openStreams(const char* why) {
		openStreamsLocked(why);
		// The burst is only readable from an open stream, so the very first
		// open on a device it has never seen can be misaligned. Learn it and
		// take the one reopen; the memo means it happens once per install, not
		// once per launch. See alignToBurst() for why this matters.
		if (!realignTo)
			return;
		int aligned = realignTo;
		realignTo = 0;
		AUDIO_WARN("Oboe: %d-frame callbacks do not fit this device's %d-frame "
			"burst; reopening at %d", blockSize, g_framesPerBurst, aligned);
		closeStreams();
		openStreamsLocked("burst alignment");
	}

	/** Set by openStreamsLocked when the stream it just opened turned out to be
	misaligned with the device's burst, naming the size to use instead. */
	int realignTo = 0;

	void openStreamsLocked(const char* why) {
		double t0 = rack::system::getTime();
		std::lock_guard<std::mutex> lock(streamMutex);
		realignTo = 0;
		if (!g_framesPerBurst)
			g_framesPerBurst = rememberedBurst();
		int callbackFrames = alignToBurst(blockSize, g_framesPerBurst);

		oboe::AudioStreamBuilder outBuilder;
		outBuilder.setDirection(oboe::Direction::Output)
			->setPerformanceMode(oboe::PerformanceMode::LowLatency)
			->setSharingMode(oboe::SharingMode::Exclusive)
			// Optional per Oboe's own docs, but read by the audio policy
			// service (API 28+) as one input into whether it grants this
			// Exclusive request or hands back Shared instead -- matches the
			// AudioAttributes MainActivity.requestAudioFocusFromNative() uses
			// for the focus request made just before this stream opens
			// (main_android.cpp), so both halves of the ask agree.
			->setUsage(oboe::Usage::Media)
			->setContentType(oboe::ContentType::Music)
			->setFormat(oboe::AudioFormat::Float)
			->setChannelCount(NUM_OUTPUTS)
			->setSampleRate((int) sampleRate)
			->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
			->setFramesPerDataCallback(callbackFrames)
			->setDataCallback(this)
			->setErrorCallback(this);

		oboe::Result result = outBuilder.openStream(outputStream);
		if (result != oboe::Result::OK) {
			WARN("Oboe: could not open output stream: %s", oboe::convertToText(result));
			return;
		}
		// The stream may have been opened with a different rate than requested.
		sampleRate = outputStream->getSampleRate();

		int burst = outputStream->getFramesPerBurst();
		if (burst >= 8 && burst <= 4096 && burst != g_framesPerBurst) {
			g_framesPerBurst = burst;
			rememberBurst(burst);
		}
		if (alignToBurst(blockSize, g_framesPerBurst) != callbackFrames)
			realignTo = alignToBurst(blockSize, g_framesPerBurst);

		oboe::AudioStreamBuilder inBuilder;
		inBuilder.setDirection(oboe::Direction::Input)
			->setPerformanceMode(oboe::PerformanceMode::LowLatency)
			->setFormat(oboe::AudioFormat::Float)
			->setChannelCount(NUM_INPUTS)
			->setSampleRate(outputStream->getSampleRate());

		result = inBuilder.openStream(inputStream);
		if (result != oboe::Result::OK) {
			WARN("Oboe: no input stream (%s), running output-only", oboe::convertToText(result));
			inputStream.reset();
		}

		inputBuffer.resize(outputStream->getBufferCapacityInFrames() * NUM_INPUTS);

		latencyTuner.reset(new oboe::LatencyTuner(*outputStream));
		// The tuner starts every stream at two bursts. See callbackFloorFrames.
		int32_t callbackFloor = callbackFloorFrames(outputStream.get());
		if (callbackFloor > outputStream->getBufferSizeInFrames())
			outputStream->setBufferSizeInFrames(callbackFloor);
		lastXRuns = 0;

		if (inputStream)
			inputStream->requestStart();
		outputStream->requestStart();
		g_lastStreamOpen = rack::system::getTime();
		// blockSize belongs in here: it is the one number in this line the app
		// itself chooses, and leaving it out cost a debugging round trip --
		// a log full of underruns with no way to tell which rung of
		// checkBlockSizeOverload()'s ladder was in effect at the time.
		AUDIO_WARN("Oboe: openStreams took %.0f ms", (rack::system::getTime() - t0) * 1000.0);
		// A reopen by itself -- a block size step -- is not a new patch: two
		// seconds, unless a patch load already asked for longer just now.
		if (!g_warmupWanted.load(std::memory_order_relaxed))
			g_warmupLimitSec.store(2, std::memory_order_relaxed);
		g_warmupWanted.store(true, std::memory_order_relaxed);
		AUDIO_WARN("Oboe: stream started (%s), sampleRate=%g block=%d callback=%d burst=%d buffer=%d "
			"capacity=%d sharing=%s performance=%s api=%s",
			why, sampleRate, blockSize, callbackFrames, outputStream->getFramesPerBurst(),
			outputStream->getBufferSizeInFrames(), outputStream->getBufferCapacityInFrames(),
			oboe::convertToText(outputStream->getSharingMode()),
			oboe::convertToText(outputStream->getPerformanceMode()),
			oboe::convertToText(outputStream->getAudioApi()));
		// Exclusive+LowLatency is what we ask for, not necessarily what gets
		// granted: Android can silently hand back a Shared stream instead, and
		// Shared goes through AudioFlinger's mixer, with materially worse and
		// less consistent latency than the dedicated path block size and thread
		// priority were tuned around. That downgrade is invisible without
		// logging the GRANTED mode: underruns that never improve no matter the
		// thread count look identical to a CPU shortage from inside this
		// process, but no thread-side fix here can undo a sharing-mode
		// fallback.
		//
		// WHY the downgrade happens is not something this side of the API can
		// see, and guessing at it cost a full debugging session. On a OnePlus
		// 8T (SM8250, OxygenOS/Android 15) the system log -- NOT anything this
		// app can print -- gave the real answer:
		//
		//   AAudioServiceExtImpl: isAAudioCompatible call
		//       getListValueByUid(aaudio-compatible-apps) but return null
		//   AAudioService: openStream(...): aaudio denied with imcompatible
		//       policy such as peformance noise
		//
		// An Oplus/OnePlus vendor allowlist ("aaudio-compatible-apps"), not
		// anything in AOSP, gates the low-latency path per app -- and on that
		// build it resolves to null, i.e. plausibly denies EVERY third-party
		// app. Confirmed there with the device idle, no other app running, and
		// audio focus held: focus, thread count, block size and core affinity
		// all make no difference to it whatsoever. So do not read Shared as
		// "another app stole the device", and do not spend another session
		// trying to earn Exclusive from the app side; on a device that refuses
		// it, coping (bigger blocks -- see checkBlockSizeOverload in
		// main_android.cpp) is the only lever left.
		if (outputStream->getSharingMode() != oboe::SharingMode::Exclusive) {
			AUDIO_WARN("Oboe: asked for Exclusive sharing but got %s -- the "
				"dedicated low-latency path was denied. Underruns from here on "
				"may have nothing to do with CPU or thread count; check the "
				"system log (AAudioService/AudioFlinger) for the reason, which "
				"is not visible to this app",
				oboe::convertToText(outputStream->getSharingMode()));
			// isMMapSupported() is the device-wide capability; isMMapUsed() is
			// what THIS stream actually got. support=1 with used=0 -- the 8T's
			// reading -- means the hardware can do it and something above it
			// said no, which is the vendor-allowlist case described above, not
			// a hardware limit. These are test-only per Oboe's own header (may
			// change/disappear), used purely to log, never to alter behavior.
			AUDIO_WARN("Oboe: device MMAP support=%d, this stream using MMAP=%d "
				"-- support=0 means no app here can ever get Exclusive; "
				"support=1 with used=0 means something above the HAL refused "
				"this app specifically",
				(int) oboe::OboeExtensions::isMMapSupported(),
				(int) oboe::OboeExtensions::isMMapUsed(outputStream.get()));
		}
		// The single most common cause of unexplained crackling, and until now
		// the one thing a log could not show: the engine pinned to a rate the
		// device does not run at. Rack then resamples every block, on the audio
		// thread, forever, and nothing on screen says so. Zero is AUTO, which by
		// definition cannot disagree.
		if (rack::settings::sampleRate > 0.f && rack::settings::sampleRate != sampleRate)
			AUDIO_WARN("Oboe: engine is fixed at %g Hz but the device opened at %g Hz"
				" -- Rack resamples continuously; set Engine > Sample rate to Auto",
				rack::settings::sampleRate, sampleRate);
		onStartStream();
	}

	void closeStreams() {
		double t0 = rack::system::getTime();
		std::lock_guard<std::mutex> lock(streamMutex);
		// Holds a reference to the stream, so it goes first.
		latencyTuner.reset();
		if (outputStream) {
			// This one keeps its wait: the output stream owns the data
			// callback, and stop() returning means the callback is no longer
			// running. It costs about 170 ms and it is worth them.
			outputStream->stop();
			outputStream->close();
			outputStream.reset();
			onStopStream();
		}
		if (inputStream) {
			// requestStop(), not stop(): stop() waits for the stream to report
			// Stopped and this one never does, so it burned the full 2 s
			// kDefaultTimeoutNanos on every close -- on the render thread.
			// Nothing needs the wait. The input stream has no data callback of
			// its own; its only reader is the output callback, and the output
			// stream is already stopped and closed above. close() tears the
			// stream down regardless of whether the state transition landed.
			inputStream->requestStop();
			inputStream->close();
			inputStream.reset();
		}
		AUDIO_WARN("Oboe: closeStreams took %.0f ms", (rack::system::getTime() - t0) * 1000.0);
	}

	// rack::audio::Device

	std::string getName() override {
		return "Default";
	}
	int getNumInputs() override {
		return inputStream ? NUM_INPUTS : 0;
	}
	int getNumOutputs() override {
		return NUM_OUTPUTS;
	}

	std::set<float> getSampleRates() override {
		return {44100.f, 48000.f};
	}
	float getSampleRate() override {
		return sampleRate;
	}
	void setSampleRate(float sr) override {
		if (sr == sampleRate)
			return;
		AUDIO_WARN("Oboe: sample rate %g -> %g", sampleRate, sr);
		closeStreams();
		sampleRate = sr;
		openStreams("sample rate changed");
	}

	std::set<int> getBlockSizes() override {
		return {64, 128, 256, 512, 1024};
	}
	int getBlockSize() override {
		return blockSize;
	}
	void setBlockSize(int bs) override {
		// Three callers reach this: a patch file applying the value saved in
		// it, the Audio module's own menu, and the app itself. Picked from a
		// menu, it is the user's choice and is remembered as one. Otherwise a
		// size the user fixed stands -- a patch carries whatever block size
		// its author's device needed, and used to impose it on every device
		// that opened it.
		if (menuUserActionRunning()) {
			if (validBlockSize(bs) && audioBlockChoice() != bs)
				storeBlockChoice(bs);
		}
		else if (audioBlockChoice() > 0) {
			bs = audioBlockChoice();
		}
		else if (!g_ownBlockChange && bs > blockSize) {
			// On Automatic a patch may ask for less latency than the device
			// is running at, never for more: a bigger block in a file is
			// either its author's device or a raise this one made during an
			// overload and then saved. If this patch needs it here, the
			// ladder finds that out in seconds and says so.
			AUDIO_WARN("Oboe: the patch asks for a %d-frame block; staying at %d "
				"(Automatic raises it only when this device needs it)", bs, blockSize);
			return;
		}
		if (bs == blockSize)
			return;
		if (g_ladderRaise && bs > blockSize) {
			if (g_raisedFrom <= 0)
				g_raisedFrom = blockSize;
		}
		else if (!g_ownBlockChange || bs <= g_raisedFrom)
			g_raisedFrom = 0;
		AUDIO_WARN("Oboe: block size %d -> %d", blockSize, bs);
		closeStreams();
		blockSize = bs;
		g_driverBlockSize = bs;
		rememberBlockSize(bs);
		openStreams("block size changed");
	}

	// oboe::AudioStreamDataCallback

	static void recordSlowCallback(int64_t elapsed, int32_t percent, const timespec& cpu0,
			const rusage& ru0, int cpuStart, int phaseStart) {
		timespec cpu1;
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu1);
		rusage ru1;
		getrusage(RUSAGE_THREAD, &ru1);
		uint32_t w = g_slowWrite.load(std::memory_order_relaxed);
		SlowCallback& c = g_slowRing[w % SLOW_RING];
		c.at = rack::system::getTime();
		c.wallUs = (int32_t) (elapsed / 1000);
		c.cpuUs = (int32_t) (((int64_t) (cpu1.tv_sec - cpu0.tv_sec) * 1000000000LL
			+ (cpu1.tv_nsec - cpu0.tv_nsec)) / 1000);
		c.percent = percent;
		c.volSwitches = (int16_t) (ru1.ru_nvcsw - ru0.ru_nvcsw);
		c.involSwitches = (int16_t) (ru1.ru_nivcsw - ru0.ru_nivcsw);
		c.cpuStart = (int8_t) cpuStart;
		c.cpuEnd = (int8_t) sched_getcpu();
		c.phaseStart = (int8_t) phaseStart;
		c.phaseEnd = (int8_t) windowPhase();
		c.threads = rack::settings::threadCount;
		g_slowWrite.store(w + 1, std::memory_order_release);
	}

	void applyWarmup(float* output, int channels, int32_t numFrames, int32_t rate, int64_t elapsedNanos) {
		if (g_warmupWanted.exchange(false, std::memory_order_relaxed)) {
			warming = true;
			warmGoodFrames = 0;
			warmFrames = 0;
			warmGain = 0.f;
			g_warmupStartUnderruns.store(g_totalUnderruns.load(std::memory_order_relaxed),
				std::memory_order_relaxed);
		}
		if (warmGain >= 1.f && !warming)
			return;
		if (rate <= 0 || numFrames <= 0)
			return;
		if (warming) {
			// On time: back before the frames it was asked for have played.
			// (Not "with room to spare" -- a patch running clean at 90% of
			// its deadline never has room to spare and was kept silent for
			// the whole timeout.)
			bool onTime = elapsedNanos * rate < (int64_t) numFrames * 1000000000LL;
			warmGoodFrames = onTime ? warmGoodFrames + numFrames : 0;
			warmFrames += numFrames;
			// Six seconds, because what is being waited for after a patch
			// loads is the thread tuner, and it needs four or five to walk to
			// the count a heavy patch wants: with two, an SM-S901E let out ten
			// seconds of 450 underruns a second while it did.
			if ((warmGoodFrames >= rate / 5 && !g_warmupHold.load(std::memory_order_relaxed))
					|| warmFrames >= rate * g_warmupLimitSec.load(std::memory_order_relaxed)) {
				warming = false;
				g_warmupEndedMs.store((int32_t) ((int64_t) warmFrames * 1000 / rate),
					std::memory_order_relaxed);
			}
			else {
				std::fill_n(output, (size_t) numFrames * channels, 0.f);
				return;
			}
		}
		// A twentieth of a second from nothing to full: short enough not to be
		// noticed as a fade, long enough not to be a click of its own.
		float step = 20.f / rate;
		for (int32_t i = 0; i < numFrames; i++) {
			for (int c = 0; c < channels; c++)
				output[i * channels + c] *= warmGain;
			warmGain = (warmGain + step < 1.f) ? warmGain + step : 1.f;
		}
	}

	oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream, void* audioData, int32_t numFrames) override {
		float* output = (float*) audioData;
		// What the stream actually asks for, which is not the engine's block
		// size wherever alignToBurst() has rounded the request. See
		// audioCallbackFrames().
		g_callbackFrames.store(numFrames, std::memory_order_relaxed);

		// Non-blocking and callback-safe by design: this is what LatencyTuner
		// is for. It only acts when the underrun count has moved.
		if (latencyTuner)
			latencyTuner->tune();
		int32_t xruns = stream->getXRunCount().value();
		if (xruns != lastXRuns) {
			// Count every one of them, however small the buffer still is: this
			// is the number that corresponds to what a listener hears. The
			// stream's own counter restarts at zero on reopen, so take the
			// difference and ignore it when it goes backwards.
			if (xruns > lastXRuns) {
				g_totalUnderruns.fetch_add(xruns - lastXRuns, std::memory_order_relaxed);
				g_lastUnderrunAt.store(rack::system::getTime(), std::memory_order_relaxed);
			}
			lastXRuns = xruns;
			// Underruns while the tuner has already grown the buffer as far as
			// it goes: the device is not jittering, it is short of CPU, and no
			// buffer will fix that. Publish it so the engine can answer.
			int32_t size = stream->getBufferSizeInFrames();
			int32_t capacity = stream->getBufferCapacityInFrames();
			if (size >= capacity - stream->getFramesPerBurst())
				g_ceilingUnderruns.fetch_add(1, std::memory_order_relaxed);
			// Do NOT log from here. Rack's WARN takes a mutex the render
			// thread also holds and ends in fflush() -- a blocking write to
			// flash -- and upstream says as much in logger.cpp: "logging is
			// not used in performance critical code". Firing that from the
			// audio callback, at the exact moment the callback is already
			// late, makes the next underrun more likely rather than less.
			// Publish what was seen; audioReportUnderruns() writes it.
			g_underrunBuffer.store(
				((uint32_t) (capacity & 0xffff) << 16) | (uint32_t) (size & 0xffff),
				std::memory_order_relaxed);
		}

		// The callback thread is one of the threads ADPF needs to know about,
		// and it is the only place its id can be read. Published once per
		// thread, not once ever: every reopened stream calls back on a new
		// one, and the old id pointed ADPF and the core pinning at a thread
		// that no longer existed.
		static thread_local int callbackTid = 0;
		if (callbackTid == 0) {
			callbackTid = gettid();
			g_audioThreadTid.store(callbackTid, std::memory_order_relaxed);
		}

		// Bracket the work ADPF is asked to make fit, and measure how much of
		// the callback's own deadline it used. CLOCK_MONOTONIC because that is
		// what the ADPF API documents its durations against, and because it is
		// a vDSO read -- no syscall, no lock, nothing that can block -- which
		// is the only reason it is allowed in here at all.
		bool reportToAdpf = adpfActive();
		timespec t0;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		// For the slow-callback record. None of these block: thread CPU time
		// and getrusage are plain syscalls on the calling thread's own
		// counters, sched_getcpu is a vDSO read. Taken every time because by
		// the time a callback is known to be slow its start is gone.
		timespec cpu0;
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu0);
		rusage ru0;
		getrusage(RUSAGE_THREAD, &ru0);
		int cpuStart = sched_getcpu();
		int phaseStart = windowPhase();

		const float* input = NULL;
		if (inputStream) {
			if ((int) inputBuffer.size() < numFrames * NUM_INPUTS)
				inputBuffer.resize(numFrames * NUM_INPUTS);
			// Non-blocking read; on underrun the missing frames stay zeroed.
			auto readResult = inputStream->read(inputBuffer.data(), numFrames, 0);
			int framesRead = readResult ? readResult.value() : 0;
			if (framesRead < numFrames) {
				std::fill(inputBuffer.begin() + framesRead * NUM_INPUTS,
					inputBuffer.begin() + numFrames * NUM_INPUTS, 0.f);
			}
			input = inputBuffer.data();
		}

		// Drives Engine::stepBlock() through the subscribed audio Ports.
		processBuffer(input, NUM_INPUTS, output, stream->getChannelCount(), numFrames);

		timespec t1;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		int64_t elapsed = (int64_t) (t1.tv_sec - t0.tv_sec) * 1000000000LL
			+ (t1.tv_nsec - t0.tv_nsec);
		applyWarmup(output, stream->getChannelCount(), numFrames, stream->getSampleRate(), elapsed);
		// Not while the silence is on: those are the start-up callbacks, and a
		// buffer sized for them would be sized for nothing that will happen again.
		if (!warming) {
			int64_t seenNs = g_callbackPeakNanos.load(std::memory_order_relaxed);
			while (elapsed > seenNs && !g_callbackPeakNanos.compare_exchange_weak(
					seenNs, elapsed, std::memory_order_relaxed))
				;
		}
		gRecorder.push(output, (size_t) numFrames * stream->getChannelCount());
		if (reportToAdpf)
			adpfReportNanos(elapsed);
		// The deadline this callback had to meet. Taken from the frame count
		// it was actually handed rather than from blockSize: the two differ
		// wherever the callback is burst-aligned, and dividing by the wrong
		// one would misreport the load by that ratio.
		int32_t rate = stream->getSampleRate();
		if (rate > 0 && numFrames > 0) {
			int64_t deadline = (int64_t) numFrames * 1000000000LL / rate;
			int32_t percent = (int32_t) (elapsed * 100 / deadline);
			// Relaxed compare-exchange max: lock-free, and a lost race costs
			// one sample of a peak that is sampled 47 times a second.
			int32_t seen = g_loadPeakPercent.load(std::memory_order_relaxed);
			while (percent > seen && !g_loadPeakPercent.compare_exchange_weak(
					seen, percent, std::memory_order_relaxed))
				;
			// Half a second of memory whatever the callback size: 2 ms ones
			// and 20 ms ones should read the same patch the same way.
			float weight = (float) numFrames / (rate * 0.5f);
			if (weight > 1.f)
				weight = 1.f;
			meterLoad += ((float) percent - meterLoad) * weight;
			g_loadForMeter.store((int32_t) (meterLoad + 0.5f), std::memory_order_relaxed);
			g_loadSumPercent.fetch_add(percent, std::memory_order_relaxed);
			g_loadSumClipped.fetch_add(percent < LOAD_CLIP_PERCENT ? percent : LOAD_CLIP_PERCENT,
				std::memory_order_relaxed);
			g_loadCount.fetch_add(1, std::memory_order_relaxed);
			if (percent >= SLOW_CALLBACK_PERCENT)
				recordSlowCallback(elapsed, percent, cpu0, ru0, cpuStart, phaseStart);
		}

		return oboe::DataCallbackResult::Continue;
	}

	// oboe::AudioStreamErrorCallback

	void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override {
		// Device disconnected (headphones unplugged, route change): reopen.
		AUDIO_WARN("Oboe: stream error %s, reopening", oboe::convertToText(error));
		closeStreams();
		openStreams("stream error");
	}
};


struct OboeDriver : rack::audio::Driver {
	OboeDevice* device = NULL;

	~OboeDriver() override;

	std::string getName() override {
		return "Android (Oboe)";
	}
	std::vector<int> getDeviceIds() override {
		return {0};
	}
	int getDefaultDeviceId() override {
		return 0;
	}
	std::string getDeviceName(int deviceId) override {
		return (deviceId == 0) ? "Default" : "";
	}
	int getDeviceNumInputs(int deviceId) override {
		return (deviceId == 0) ? NUM_INPUTS : 0;
	}
	int getDeviceNumOutputs(int deviceId) override {
		return (deviceId == 0) ? NUM_OUTPUTS : 0;
	}

	/** When the last port left, or 0 while something is still subscribed. */
	double idleSince = 0.0;

	rack::audio::Device* subscribe(int deviceId, rack::audio::Port* port) override {
		if (deviceId != 0)
			return NULL;
		idleSince = 0.0;
		if (!device)
			device = new OboeDevice;
		// The port has to know its device BEFORE the callback can see the port.
		// Rack's Port::setDeviceId() assigns it from our return value, which is
		// after Device::subscribe() has published the port; a callback landing
		// in between runs core Audio's processInput() with port->device NULL,
		// reads a device sample rate of 0, divides by it, and asks the engine
		// for INT_MAX frames in one stepBlock(). That call never returns: the
		// callback is gone, it holds the Device mutex, and the interface thread
		// stops for good at the next unsubscribe. On a desktop the window is a
		// few instructions wide against a callback every few milliseconds. Here
		// a patch too heavy for the device has the callback running back to
		// back, and it was hit twice in an afternoon on an SM-S901E while
		// changing patch (thread dumps: the callback inside stepBlock for
		// minutes, the interface thread in ~Audio -> Device::unsubscribe ->
		// mutex::lock).
		port->device = device;
		device->subscribe(port);
		g_portCount.store((int) device->subscribed.size(), std::memory_order_relaxed);
		g_portEpoch.fetch_add(1, std::memory_order_relaxed);
		g_warmupLimitSec.store(8, std::memory_order_relaxed);
		// Held from here, not from when the tuner next looks: in between, the
		// first on-time fifth of a second let a blip of the patch out before
		// the silence came back down on it. The tuner lets go.
		g_warmupHold.store(true, std::memory_order_relaxed);
		g_warmupWanted.store(true, std::memory_order_relaxed);
		AUDIO_WARN("Oboe: port subscribed (%d now)", (int) device->subscribed.size());
		return device;
	}

	void unsubscribe(int deviceId, rack::audio::Port* port) override {
		if (deviceId != 0 || !device)
			return;
		device->unsubscribe(port);
		g_portCount.store((int) device->subscribed.size(), std::memory_order_relaxed);
		AUDIO_WARN("Oboe: port unsubscribed (%d left)", (int) device->subscribed.size());
		// Deliberately NOT destroyed here. Rack rewrites a port's driver,
		// device and channel count one after another while restoring a patch,
		// and every one of those is an unsubscribe followed immediately by a
		// subscribe. Tearing the device down in between meant three complete
		// create/destroy cycles on every startup -- each one opening an Oboe
		// stream and then blocking ~170 ms in stop() waiting for the callback
		// to finish -- which is most of the 2.8 s the audio took to settle, and
		// the source of the underruns heard while a patch loads. Keeping it for
		// a moment lets the next subscribe walk straight back in.
		if (device->subscribed.empty())
			idleSince = rack::system::getTime();
	}
};


static OboeDriver* g_driver = NULL;

/** The device goes with the driver. It did not: unsubscribe() keeps the device
for a couple of seconds on purpose, stopRack() deleted the driver inside that
time, and the stream went on calling back for the life of the process. That
is longer than it sounds -- the playback service keeps the process when the
activity is closed, so opening the app again starts a second Rack beside it.
On a Nothing A024 the orphan kept the phone's one Exclusive endpoint and the
new stream was given Shared: 170.8 ms of measured latency against the usual
handful, a 960-frame callback, and two streams taking turns to tell ADPF
their deadline was 2 ms and 20 ms, 1600 times in three minutes. */
OboeDriver::~OboeDriver() {
	delete device;
	g_portCount.store(0, std::memory_order_relaxed);
	if (g_driver == this)
		g_driver = NULL;
}

void oboeInit() {
	g_driver = new OboeDriver;
	rack::audio::addDriver(OBOE_DRIVER_ID, g_driver);
}


int audioBlockSize() {
	return (g_driver && g_driver->device) ? g_driver->device->blockSize : 0;
}


void audioApplyBlockChoice() {
	int choice = g_blockChoicePending.exchange(-1, std::memory_order_relaxed);
	if (choice < 0)
		return;
	// What Automatic was running before the user fixed a size, to go back to.
	static int autoSizeBeforeFix = 0;
	int was = audioBlockChoice();
	if (was == 0 && choice > 0 && g_driver && g_driver->device)
		autoSizeBeforeFix = g_driver->device->blockSize;
	if (choice != was)
		storeBlockChoice(choice);
	// A fixed size takes effect now. Going back to automatic returns to the
	// size Automatic had chosen: it used to carry on from wherever the fixed
	// size had left the stream, and a tester who tried 1024 and went back to
	// Automatic stayed at 1024 -- 22 ms a block, and a patch that had run at
	// 75% of its deadline now at 92%.
	if (choice > 0 && g_driver && g_driver->device)
		g_driver->device->setBlockSize(choice);
	else if (choice == 0 && was > 0 && autoSizeBeforeFix > 0)
		audioSetBlockSize(autoSizeBeforeFix);
}


bool audioSetBlockSize(int blockSize, bool ladderRaise) {
	if (!g_driver || !g_driver->device)
		return false;
	g_ownBlockChange = true;
	g_ladderRaise = ladderRaise;
	g_driver->device->setBlockSize(blockSize);
	g_ownBlockChange = g_ladderRaise = false;
	return true;
}


int audioMaxUsefulBlockSize() {
	if (!g_driver || !g_driver->device || !g_driver->device->outputStream)
		return 0;
	int capacity = g_driver->device->outputStream->getBufferCapacityInFrames();
	if (capacity <= 0)
		return 0;
	// Never hand the callback more frames than the whole stream can hold. A
	// block bigger than the buffer is not a trade of latency for headroom, it
	// is a guaranteed late callback: the OnePlus 8T reports capacity=1536
	// frames (32 ms), and asking it for 4096-frame (85 ms) blocks produced
	// 225 underruns in 30 s -- every callback due before it could possibly
	// arrive. Largest power of two that fits, then, and never above the 1024
	// the measurements in DEFAULT_BLOCK_SIZE's comment actually cover.
	int block = 64;
	while (block * 2 <= capacity && block < 1024)
		block *= 2;
	return block;
}


void audioReleaseIdleDevice() {
	if (!g_driver || !g_driver->device)
		return;
	if (!g_driver->device->subscribed.empty() || g_driver->idleSince <= 0.0)
		return;
	// Long enough to cover Rack rewriting a port's settings in sequence (each
	// rewrite is an unsubscribe immediately followed by a subscribe), short
	// enough that genuinely deleting the Audio module stops holding the audio
	// device and its callback soon after.
	if (rack::system::getTime() - g_driver->idleSince < 2.0)
		return;
	AUDIO_WARN("Oboe: no ports left; closing the audio device");
	delete g_driver->device;
	g_driver->device = NULL;
	g_driver->idleSince = 0.0;
}


/** Logs the stream's real round-trip latency, once per stream, a few seconds
after it opens. Everything said about latency here until now has been arithmetic
on block sizes; Oboe can ask the stream itself, and a timestamp needs the stream
to have been running a moment to be available. Worth knowing precisely, because
the two terms are not the same kind of problem: the sharing mode is the
device's decision and nothing here can change it, while the block size is ours
and is the larger of the two on every device measured so far. */
/** Walks the stream's buffer back down while nothing is going wrong, and back
up the moment something does.

Oboe's LatencyTuner only ever grows the buffer, and it grows it for any
underrun -- including the burst every patch load produces. Measured on an S22:
47 underruns in the first second after launch took the buffer from 192 frames
to 2976 of a 3072 capacity, and there it stayed for the whole session. Sixty-two
milliseconds of latency, bought in one second of startup turbulence and paid for
hours.

Unlike the block size this costs nothing to change: the buffer is a live
property of a running stream, not a construction parameter, so there is no
reopen and no gap in either direction.

The first version of this leaned on the LatencyTuner to grow the buffer back if
the trim went too far. That was wrong, and the device said so: trimmed to two
bursts, the S22 ran clean for three minutes and then underran twenty-eight
times with the buffer still sitting at 192 -- the tuner had already reached the
capacity ceiling earlier in the session and stopped tuning for good. Taking the
safety net away without putting another one up is not a trade, it is a
regression. So this owns both directions now, and remembers the size that did
not hold so it stops one step above it rather than rediscovering it in a
loop. */
/* How much delay the user is willing to trade for safety. Everything else the
audio side decides is measured; this one cannot be, because it is not a
question about the device. Someone playing a keyboard wants the sound now and
will forgive an occasional click; someone building a patch and listening wants
silence and does not care about a few tens of milliseconds. The engine has no
way to tell which of the two is holding the phone, so it asks once and then
stays out of it.

The setting does not fix a number. It fixes how low the automatic tuning is
allowed to go -- everything else in this file keeps doing its job inside that
band. */
static std::atomic<int> g_latencyMode{1}; // 0 = play, 1 = balanced, 2 = listen

void audioSetLatencyMode(int mode) {
	if (mode < 0 || mode > 2)
		return;
	int was = g_latencyMode.exchange(mode);
	if (was != mode)
		AUDIO_WARN("Oboe: response setting is now %s",
			mode == 0 ? "playing (lowest delay)"
			: mode == 2 ? "listening (safest)" : "balanced");
}

int audioLatencyMode() {
	return g_latencyMode.load(std::memory_order_relaxed);
}

/** The least buffer a stream whose callback is larger than a burst can live
with: one whole callback, rounded up to bursts. 0 when the callback is a burst.

Oboe serves such a stream from an adapter: the device still asks for a burst at
a time, and every so often one of those requests has to wait for the engine to
compute the entire next block. Whatever is buffered then is all there is to
play meanwhile. A Nothing A024 at block 1024 (callback 1056, burst 96) opened
at the tuner's two bursts and was trimmed back to them: seven underruns in the
first 1.4 s of every launch, then one each time the buffer was halved again,
all with "no callback ran late" -- the engine was at 26% of its deadline, and
4 ms of buffer cannot cover 26% of 22 ms. The tuner found 480 frames by itself,
one click at a time, and forgot it at the next launch.
ponytail: a whole callback is the size that cannot underrun this way whatever
the load; scale it by the measured load if the latency at big blocks matters. */
static int32_t callbackFloorFrames(oboe::AudioStream* stream) {
	int32_t burst = stream->getFramesPerBurst();
	int32_t callback = stream->getFramesPerDataCallback();
	if (burst <= 0 || callback <= burst)
		return 0;
	return (callback + burst - 1) / burst * burst;
}

/** Bursts of buffer the trim may not go below, by setting. Two is the hardware
floor -- under that a callback has nowhere to be late at all. */
static int32_t latencyFloorBursts() {
	switch (g_latencyMode.load(std::memory_order_relaxed)) {
		case 0: return 2;
		case 2: return 8;
		default: return 4;
	}
}


void audioTrimBuffer() {
	if (!g_driver || !g_driver->device || !g_driver->device->outputStream)
		return;
	static double quietSince = 0.0;
	static int32_t lastUnderruns = -1;
	// The smallest size seen to underrun, and when. It has to expire: a single
	// underrun can come from anywhere -- a thread change, a patch edit, another
	// app -- and treating one as a permanent verdict locked an 8T's buffer at
	// its full 1536-frame capacity, 32 ms, for the rest of the session. Same
	// lesson the thread tuner's scores had to learn.
	static int32_t tooSmall = 0;
	static double tooSmallAt = 0.0;
	static const double TOO_SMALL_TTL = 300.0;

	auto* stream = g_driver->device->outputStream.get();
	int32_t burst = stream->getFramesPerBurst();
	if (burst <= 0)
		return;
	int32_t size = stream->getBufferSizeInFrames();
	int32_t underruns = g_totalUnderruns.load(std::memory_order_relaxed);
	double now = rack::system::getTime();
	float rate = (float) g_driver->device->getSampleRate();

	// What the buffer has to hold, worked out before anything is heard. A
	// callback that takes T to return leaves the device playing from the
	// buffer for T, so a buffer shorter than the longest callback of the last
	// minute has already been outrun once and only luck says when it clicks.
	// Until now the size was found from below: trimmed every quiet twenty
	// seconds until it underran, then put back -- "underran at a 384-frame
	// buffer; back up to 768" on a Nothing A024 whose callbacks had been
	// reaching 12 ms, 570 frames, the whole time. Each of those was a click
	// spent to learn a number the callback had been reporting all along.
	// The margin is the user's own setting: how much delay for how much safety.
	// Remembered in ten-second slots, for as long as the setting says: one
	// minute when playing, five when balanced, fifteen when listening. It was
	// one minute for everybody, and a minute is how long a hand stays off the
	// screen: on a Nothing A024 the buffer was walked back down to 384 frames
	// during a quiet spell and the next pinch -- a 9 ms callback -- underran
	// it. What stalls a callback comes back; the buffer should still be there.
	static const int PEAK_SLOTS = 90;
	static int64_t peakBySlot[PEAK_SLOTS];
	static int peakSlot = 0;
	static double peakSlotAt = 0.0;
	if (rackdroid::audioSecondsSinceStreamOpen() < 1.0) {
		std::fill(std::begin(peakBySlot), std::end(peakBySlot), (int64_t) 0);
		g_callbackPeakNanos.store(0, std::memory_order_relaxed);
	}
	if (now - peakSlotAt >= 10.0) {
		peakSlotAt = now;
		peakSlot = (peakSlot + 1) % PEAK_SLOTS;
		peakBySlot[peakSlot] = 0;
	}
	int64_t peakNow = g_callbackPeakNanos.exchange(0, std::memory_order_relaxed);
	if (peakNow > peakBySlot[peakSlot])
		peakBySlot[peakSlot] = peakNow;
	int mode = g_latencyMode.load(std::memory_order_relaxed);
	int slotsBack = (mode == 0) ? 6 : (mode == 2) ? PEAK_SLOTS : 30;
	int64_t peakNs = 0;
	for (int i = 0; i < slotsBack; i++)
		peakNs = std::max(peakNs, peakBySlot[(peakSlot - i + PEAK_SLOTS) % PEAK_SLOTS]);
	float margin = (mode == 0) ? 1.5f : (mode == 2) ? 3.f : 2.f;
	int32_t neededFrames = 0;
	if (rate > 0.f && peakNs > 0) {
		neededFrames = (int32_t) (peakNs * 1e-9 * rate * margin);
		neededFrames = (neededFrames + burst - 1) / burst * burst;
		int32_t most = stream->getBufferCapacityInFrames() - burst;
		if (neededFrames > most)
			neededFrames = most;
	}
	if (neededFrames > size && rackdroid::audioSecondsSinceStreamOpen() >= 2.0) {
		auto grown = stream->setBufferSizeInFrames(neededFrames);
		if (grown && grown.value() > size) {
			AUDIO_WARN("Oboe: callbacks have taken up to %.1f ms; buffer %d -> %d "
				"frames before that is heard", peakNs * 1e-6, size, grown.value());
			size = grown.value();
			quietSince = now;
		}
	}

	if (underruns != lastUnderruns) {
		bool first = lastUnderruns < 0;
		int32_t added = first ? 0 : underruns - lastUnderruns;
		lastUnderruns = underruns;
		quietSince = now;

		// One underrun is not evidence that the buffer is too small. On a
		// device denied the fast audio path they arrive in ones and twos even
		// when everything is fine -- an 8T does it about once a minute -- and
		// treating each one as a verdict pinned its buffer at the full
		// capacity within half a minute of every launch, twice over. Count
		// them instead, and only act on a cluster. Same rule the thread tuner
		// uses on its own windows.
		static int32_t burstCount = 0;
		static double burstSince = 0.0;
		if (now - burstSince > 10.0) {
			burstSince = now;
			burstCount = 0;
		}
		burstCount += added;
		if (burstCount < 4)
			return;
		burstCount = 0;
		// Something went wrong at this size. Give the room back at once --
		// this is the half that used to be missing -- and do not come below
		// one step above it again.
		// Thirty seconds, not five. A stream that has just opened underruns
		// while the patch loads and the thread tuner finds its feet, and on an
		// 8T that turbulence outlasted a five-second guard: the growth path
		// fired twenty seconds in and pinned the buffer at the full capacity
		// before the engine had settled at all.
		if (!first && rackdroid::audioSecondsSinceStreamOpen() > 30.0) {
			int32_t want = size * 2;
			int32_t cap = stream->getBufferCapacityInFrames();
			if (want > cap)
				want = cap;
			// The ask can be granted and still change nothing: the stream caps
			// the buffer a burst below its capacity, so once size is already
			// at that ceiling setBufferSizeInFrames() returns the size it
			// already had. That counts as success, and the old code took it as
			// one -- reprinting "back up to 2976 and no lower from here"
			// fifteen times in a hundred milliseconds on a device that was
			// underrunning continuously at its ceiling. Only a size that
			// actually moved is news; the ceiling is worth saying once.
			static int32_t capacityLoggedAt = 0;
			if (want > size) {
				auto grown = stream->setBufferSizeInFrames(want);
				if (grown && grown.value() > size) {
					tooSmall = size;
					tooSmallAt = now;
					capacityLoggedAt = 0;
					AUDIO_WARN("Oboe: underran at a %d-frame buffer; back up to "
						"%d and no lower from here", size, grown.value());
					return;
				}
			}
			if (capacityLoggedAt != size) {
				capacityLoggedAt = size;
				AUDIO_WARN("Oboe: still underrunning at %d frames with the buffer "
					"already at this device's %d-frame ceiling -- there is no "
					"more room left to give it here", size,
					stream->getBufferCapacityInFrames());
			}
		}
		return;
	}

	if (quietSince <= 0.0) {
		quietSince = now;
		return;
	}
	if (now - quietSince < 20.0)
		return;

	// A device that keeps growing the buffer back from a size is telling us
	// that size does not suit it, one underrun at a time -- too few at a time
	// to trip the cluster rule above, but plainly and repeatedly. Trimming to
	// it again after each is not persistence, it is not listening: an 8T sat
	// at 384 frames and had Oboe's tuner regrow it twice in three minutes,
	// and this undid both. After a third time, believe it and settle a step
	// higher.
	static int32_t lastTrimmedTo = 0;
	static int regrowths = 0;
	static int32_t learnedFloor = 0;
	static int32_t observedRegrowth = 0;
	if (lastTrimmedTo > 0 && size > lastTrimmedTo) {
		regrowths++;
		// Settle at the size the device itself grew to, not at double the one
		// that failed. Oboe's tuner picks what it actually needs; doubling
		// overshoots it -- on an 8T that meant 1536 frames where 1152 was
		// enough, eight milliseconds thrown away for nothing.
		if (size > observedRegrowth)
			observedRegrowth = size;
		if (regrowths >= 3) {
			learnedFloor = observedRegrowth;
			regrowths = 0;
			AUDIO_WARN("Oboe: the buffer keeps being grown back from %d frames; "
				"settling at the %d this device asks for", lastTrimmedTo,
				learnedFloor);
		}
	}

	// Two bursts is the hard floor -- below that a callback has nowhere to be
	// late -- and one step above whatever already failed is the learned one.
	int32_t floorFrames = burst * latencyFloorBursts();
	if (callbackFloorFrames(stream) > floorFrames)
		floorFrames = callbackFloorFrames(stream);
	if (learnedFloor > floorFrames)
		floorFrames = learnedFloor;
	if (tooSmall > 0 && now - tooSmallAt > TOO_SMALL_TTL)
		tooSmall = 0; // old news; worth asking again
	if (tooSmall > 0 && tooSmall * 2 > floorFrames)
		floorFrames = tooSmall * 2;
	// And never below what the callbacks of the last minute have needed.
	if (neededFrames > floorFrames)
		floorFrames = neededFrames;
	if (size <= floorFrames) {
		// Already where it belongs. Record that and restart the clock, or the
		// regrowth counter above keeps climbing every frame and re-announces
		// the same conclusion a few times a second -- which it did, four times
		// in a hundred and fifty milliseconds.
		lastTrimmedTo = size;
		quietSince = now;
		return;
	}
	int32_t want = size / 2;
	if (want < floorFrames)
		want = floorFrames;
	auto result = stream->setBufferSizeInFrames(want);
	if (!result)
		return;
	AUDIO_WARN("Oboe: quiet for %.0fs; buffer %d -> %d frames (%.1f ms less "
		"latency, no gap)", now - quietSince, size, result.value(),
		rate > 0.f ? (size - result.value()) / rate * 1000.f : 0.f);
	lastTrimmedTo = result.value();
	quietSince = now;
}


void audioReportLatency() {
	if (!g_driver || !g_driver->device || !g_driver->device->outputStream)
		return;
	static double reportedFor = -1.0;
	if (g_lastStreamOpen <= 0.0 || reportedFor == g_lastStreamOpen)
		return;
	if (rack::system::getTime() - g_lastStreamOpen < 4.0)
		return; // no timestamps yet
	auto result = g_driver->device->outputStream->calculateLatencyMillis();
	if (!result) {
		reportedFor = g_lastStreamOpen; // do not ask again for this stream
		AUDIO_WARN("Oboe: the stream will not report its latency (%s)",
			oboe::convertToText(result.error()));
		return;
	}
	// A stream that has only just started can hand back a timestamp pair that
	// makes no sense -- a fresh install measured minus three point nine
	// SECONDS -- and Oboe reports that as a success, so the result being "ok"
	// is not enough. Anything outside what an audio device could plausibly do
	// is not a measurement; wait and ask again rather than writing a number
	// into the log that will send somebody the wrong way. Same lesson as the
	// "-1% of the phone's busy CPU" line that shipped yesterday.
	if (result.value() <= 0.0 || result.value() > 2000.0)
		return;
	reportedFor = g_lastStreamOpen;
	int block = g_driver->device->getBlockSize();
	float rate = g_driver->device->getSampleRates().empty()
		? 48000.f : (float) g_driver->device->getSampleRate();
	// Two separate terms, not one inside the other: the stream's own latency
	// is what Oboe measures, and the engine's block is time spent before a
	// sample ever reaches it. Saying "of which" here was wrong, and on this
	// hardware obviously so -- the block alone came out larger than the whole
	// stream latency it was supposed to be part of.
	AUDIO_WARN("Oboe: measured stream latency %.1f ms; the engine's %d-frame "
		"block adds %.1f ms on top of it",
		result.value(), block, rate > 0.f ? block / rate * 1000.f : 0.f);
}


bool audioIsRecording() {
	return gRecorder.active.load();
}


bool audioIsSharedMode() {
	if (!g_driver || !g_driver->device || !g_driver->device->outputStream)
		return false;
	return g_driver->device->outputStream->getSharingMode() != oboe::SharingMode::Exclusive;
}


// ---- JNI: master recording toggle (MainActivity's ⏺ button) ----

/** Whether anything is being played at all. The recorder is fed by the audio
callback, and a patch with no Audio module has no stream and no callback: the
button then "recorded" for as long as it was held on and saved a 44-byte file,
a header and nothing else, with a toast saying where. Found on a OnePlus 8T
after File > New. */
/** For the load meter on the toolbar; 0 with no patch playing. */
extern "C" JNIEXPORT jint JNICALL
Java_org_rackdroid_MainActivity_nativeEngineLoad(JNIEnv*, jobject) {
	if (g_portCount.load(std::memory_order_relaxed) <= 0)
		return 0;
	return g_loadForMeter.load(std::memory_order_relaxed);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_rackdroid_MainActivity_nativeHasAudioOutput(JNIEnv*, jobject) {
	return g_portCount.load(std::memory_order_relaxed) > 0;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_rackdroid_MainActivity_nativeRecordStart(JNIEnv* env, jobject thiz, jstring jPath) {
	const char* chars = env->GetStringUTFChars(jPath, NULL);
	std::string path = chars ? chars : "";
	env->ReleaseStringUTFChars(jPath, chars);
	// The stream's actual rate lives in the device; 48k is the default and
	// the only rate the port opens in practice unless the user changed it.
	int sr = DEFAULT_SAMPLE_RATE;
	if (APP && APP->engine)
		sr = (int) APP->engine->getSampleRate();
	bool ok = gRecorder.start(path, sr, NUM_OUTPUTS);
	AUDIO_WARN("record start %s: %d", path.c_str(), ok);
	return ok;
}

/** What the Audio block picker needs, in one call: sample rate, the choice (0
automatic), the callback the choice produces, the block in use now, then one
(block, callback frames) pair per size worth offering. Two sizes that reach
this device as the same callback are one option -- the larger, which does the
same work in fewer steps -- and nothing above what the stream's buffer holds. */
extern "C" JNIEXPORT jintArray JNICALL
Java_org_rackdroid_MainActivity_nativeGetBlockInfo(JNIEnv* env, jobject) {
	std::vector<jint> out;
	int rate = 48000;
	if (g_driver && g_driver->device && g_driver->device->getSampleRate() > 0.f)
		rate = (int) g_driver->device->getSampleRate();
	int choice = rackdroid::audioBlockChoice();
	auto aligned = [](int bs) {
		int a = rackdroid::audioAlignedCallbackFrames(bs);
		return a > 0 ? a : bs;
	};
	out.push_back(rate);
	out.push_back(choice);
	out.push_back(choice > 0 ? aligned(choice) : 0);
	out.push_back(rackdroid::audioBlockSize());
	int cap = rackdroid::audioMaxUsefulBlockSize();
	for (int bs = 64; bs <= 1024; bs *= 2) {
		if (cap > 0 && bs > cap)
			break;
		int frames = aligned(bs);
		if (out.size() > 4 && out.back() == frames) {
			out[out.size() - 2] = bs;
			continue;
		}
		out.push_back(bs);
		out.push_back(frames);
	}
	jintArray arr = env->NewIntArray((jsize) out.size());
	env->SetIntArrayRegion(arr, 0, (jsize) out.size(), out.data());
	return arr;
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeSetBlockChoice(JNIEnv*, jobject, jint choice) {
	rackdroid::audioRequestBlockChoice((int) choice);
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeSetLatencyMode(JNIEnv*, jobject, jint mode) {
	rackdroid::audioSetLatencyMode((int) mode);
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeRecordStop(JNIEnv* env, jobject thiz) {
	gRecorder.stop();
	AUDIO_WARN("record stop");
}


} // namespace rackdroid
