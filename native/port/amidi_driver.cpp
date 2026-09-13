/* rack::midi driver on the NDK AMidi API (replaces src/rtmidi.cpp).
 *
 * Device lifecycle: MainActivity opens MidiManager devices on the Java side
 * and hands each android.media.midi.MidiDevice here through the JNI
 * callbacks below; AMidiDevice_fromJava converts it for NDK use. The
 * registry survives driver re-creation and hotplug.
 *
 * Terminology (confusing on purpose, thanks MIDI):
 *  - a device's OUTPUT port produces MIDI for us -> Rack INPUT device
 *  - a device's INPUT port consumes MIDI from us -> Rack OUTPUT device
 *
 * Input runs a 1 kHz polling thread per subscribed device (AMidi has no
 * blocking receive); messages are parsed with running status and forwarded
 * to InputDevice::onMessage, which is thread-safe by design in Rack.
 */
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <amidi/AMidi.h>
#include <android/log.h>
#include <jni.h>

#include <midi.hpp>
#include <common.hpp>
#include <logger.hpp>

#include "amidi_driver.hpp"

/* Both sinks, like the rest of the port layer. These used to reach logcat
only, which means a user reporting "my keyboard is not detected" sends a log
saying nothing whatever about MIDI -- the one file that could have answered it.
Neither of these is in the 1 kHz polling path: they fire on hotplug and on a
device that failed to open, so Rack's WARN and its fflush cost nothing here. */
#define LOGI(...) do { __android_log_print(ANDROID_LOG_INFO, "rackdroid", __VA_ARGS__); INFO(__VA_ARGS__); } while (0)
#define LOGE(...) do { __android_log_print(ANDROID_LOG_ERROR, "rackdroid", __VA_ARGS__); WARN(__VA_ARGS__); } while (0)


namespace rackdroid {


struct AmidiDeviceEntry {
	AMidiDevice* device = NULL;
	std::string name;
	int inputPortCount = 0;  // ports that consume MIDI (Rack outputs)
	int outputPortCount = 0; // ports that produce MIDI (Rack inputs)
};

static std::mutex registryMutex;
static std::map<int, AmidiDeviceEntry> registry;


// ---- Rack input device (device output port -> Rack) ----

struct AmidiInputDevice : rack::midi::InputDevice {
	std::string name;
	AMidiOutputPort* port = NULL;
	std::thread readerThread;
	std::atomic<bool> running{false};

	AmidiInputDevice(const AmidiDeviceEntry& entry) {
		name = entry.name;
		media_status_t status = AMidiOutputPort_open(entry.device, 0, &port);
		if (status != AMEDIA_OK || !port)
			throw rack::Exception("Could not open MIDI output port of %s (%d)", name.c_str(), status);
		running = true;
		readerThread = std::thread([this] { run(); });
	}

	~AmidiInputDevice() override {
		running = false;
		if (readerThread.joinable())
			readerThread.join();
		if (port)
			AMidiOutputPort_close(port);
	}

	std::string getName() override {
		return name;
	}

	void run() {
		uint8_t buffer[512];
		// Running status state for the byte-stream parser
		uint8_t runningStatus = 0;

		while (running) {
			int32_t opcode;
			size_t numBytes = 0;
			int64_t timestamp;
			ssize_t n = AMidiOutputPort_receive(port, &opcode, buffer, sizeof(buffer), &numBytes, &timestamp);
			if (n < 0)
				break; // port closed / device gone
			if (n == 0) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}
			if (opcode != AMIDI_OPCODE_DATA || numBytes == 0)
				continue;
			parse(buffer, numBytes, runningStatus);
		}
	}

	static int dataLength(uint8_t status) {
		switch (status & 0xf0) {
			case 0x80: case 0x90: case 0xa0: case 0xb0: case 0xe0: return 2;
			case 0xc0: case 0xd0: return 1;
		}
		switch (status) {
			case 0xf1: case 0xf3: return 1;
			case 0xf2: return 2;
			default: return 0; // realtime + others
		}
	}

	void parse(const uint8_t* data, size_t len, uint8_t& runningStatus) {
		size_t i = 0;
		while (i < len) {
			uint8_t b = data[i];
			uint8_t status;
			if (b & 0x80) {
				status = b;
				i++;
				if (status == 0xf0) {
					// Skip SysEx (not used by Rack modules)
					while (i < len && data[i] != 0xf7)
						i++;
					if (i < len)
						i++;
					continue;
				}
				if (status < 0xf8)
					runningStatus = (status < 0xf0) ? status : 0;
			}
			else {
				if (!runningStatus) {
					i++;
					continue;
				}
				status = runningStatus;
			}

			int dlen = dataLength(status);
			if ((int) (len - i) < dlen)
				break; // truncated; drop remainder

			rack::midi::Message msg;
			msg.setSize(1 + dlen);
			msg.bytes[0] = status;
			for (int d = 0; d < dlen; d++)
				msg.bytes[1 + d] = data[i + d] & 0x7f;
			i += dlen;
			onMessage(msg);
		}
	}
};


// ---- Rack output device (Rack -> device input port) ----

struct AmidiOutputDevice : rack::midi::OutputDevice {
	std::string name;
	AMidiInputPort* port = NULL;

	AmidiOutputDevice(const AmidiDeviceEntry& entry) {
		name = entry.name;
		media_status_t status = AMidiInputPort_open(entry.device, 0, &port);
		if (status != AMEDIA_OK || !port)
			throw rack::Exception("Could not open MIDI input port of %s (%d)", name.c_str(), status);
	}

	~AmidiOutputDevice() override {
		if (port)
			AMidiInputPort_close(port);
	}

	std::string getName() override {
		return name;
	}

	void sendMessage(const rack::midi::Message& message) override {
		AMidiInputPort_send(port, message.bytes.data(), message.bytes.size());
	}
};


// ---- Driver ----

struct AmidiDriver : rack::midi::Driver {
	std::map<int, AmidiInputDevice*> inputDevices;
	std::map<int, AmidiOutputDevice*> outputDevices;

	std::string getName() override {
		return "Android MIDI";
	}

	std::vector<int> getInputDeviceIds() override {
		std::lock_guard<std::mutex> lock(registryMutex);
		std::vector<int> ids;
		for (auto& pair : registry) {
			if (pair.second.outputPortCount > 0)
				ids.push_back(pair.first);
		}
		return ids;
	}

	int getDefaultInputDeviceId() override {
		std::vector<int> ids = getInputDeviceIds();
		return ids.empty() ? -1 : ids[0];
	}

	std::string getInputDeviceName(int deviceId) override {
		std::lock_guard<std::mutex> lock(registryMutex);
		auto it = registry.find(deviceId);
		return (it != registry.end()) ? it->second.name : "";
	}

	rack::midi::InputDevice* subscribeInput(int deviceId, rack::midi::Input* input) override {
		AmidiInputDevice* device = NULL;
		auto it = inputDevices.find(deviceId);
		if (it != inputDevices.end()) {
			device = it->second;
		}
		else {
			std::lock_guard<std::mutex> lock(registryMutex);
			auto rit = registry.find(deviceId);
			if (rit == registry.end())
				return NULL;
			try {
				device = new AmidiInputDevice(rit->second);
			}
			catch (rack::Exception& e) {
				LOGE("%s", e.what());
				return NULL;
			}
			inputDevices[deviceId] = device;
		}
		device->subscribe(input);
		return device;
	}

	void unsubscribeInput(int deviceId, rack::midi::Input* input) override {
		auto it = inputDevices.find(deviceId);
		if (it == inputDevices.end())
			return;
		it->second->unsubscribe(input);
		if (it->second->subscribed.empty()) {
			delete it->second;
			inputDevices.erase(it);
		}
	}

	std::vector<int> getOutputDeviceIds() override {
		std::lock_guard<std::mutex> lock(registryMutex);
		std::vector<int> ids;
		for (auto& pair : registry) {
			if (pair.second.inputPortCount > 0)
				ids.push_back(pair.first);
		}
		return ids;
	}

	int getDefaultOutputDeviceId() override {
		std::vector<int> ids = getOutputDeviceIds();
		return ids.empty() ? -1 : ids[0];
	}

	std::string getOutputDeviceName(int deviceId) override {
		return getInputDeviceName(deviceId);
	}

	rack::midi::OutputDevice* subscribeOutput(int deviceId, rack::midi::Output* output) override {
		AmidiOutputDevice* device = NULL;
		auto it = outputDevices.find(deviceId);
		if (it != outputDevices.end()) {
			device = it->second;
		}
		else {
			std::lock_guard<std::mutex> lock(registryMutex);
			auto rit = registry.find(deviceId);
			if (rit == registry.end())
				return NULL;
			try {
				device = new AmidiOutputDevice(rit->second);
			}
			catch (rack::Exception& e) {
				LOGE("%s", e.what());
				return NULL;
			}
			outputDevices[deviceId] = device;
		}
		device->subscribe(output);
		return device;
	}

	void unsubscribeOutput(int deviceId, rack::midi::Output* output) override {
		auto it = outputDevices.find(deviceId);
		if (it == outputDevices.end())
			return;
		it->second->unsubscribe(output);
		if (it->second->subscribed.empty()) {
			delete it->second;
			outputDevices.erase(it);
		}
	}
};


void amidiInit() {
	rack::midi::addDriver(AMIDI_DRIVER_ID, new AmidiDriver);
}


// ---- JNI callbacks from MainActivity ----

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeMidiDeviceAdded(JNIEnv* env, jobject thiz,
		jint id, jstring jName, jobject jDevice, jint inputPorts, jint outputPorts) {
	AMidiDevice* device = NULL;
	media_status_t status = AMidiDevice_fromJava(env, jDevice, &device);
	if (status != AMEDIA_OK || !device) {
		LOGE("AMidiDevice_fromJava failed (%d)", status);
		return;
	}
	const char* nameChars = env->GetStringUTFChars(jName, NULL);
	AmidiDeviceEntry entry;
	entry.device = device;
	entry.name = nameChars ? nameChars : "MIDI device";
	entry.inputPortCount = inputPorts;
	entry.outputPortCount = outputPorts;
	env->ReleaseStringUTFChars(jName, nameChars);

	std::lock_guard<std::mutex> lock(registryMutex);
	registry[id] = entry;
	LOGI("MIDI device added: %s (in %d, out %d)", entry.name.c_str(), inputPorts, outputPorts);
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeMidiDeviceRemoved(JNIEnv* env, jobject thiz, jint id) {
	std::lock_guard<std::mutex> lock(registryMutex);
	auto it = registry.find(id);
	if (it == registry.end())
		return;
	LOGI("MIDI device removed: %s", it->second.name.c_str());
	// Note: AMidiDevice_release would invalidate open ports; subscribed
	// reader threads detect closure via receive() errors. Release here since
	// the Java device was closed anyway.
	AMidiDevice_release(it->second.device);
	registry.erase(it);
}


} // namespace rackdroid
