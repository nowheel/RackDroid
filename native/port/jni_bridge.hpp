#pragma once
#include <string>
#include <vector>

struct ANativeActivity;

namespace rackdroid {

/** Stores the activity/VM for later JNI calls. Call once from android_main. */
void jniInit(ANativeActivity* activity);

/** Installs the glue-thread event pump used while a dialog is open (see
main_android.cpp). Must be called once from android_main. */
void jniSetPump(void (*pump)(int timeoutMs));

/** True while a dialog result is being waited on; handleInput drops touch
events during this window (the dialog is modal anyway). */
bool dialogIsPumping();

/** One-shot options set by MainActivity before NativeActivity starts the
native glue thread. Used for automatic recovery after repeated failed starts. */
void setStartupOptions(bool safeMode, bool skipUserPlugins);
bool startupSafeModeRequested();
bool userPluginsDisabled();

/** Shows the native (Android bottom-sheet) menu with the given rows.
Non-blocking: taps are delivered back via the nativeMenuSelect JNI callback. */
void nativeMenuShow(const std::vector<std::string>& labels,
	const std::vector<std::string>& rights,
	const std::vector<int>& flags);
/** Dismisses the native menu sheet, if shown. */
void nativeMenuDismiss();

/** Tells Java the module browser was opened (canvas browser already hidden
by the caller); MainActivity pulls the model list itself via the
nativeBrowserModelsJson JNI callback and shows the sheet. */
void nativeBrowserShow();

/** Hand a saved .vcv to Java for the system share sheet. */
void nativeSharePatch(const std::string& path);

/** Open the Java help UI: 0 = guide sheet, 1 = step-by-step wizard. */
void nativeShowHelp(int which);

/** Opens the response/latency picker (the Engine menu row). Java owns the
dialog and the stored preference; native only asks for it and is told the
result through nativeSetLatencyMode. */
void nativeShowLatencyPicker();

/** Tells Java the patch is restored and the engine is running, so it can
build the model list and raise the palette. Non-blocking. */
void nativePatchReady();

/** The user picked a different interface language from Rack's own Help menu.
Java owns the other half of the app's strings -- everything in the toolbar,
the palette, the tour -- and those come from Android resources, which follow
the DEVICE locale and know nothing about Rack's setting. So it is told, it
remembers the choice, and it restarts: Rack asks for a restart anyway, having
no way to relabel widgets already on screen, and on a phone there is no reason
to make the user do it by hand. */
void nativeLanguageChanged(const std::string& code);

/** Runs MainActivity.loadUserPluginsFromNative() and BLOCKS the calling glue
thread until it reports back (pumping the looper meanwhile), so side-loaded
.rdmod packs are registered before the patch is restored. A pack's .so can
only be brought in by Java System.load() (Android linker namespaces), so this
has to round-trip through the UI thread rather than being done natively. */
void loadUserPluginsBlocking();

// Clipboard (thread-safe; dispatches to the Java UI thread internally)
void clipboardSet(const std::string& text);
std::string clipboardGet();

/** Android's own thermal throttling verdict for the whole device
(PowerManager.THERMAL_STATUS_*): 0 NONE, 1 LIGHT, 2 MODERATE, 3 SEVERE,
4 CRITICAL, 5 EMERGENCY, 6 SHUTDOWN. Thread-safe, dispatches to the Java UI
thread internally, same as the clipboard calls above. */
int thermalStatus();

/** A one-off, non-blocking notice shown as a Toast, already-resolved text --
for anything that does not need one of the specific localized notices below.
Fire-and-forget: does not wait for Java, unlike the clipboard/thermal reads
above. */
void showToast(const std::string& text);

/** Same idea, for the two notices the engine itself can raise: native has
no Android string resources of its own, so this passes a kind rather than
text, and Java resolves + localizes it (0 = engine_maxed_out, 1 =
engine_thermal_throttled -- see checkMaxedOutOverload() in main_android.cpp
for when each fires). */
void showEngineNotice(int kind);

/** Synchronous dialogs, called from the native glue thread. The thread blocks
while the dialog is shown on the Java UI thread. Levels/buttons use the
osdialog enum values. Returns 1 for OK/Yes. */
int dialogMessage(int level, int buttons, const std::string& message);

/** Text prompt. Returns true and fills `result` if confirmed. */
bool dialogPrompt(const std::string& title, const std::string& text, std::string& result);

/** Patch file picker. save=false lists *.vcv files in `dir` for opening;
save=true asks for a filename (suggesting `filename`). Returns true and fills
`path` (absolute) if confirmed. */
bool dialogFile(bool save, const std::string& dir, const std::string& filename, std::string& path);

/** Requests AUDIOFOCUS_GAIN once, held for the process's lifetime (never
ducked or abandoned automatically -- see requestAudioFocusFromNative() in
MainActivity.kt for why). Not just app etiquette: AAudio's audio policy
service uses the requesting app's audio focus/attributes state as one input
into whether it grants an Exclusive (dedicated, low-latency) stream or falls
back to Shared (mixed through AudioFlinger, materially worse and less
consistent latency) -- confirmed on a real OnePlus 8T logging "sharing=Shared"
while another app was audible, something no thread-count or block-size tuning
here can fix, since the bottleneck in that case is outside this process
entirely. Returns false (and clears any pending JNI exception) if the method
is missing or the call failed; the caller logs, and the Oboe stream still
opens either way -- this only improves the odds of the mode we actually want. */
bool requestAudioFocus();

/** Sets a thread's scheduling priority through android.os.Process, not a raw
setpriority() syscall: a thread renicing ANOTHER thread of the same process
below nice 0 needs CAP_SYS_NICE, which an app does not have, so a native-only
attempt silently no-ops. Process.setThreadPriority additionally moves the
thread into Android's audio/foreground cgroup (libcutils set_sched_policy),
which the app IS allowed to do to its own threads -- it is how RenderThread
and the audio callback thread get their elevated priority. Returns false (and
clears any pending JNI exception) if the call could not be made or threw. */
bool jniSetThreadPriority(int tid, int priority);

} // namespace rackdroid
