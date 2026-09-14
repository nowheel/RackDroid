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

#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <cstdio>
#include <cstring>
#include <cmath>

#include <jni.h>
#include <android/log.h>

#include <unistd.h>
#include <ctime>

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
		if (n > freeSpace)
			n = freeSpace;
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

/** When the last underrun happened, as rack::system::getTime(). */
static std::atomic<double> g_lastUnderrunAt{0.0};

bool audioUnderrunsRecently() {
	double at = g_lastUnderrunAt.load(std::memory_order_relaxed);
	if (at <= 0.0)
		return false;
	return rack::system::getTime() - at < 2.0;
}

/** The audio callback thread, as gettid() -- readable only from inside the
callback, so it is published from there the first time it runs. Zero until
then. ADPF wants it at the head of its thread list. */
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
failed was this size on that workload, not for ever. */
static int g_driverBlockSize = 0;
static int g_knownTooSmall = 0;

int audioKnownTooSmallBlock() {
	return g_knownTooSmall;
}

void audioNoteBlockTooSmall(int bs) {
	g_knownTooSmall = bs;
	if (g_driverBlockSize > 0) {
		FILE* f = std::fopen(blockSizeMemoPath().c_str(), "w");
		if (f) {
			std::fprintf(f, "%d %d\n", g_driverBlockSize, bs);
			std::fclose(f);
		}
	}
}

static int rememberedBlockSize() {
	FILE* f = std::fopen(blockSizeMemoPath().c_str(), "r");
	if (!f)
		return DEFAULT_BLOCK_SIZE;
	int v = 0, tooSmall = 0;
	int n = std::fscanf(f, "%d %d", &v, &tooSmall);
	std::fclose(f);
	if (n >= 2 && tooSmall >= 64 && tooSmall <= 4096 && (tooSmall & (tooSmall - 1)) == 0)
		g_knownTooSmall = tooSmall;
	// Only sizes Rack itself offers; anything else is a stale or corrupt file.
	if (n < 1 || v < 64 || v > 4096 || (v & (v - 1)) != 0)
		return DEFAULT_BLOCK_SIZE;
	return v;
}

static void rememberBlockSize(int bs) {
	FILE* f = std::fopen(blockSizeMemoPath().c_str(), "w");
	if (!f)
		return;
	std::fprintf(f, "%d %d\n", bs, g_knownTooSmall);
	std::fclose(f);
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
		double t0 = rack::system::getTime();
		std::lock_guard<std::mutex> lock(streamMutex);

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
			->setFramesPerDataCallback(blockSize)
			->setDataCallback(this)
			->setErrorCallback(this);

		oboe::Result result = outBuilder.openStream(outputStream);
		if (result != oboe::Result::OK) {
			WARN("Oboe: could not open output stream: %s", oboe::convertToText(result));
			return;
		}
		// The stream may have been opened with a different rate than requested.
		sampleRate = outputStream->getSampleRate();

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
		AUDIO_WARN("Oboe: stream started (%s), sampleRate=%g block=%d burst=%d buffer=%d "
			"capacity=%d sharing=%s performance=%s api=%s",
			why, sampleRate, blockSize, outputStream->getFramesPerBurst(),
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
		if (bs == blockSize)
			return;
		AUDIO_WARN("Oboe: block size %d -> %d", blockSize, bs);
		closeStreams();
		blockSize = bs;
		g_driverBlockSize = bs;
		rememberBlockSize(bs);
		openStreams("block size changed");
	}

	// oboe::AudioStreamDataCallback

	oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream, void* audioData, int32_t numFrames) override {
		float* output = (float*) audioData;

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
		// and it is the only place its id can be read. Published once.
		if (g_audioThreadTid.load(std::memory_order_relaxed) == 0)
			g_audioThreadTid.store(gettid(), std::memory_order_relaxed);

		// Bracket the work ADPF is asked to make fit. CLOCK_MONOTONIC because
		// that is what the API documents its durations against. Skipped
		// entirely where no session exists -- most devices, as it turns out --
		// so a refused session costs the callback nothing at all.
		bool timing = adpfActive();
		timespec t0;
		if (timing)
			clock_gettime(CLOCK_MONOTONIC, &t0);

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
		gRecorder.push(output, (size_t) numFrames * stream->getChannelCount());

		if (timing) {
			timespec t1;
			clock_gettime(CLOCK_MONOTONIC, &t1);
			adpfReportNanos((int64_t) (t1.tv_sec - t0.tv_sec) * 1000000000LL
				+ (t1.tv_nsec - t0.tv_nsec));
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
		device->subscribe(port);
		AUDIO_WARN("Oboe: port subscribed (%d now)", (int) device->subscribed.size());
		return device;
	}

	void unsubscribe(int deviceId, rack::audio::Port* port) override {
		if (deviceId != 0 || !device)
			return;
		device->unsubscribe(port);
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

void oboeInit() {
	g_driver = new OboeDriver;
	rack::audio::addDriver(OBOE_DRIVER_ID, g_driver);
}


int audioBlockSize() {
	return (g_driver && g_driver->device) ? g_driver->device->blockSize : 0;
}


bool audioSetBlockSize(int blockSize) {
	if (!g_driver || !g_driver->device)
		return false;
	g_driver->device->setBlockSize(blockSize);
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
			if (want > size) {
				auto grown = stream->setBufferSizeInFrames(want);
				if (grown) {
					tooSmall = size;
					tooSmallAt = now;
					AUDIO_WARN("Oboe: underran at a %d-frame buffer; back up to "
						"%d and no lower from here", size, grown.value());
				}
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

	// Two bursts is the hard floor -- below that a callback has nowhere to be
	// late -- and one step above whatever already failed is the learned one.
	int32_t floorFrames = burst * 2;
	if (tooSmall > 0 && now - tooSmallAt > TOO_SMALL_TTL)
		tooSmall = 0; // old news; worth asking again
	if (tooSmall > 0 && tooSmall * 2 > floorFrames)
		floorFrames = tooSmall * 2;
	if (size <= floorFrames)
		return;
	int32_t want = size / 2;
	if (want < floorFrames)
		want = floorFrames;
	auto result = stream->setBufferSizeInFrames(want);
	if (!result)
		return;
	AUDIO_WARN("Oboe: quiet for %.0fs; buffer %d -> %d frames (%.1f ms less "
		"latency, no gap)", now - quietSince, size, result.value(),
		rate > 0.f ? (size - result.value()) / rate * 1000.f : 0.f);
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

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeRecordStop(JNIEnv* env, jobject thiz) {
	gRecorder.stop();
	AUDIO_WARN("record stop");
}


} // namespace rackdroid
