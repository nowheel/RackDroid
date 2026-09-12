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
#include <system.hpp>
#include <context.hpp>
#include <engine/Engine.hpp>
#include <common.hpp>


// ---- Master WAV recorder ------------------------------------------------
// The audio callback taps the final output buffer into a single-producer/
// single-consumer ring; a writer thread drains it to a 16-bit PCM WAV.
// File I/O never happens on the audio thread; on ring overflow (writer
// stalled) frames are dropped rather than blocking the callback.

namespace {

struct WavRecorder {
	static const size_t RING_FLOATS = 1 << 20; // ~5.5s stereo @48k
	std::vector<float> ring;
	std::atomic<size_t> head{0}; // producer (audio thread)
	std::atomic<size_t> tail{0}; // consumer (writer thread)
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


struct OboeDevice : rack::audio::Device, oboe::AudioStreamDataCallback, oboe::AudioStreamErrorCallback {
	std::shared_ptr<oboe::AudioStream> outputStream;
	std::shared_ptr<oboe::AudioStream> inputStream;
	float sampleRate = DEFAULT_SAMPLE_RATE;
	int blockSize = DEFAULT_BLOCK_SIZE;
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
		openStreams();
	}

	~OboeDevice() override {
		closeStreams();
	}

	void openStreams() {
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
		// blockSize belongs in here: it is the one number in this line the app
		// itself chooses, and leaving it out cost a debugging round trip --
		// a log full of underruns with no way to tell which rung of
		// checkBlockSizeOverload()'s ladder was in effect at the time.
		AUDIO_WARN("Oboe: stream started, sampleRate=%g block=%d burst=%d buffer=%d "
			"capacity=%d sharing=%s performance=%s api=%s",
			sampleRate, blockSize, outputStream->getFramesPerBurst(),
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
		closeStreams();
		sampleRate = sr;
		openStreams();
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
		closeStreams();
		blockSize = bs;
		openStreams();
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
			if (xruns > lastXRuns)
				g_totalUnderruns.fetch_add(xruns - lastXRuns, std::memory_order_relaxed);
			lastXRuns = xruns;
			// Underruns while the tuner has already grown the buffer as far as
			// it goes: the device is not jittering, it is short of CPU, and no
			// buffer will fix that. Publish it so the engine can answer.
			if (stream->getBufferSizeInFrames() >=
				stream->getBufferCapacityInFrames() - stream->getFramesPerBurst())
				g_ceilingUnderruns.fetch_add(1, std::memory_order_relaxed);
			AUDIO_WARN("Oboe: %d underruns, buffer now %d frames of %d",
				xruns, stream->getBufferSizeInFrames(),
				stream->getBufferCapacityInFrames());
		}

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

		return oboe::DataCallbackResult::Continue;
	}

	// oboe::AudioStreamErrorCallback

	void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override {
		// Device disconnected (headphones unplugged, route change): reopen.
		WARN("Oboe: stream error %s, reopening", oboe::convertToText(error));
		closeStreams();
		openStreams();
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

	rack::audio::Device* subscribe(int deviceId, rack::audio::Port* port) override {
		if (deviceId != 0)
			return NULL;
		if (!device)
			device = new OboeDevice;
		device->subscribe(port);
		return device;
	}

	void unsubscribe(int deviceId, rack::audio::Port* port) override {
		if (deviceId != 0 || !device)
			return;
		device->unsubscribe(port);
		if (device->subscribed.empty()) {
			delete device;
			device = NULL;
		}
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
	__android_log_print(ANDROID_LOG_INFO, "rackdroid", "record start %s: %d", path.c_str(), ok);
	return ok;
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeRecordStop(JNIEnv* env, jobject thiz) {
	gRecorder.stop();
	__android_log_print(ANDROID_LOG_INFO, "rackdroid", "record stop");
}


} // namespace rackdroid
