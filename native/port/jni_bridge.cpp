/* JNI bridge to MainActivity for clipboard and dialogs.
 *
 * All entry points may be called from the native glue thread (or Rack's
 * threads for the clipboard): the thread is attached to the VM on demand.
 *
 * Dialogs keep osdialog's synchronous contract WITHOUT blocking the glue
 * thread's event processing: the Java side shows the dialog and returns
 * immediately; the native caller then pumps the glue looper (lifecycle
 * cmds keep flowing, input is dropped by handleInput while pumping — see
 * dialogIsPumping) until the result arrives via nativeDialogInt/String.
 * A plain latch-blocked wait here left NativeActivity's input queue
 * unread, which Android reports as an ANR after a few seconds of touches.
 */
#include <atomic>
#include <thread>
#include <chrono>

#include <android/native_activity.h>
#include <android/log.h>
#include <android/looper.h>
#include <jni.h>

#include <system.hpp>

#include "jni_bridge.hpp"

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "rackdroid", __VA_ARGS__)


namespace rackdroid {


static JavaVM* vm = NULL;
static jobject activityObj = NULL; // global ref
static jclass activityCls = NULL;  // global ref
static jclass stringCls = NULL;    // global ref (java/lang/String)
static jmethodID midClipboardSet;
static jmethodID midClipboardGet;
static jmethodID midDialogMessage;
static jmethodID midDialogPrompt;
static jmethodID midDialogFile;
static jmethodID midMenuShow;
static jmethodID midMenuDismiss;
static jmethodID midBrowserShow;
static jmethodID midSharePatch;
static jmethodID midShowHelp;
static jmethodID midShowLatencyPicker;
static jmethodID midLoadUserPlugins;
static jmethodID midPatchReady;
static jmethodID midLanguageChanged;
static jmethodID midThermalStatus;
static jmethodID midShowToast;
static jmethodID midShowEngineNotice;
static jmethodID midRequestAudioFocus;

// ---- dialog result handoff (UI thread -> pumping glue thread) ----
static void (*pumpOnce)(int timeoutMs) = NULL; // installed by main_android
static std::atomic<bool> dialogDone{false};
static std::atomic<bool> userPluginsDone{false};
static std::atomic<bool> pumping{false};
static std::atomic<bool> safeStartup{false};
static std::atomic<bool> skipStartupUserPlugins{false};
static int dialogResultInt = 0;
static bool dialogResultHasStr = false;
static std::string dialogResultStr;


void jniSetPump(void (*pump)(int timeoutMs)) {
	pumpOnce = pump;
}


bool dialogIsPumping() {
	return pumping.load();
}


void setStartupOptions(bool safeMode, bool skipUserPlugins) {
	safeStartup.store(safeMode, std::memory_order_release);
	skipStartupUserPlugins.store(skipUserPlugins, std::memory_order_release);
}


bool startupSafeModeRequested() {
	return safeStartup.load(std::memory_order_acquire);
}


bool userPluginsDisabled() {
	return skipStartupUserPlugins.load(std::memory_order_acquire);
}


/** Pumps glue events until the Java side sets `done`.
Returns false on timeout/reentry (callers fall back to their default). */
static bool pumpUntilFlag(std::atomic<bool>& done, double timeoutSec, const char* what) {
	if (!pumpOnce)
		return false;
	if (pumping.exchange(true))
		return false; // nested wait from a pumped event: refuse
	// Callers off the glue thread (a plugin's worker/audio thread) have no
	// looper to pump; a plain wait is correct there — only the glue thread
	// owns the input queue whose starvation causes the ANR.
	bool onGlueThread = ALooper_forThread() != NULL;
	double start = rack::system::getTime();
	while (!done.load()) {
		if (onGlueThread)
			pumpOnce(50);
		else
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		// Safety valve: a lost reply (activity recreated, Java exception)
		// must not wedge the render thread forever.
		if (rack::system::getTime() - start > timeoutSec) {
			LOGE("%s reply never arrived; abandoning", what);
			pumping = false;
			return false;
		}
	}
	pumping = false;
	return true;
}

static bool pumpUntilDialogDone() {
	return pumpUntilFlag(dialogDone, 300.0, "dialog");
}


void jniInit(ANativeActivity* activity) {
	vm = activity->vm;
	JNIEnv* env = NULL;
	vm->AttachCurrentThread(&env, NULL);
	activityObj = env->NewGlobalRef(activity->clazz);
	jclass cls = env->GetObjectClass(activityObj);
	activityCls = (jclass) env->NewGlobalRef(cls);
	jclass sc = env->FindClass("java/lang/String");
	stringCls = (jclass) env->NewGlobalRef(sc);

	midClipboardSet = env->GetMethodID(activityCls, "clipboardSet", "(Ljava/lang/String;)V");
	midClipboardGet = env->GetMethodID(activityCls, "clipboardGet", "()Ljava/lang/String;");
	midDialogMessage = env->GetMethodID(activityCls, "dialogMessageAsync", "(IILjava/lang/String;)V");
	midDialogPrompt = env->GetMethodID(activityCls, "dialogPromptAsync", "(Ljava/lang/String;Ljava/lang/String;)V");
	midDialogFile = env->GetMethodID(activityCls, "dialogFileAsync", "(ZLjava/lang/String;Ljava/lang/String;)V");
	midMenuShow = env->GetMethodID(activityCls, "showNativeMenu", "([Ljava/lang/String;[Ljava/lang/String;[I)V");
	midMenuDismiss = env->GetMethodID(activityCls, "dismissNativeMenu", "()V");
	midBrowserShow = env->GetMethodID(activityCls, "showNativeBrowser", "()V");
	midSharePatch = env->GetMethodID(activityCls, "sharePatchFromNative", "(Ljava/lang/String;)V");
	midShowHelp = env->GetMethodID(activityCls, "showHelpFromNative", "(I)V");
	midShowLatencyPicker = env->GetMethodID(activityCls, "showLatencyPickerFromNative", "()V");
	if (!midShowLatencyPicker)
		LOGE("jni: showLatencyPickerFromNative not found");
	midLoadUserPlugins = env->GetMethodID(activityCls, "loadUserPluginsFromNative", "()V");
	midPatchReady = env->GetMethodID(activityCls, "patchReadyFromNative", "()V");
	midLanguageChanged = env->GetMethodID(activityCls, "languageChangedFromNative", "(Ljava/lang/String;)V");
	midThermalStatus = env->GetMethodID(activityCls, "currentThermalStatus", "()I");
	if (env->ExceptionCheck())
		env->ExceptionClear();
	midShowToast = env->GetMethodID(activityCls, "showToastFromNative", "(Ljava/lang/String;)V");
	if (env->ExceptionCheck())
		env->ExceptionClear();
	midShowEngineNotice = env->GetMethodID(activityCls, "showEngineNoticeFromNative", "(I)V");
	if (env->ExceptionCheck())
		env->ExceptionClear();
	midRequestAudioFocus = env->GetMethodID(activityCls, "requestAudioFocusFromNative", "()Z");
	if (env->ExceptionCheck())
		env->ExceptionClear();
	// Checked (and logged) separately from the generic catch-all below, which
	// only nulls midClipboardSet -- a silent miss here would otherwise look
	// identical to the notice simply never being shown.
	if (!midThermalStatus || !midShowToast || !midShowEngineNotice || !midRequestAudioFocus)
		LOGE("jniInit: engine-notice methods not found (thermal=%p toast=%p notice=%p focus=%p)",
			(void*) midThermalStatus, (void*) midShowToast, (void*) midShowEngineNotice,
			(void*) midRequestAudioFocus);
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		LOGE("jniInit: MainActivity methods missing; dialogs/clipboard disabled");
		midClipboardSet = NULL;
	}
}


static JNIEnv* getEnv() {
	if (!vm)
		return NULL;
	JNIEnv* env = NULL;
	if (vm->GetEnv((void**) &env, JNI_VERSION_1_6) != JNI_OK)
		vm->AttachCurrentThread(&env, NULL);
	return env;
}


static jobjectArray toStringArray(JNIEnv* env, const std::vector<std::string>& v) {
	jobjectArray arr = env->NewObjectArray(v.size(), stringCls, NULL);
	if (!arr)
		return NULL;
	for (size_t i = 0; i < v.size(); i++) {
		jstring s = env->NewStringUTF(v[i].c_str());
		env->SetObjectArrayElement(arr, i, s);
		if (s)
			env->DeleteLocalRef(s);
	}
	return arr;
}


void nativeMenuShow(const std::vector<std::string>& labels,
		const std::vector<std::string>& rights, const std::vector<int>& flags) {
	JNIEnv* env = getEnv();
	if (!env || !midMenuShow || !stringCls)
		return;
	jobjectArray jLabels = toStringArray(env, labels);
	jobjectArray jRights = toStringArray(env, rights);
	jintArray jFlags = env->NewIntArray(flags.size());
	if (!jLabels || !jRights || !jFlags) {
		if (env->ExceptionCheck())
			env->ExceptionClear();
		return;
	}
	if (!flags.empty())
		env->SetIntArrayRegion(jFlags, 0, flags.size(), flags.data());
	env->CallVoidMethod(activityObj, midMenuShow, jLabels, jRights, jFlags);
	env->DeleteLocalRef(jLabels);
	env->DeleteLocalRef(jRights);
	env->DeleteLocalRef(jFlags);
	if (env->ExceptionCheck())
		env->ExceptionClear();
}


void nativeMenuDismiss() {
	JNIEnv* env = getEnv();
	if (!env || !midMenuDismiss)
		return;
	env->CallVoidMethod(activityObj, midMenuDismiss);
	if (env->ExceptionCheck())
		env->ExceptionClear();
}


void nativeBrowserShow() {
	JNIEnv* env = getEnv();
	if (!env || !midBrowserShow)
		return;
	env->CallVoidMethod(activityObj, midBrowserShow);
	if (env->ExceptionCheck())
		env->ExceptionClear();
}


void nativeShowHelp(int which) {
	JNIEnv* env = getEnv();
	if (!env || !midShowHelp)
		return;
	env->CallVoidMethod(activityObj, midShowHelp, (jint) which);
	if (env->ExceptionCheck())
		env->ExceptionClear();
}


/** The language the user picked in Help > Language, or empty if they never
picked one. Java is the only place that knows: it writes that preference and
nothing else does, so an empty value here means "no choice has ever been
made", which is exactly what startRack() needs in order to follow the device
without ever overruling somebody. */
static std::string g_chosenLanguage;

std::string startupChosenLanguage() {
	return g_chosenLanguage;
}


void nativeShowLatencyPicker() {
	JNIEnv* env = getEnv();
	if (!env || !midShowLatencyPicker)
		return;
	env->CallVoidMethod(activityObj, midShowLatencyPicker);
	if (env->ExceptionCheck())
		env->ExceptionClear();
}


void nativeSharePatch(const std::string& path) {
	JNIEnv* env = getEnv();
	if (!env || !midSharePatch)
		return;
	jstring js = env->NewStringUTF(path.c_str());
	env->CallVoidMethod(activityObj, midSharePatch, js);
	if (js)
		env->DeleteLocalRef(js);
	if (env->ExceptionCheck())
		env->ExceptionClear();
}


static std::string jstringToStd(JNIEnv* env, jstring js) {
	if (!js)
		return "";
	const char* chars = env->GetStringUTFChars(js, NULL);
	std::string s = chars ? chars : "";
	env->ReleaseStringUTFChars(js, chars);
	return s;
}


void clipboardSet(const std::string& text) {
	JNIEnv* env = getEnv();
	if (!env || !midClipboardSet)
		return;
	jstring js = env->NewStringUTF(text.c_str());
	env->CallVoidMethod(activityObj, midClipboardSet, js);
	env->DeleteLocalRef(js);
	if (env->ExceptionCheck())
		env->ExceptionClear();
}


std::string clipboardGet() {
	JNIEnv* env = getEnv();
	if (!env || !midClipboardSet)
		return "";
	jstring js = (jstring) env->CallObjectMethod(activityObj, midClipboardGet);
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		return "";
	}
	std::string s = jstringToStd(env, js);
	if (js)
		env->DeleteLocalRef(js);
	return s;
}


/** Logs a pending Java exception's toString() instead of just clearing it --
the difference between "we know why this call never took effect" and a
silent no-op indistinguishable from the call simply never having happened.
Always clears the exception before returning: JNI's behaviour with one still
pending, on the next JNI call, is undefined. */
static void logAndClearException(const char* where) {
	JNIEnv* env = getEnv();
	if (!env || !env->ExceptionCheck())
		return;
	jthrowable ex = env->ExceptionOccurred();
	env->ExceptionClear(); // must come before any further JNI call below
	jclass exCls = ex ? env->GetObjectClass(ex) : NULL;
	jmethodID midToString = exCls ? env->GetMethodID(exCls, "toString", "()Ljava/lang/String;") : NULL;
	jstring exStr = midToString ? (jstring) env->CallObjectMethod(ex, midToString) : NULL;
	if (env->ExceptionCheck())
		env->ExceptionClear(); // toString() itself throwing: give up gracefully
	std::string msg = exStr ? jstringToStd(env, exStr) : "(no exception object)";
	LOGE("%s: Java exception: %s", where, msg.c_str());
}


int thermalStatus() {
	JNIEnv* env = getEnv();
	if (!env || !midThermalStatus)
		return 0; // PowerManager.THERMAL_STATUS_NONE
	int status = env->CallIntMethod(activityObj, midThermalStatus);
	if (env->ExceptionCheck()) {
		logAndClearException("thermalStatus");
		return 0;
	}
	return status;
}


void showToast(const std::string& text) {
	JNIEnv* env = getEnv();
	if (!env || !midShowToast)
		return;
	jstring js = env->NewStringUTF(text.c_str());
	env->CallVoidMethod(activityObj, midShowToast, js);
	env->DeleteLocalRef(js);
	if (env->ExceptionCheck())
		logAndClearException("showToast");
}


void showEngineNotice(int kind) {
	JNIEnv* env = getEnv();
	if (!env || !midShowEngineNotice)
		return;
	env->CallVoidMethod(activityObj, midShowEngineNotice, (jint) kind);
	if (env->ExceptionCheck())
		logAndClearException("showEngineNotice");
}


bool requestAudioFocus() {
	JNIEnv* env = getEnv();
	if (!env || !midRequestAudioFocus)
		return false;
	jboolean granted = env->CallBooleanMethod(activityObj, midRequestAudioFocus);
	if (env->ExceptionCheck()) {
		logAndClearException("requestAudioFocus");
		return false;
	}
	return granted;
}


void nativePatchReady() {
	JNIEnv* env = getEnv();
	if (!env || !midPatchReady)
		return;
	env->CallVoidMethod(activityObj, midPatchReady);
	if (env->ExceptionCheck())
		env->ExceptionClear();
}


void nativeLanguageChanged(const std::string& code) {
	JNIEnv* env = getEnv();
	if (!env || !midLanguageChanged)
		return;
	jstring js = env->NewStringUTF(code.c_str());
	env->CallVoidMethod(activityObj, midLanguageChanged, js);
	env->DeleteLocalRef(js);
	if (env->ExceptionCheck())
		env->ExceptionClear();
}


void loadUserPluginsBlocking() {
	JNIEnv* env = getEnv();
	if (!env || !midLoadUserPlugins) {
		LOGE("cannot load user plugins: JNI bridge not ready");
		return;
	}
	userPluginsDone = false;
	env->CallVoidMethod(activityObj, midLoadUserPlugins);
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		return;
	}
	// Shorter leash than a dialog: this runs on the startup path, so a Java
	// side that never answers must degrade to "no side-loaded packs" quickly
	// rather than hold the patch (and the whole app) hostage.
	pumpUntilFlag(userPluginsDone, 30.0, "user plugin load");
}


int dialogMessage(int level, int buttons, const std::string& message) {
	JNIEnv* env = getEnv();
	if (!env || !midClipboardSet)
		return 1; // behave like the headless stub: proceed with OK
	dialogDone = false;
	jstring js = env->NewStringUTF(message.c_str());
	env->CallVoidMethod(activityObj, midDialogMessage, level, buttons, js);
	env->DeleteLocalRef(js);
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		return 1;
	}
	if (!pumpUntilDialogDone())
		return 1;
	return dialogResultInt;
}


bool dialogPrompt(const std::string& title, const std::string& text, std::string& result) {
	JNIEnv* env = getEnv();
	if (!env || !midClipboardSet)
		return false;
	dialogDone = false;
	jstring jTitle = env->NewStringUTF(title.c_str());
	jstring jText = env->NewStringUTF(text.c_str());
	env->CallVoidMethod(activityObj, midDialogPrompt, jTitle, jText);
	env->DeleteLocalRef(jTitle);
	env->DeleteLocalRef(jText);
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		return false;
	}
	if (!pumpUntilDialogDone() || !dialogResultHasStr)
		return false;
	result = dialogResultStr;
	return true;
}


bool dialogFile(bool save, const std::string& dir, const std::string& filename, std::string& path) {
	JNIEnv* env = getEnv();
	if (!env || !midClipboardSet)
		return false;
	dialogDone = false;
	jstring jDir = env->NewStringUTF(dir.c_str());
	jstring jName = env->NewStringUTF(filename.c_str());
	env->CallVoidMethod(activityObj, midDialogFile, (jboolean) save, jDir, jName);
	env->DeleteLocalRef(jDir);
	env->DeleteLocalRef(jName);
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		return false;
	}
	if (!pumpUntilDialogDone() || !dialogResultHasStr)
		return false;
	path = dialogResultStr;
	return true;
}


bool jniSetThreadPriority(int tid, int priority) {
	JNIEnv* env = getEnv();
	if (!env)
		return false;
	static jclass processCls = NULL;      // global ref, resolved once
	static jmethodID midSetPriority = NULL;
	if (!processCls) {
		jclass local = env->FindClass("android/os/Process");
		if (!local) {
			env->ExceptionClear();
			return false;
		}
		processCls = (jclass) env->NewGlobalRef(local);
		env->DeleteLocalRef(local);
		midSetPriority = env->GetStaticMethodID(processCls, "setThreadPriority", "(II)V");
		if (env->ExceptionCheck() || !midSetPriority) {
			env->ExceptionClear();
			midSetPriority = NULL;
			return false;
		}
	}
	if (!midSetPriority)
		return false;
	env->CallStaticVoidMethod(processCls, midSetPriority, (jint) tid, (jint) priority);
	if (env->ExceptionCheck()) {
		// Process.setThreadPriority throws IllegalArgumentException if the tid
		// is gone by the time this runs (a worker can be torn down between our
		// /proc scan and this call) -- routine, not worth logging.
		env->ExceptionClear();
		return false;
	}
	return true;
}


// ---- results posted by MainActivity (UI thread) ----

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeSetStartupOptions(JNIEnv* env, jobject thiz,
		jboolean safeMode, jboolean skipUserPlugins) {
	setStartupOptions(safeMode, skipUserPlugins);
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeSetChosenLanguage(JNIEnv* env, jobject,
		jstring jCode) {
	if (!jCode) {
		g_chosenLanguage.clear();
		return;
	}
	const char* chars = env->GetStringUTFChars(jCode, NULL);
	g_chosenLanguage = chars ? chars : "";
	if (chars)
		env->ReleaseStringUTFChars(jCode, chars);
}

/** MainActivity signals that loadUserPlugins() has finished, so the startup
path may proceed to launch the patch with every side-loaded pack registered. */
extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeUserPluginsLoaded(JNIEnv* env, jobject thiz) {
	userPluginsDone = true;
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeDialogInt(JNIEnv* env, jobject thiz, jint result) {
	dialogResultInt = result;
	dialogResultHasStr = false;
	dialogDone = true;
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeDialogString(JNIEnv* env, jobject thiz, jstring js) {
	if (js) {
		dialogResultStr = jstringToStd(env, js);
		dialogResultHasStr = true;
	}
	else {
		dialogResultHasStr = false;
	}
	dialogDone = true;
}


} // namespace rackdroid
