# RackDroid — unofficial Android port of the VCV Rack engine.
#
# This build compiles the Rack "engine subset" (DSP engine, plugin loader,
# patch-independent infrastructure) from the unmodified Rack v2 sources in
# third_party/Rack, plus a small Android port layer (Oboe audio driver, asset
# extraction, NativeActivity entry point).
#
# Two build modes:
#  - ANDROID (invoked by Gradle via externalNativeBuild): produces librackdroid.so
#  - host Linux (invoked directly): produces the `rack_smoke` test executable,
#    used to verify that the engine subset compiles and runs outside of the
#    official Makefile build system.
#
# UI (nanovg/GLES3) is phase 2+ — see ../PORTING.md.

cmake_minimum_required(VERSION 3.22)
project(rackdroid LANGUAGES C CXX)

# CMake 4 removed implicit compatibility with projects declaring versions
# older than 3.5. Some pinned dependencies (notably Jansson) still declare an
# old minimum despite building correctly with modern policies. Set the floor
# centrally so documented host builds work on both CMake 3.x and 4.x without
# requiring a machine-specific command-line flag.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
	set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

# Rack's logger prints __FILE__, which the compiler expands to the absolute
# path of the developer's checkout — leaked verbatim into the app's log.txt.
# Remap both the repo root and the build dir (generated/patched sources) so
# logs and debug info carry repo-relative paths only.
get_filename_component(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
add_compile_options(
	"-ffile-prefix-map=${REPO_ROOT}/="
	"-ffile-prefix-map=${CMAKE_BINARY_DIR}/=")

# Host-side reproduction of the full Android UI build (rack_ui_smoke), using
# Mesa's EGL surfaceless platform + GLES3. Requires libegl1-mesa-dev and
# libgles-dev. Catches on the desktop the same crashes the app would hit.
option(RACKDROID_HOST_UI "Build the host UI smoke test (Mesa EGL/GLES3)" OFF)

set(RACKDROID_UI FALSE)
if(ANDROID OR RACKDROID_HOST_UI)
	set(RACKDROID_UI TRUE)
endif()

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET default)
set(CMAKE_CXX_VISIBILITY_PRESET default)

set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
set(RACK_DIR ${THIRD_PARTY}/Rack)
set(RACK_VERSION 2.6.4)

if(NOT EXISTS ${RACK_DIR}/include/rack.hpp)
	message(FATAL_ERROR "Rack sources not found in ${RACK_DIR}. Run scripts/setup.sh first.")
endif()

include(FetchContent)

# ------------------------------------------------------------------
# Vendored dependencies (source builds, same on host and Android)
# ------------------------------------------------------------------

# jansson (JSON, used everywhere in Rack)
set(JANSSON_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(JANSSON_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JANSSON_WITHOUT_TESTS ON CACHE BOOL "" FORCE)
set(JANSSON_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(jansson
	GIT_REPOSITORY https://github.com/akheron/jansson.git
	# v2.14, pinned to an immutable commit for reproducible builds.
	GIT_TAG de5460cf7108b3161026a85800004e208e19f902)
FetchContent_MakeAvailable(jansson)

# zstd (patch archives are .tar.zst)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zstd
	GIT_REPOSITORY https://github.com/facebook/zstd.git
	# v1.5.6
	GIT_TAG 35016bc1c0b9a2f7121b7ecc312100aad7d9f2ad
	SOURCE_SUBDIR build/cmake)
FetchContent_MakeAvailable(zstd)

# libarchive (tar.zst patches; zip reading for APK asset extraction)
set(ENABLE_ZSTD ON CACHE BOOL "" FORCE)
set(ZSTD_INCLUDE_DIR ${zstd_SOURCE_DIR}/lib CACHE PATH "" FORCE)
set(ZSTD_LIBRARY libzstd_static CACHE STRING "" FORCE)
# Pre-seed libarchive's CHECK_FUNCTION_EXISTS results: they can't try_compile
# against the not-yet-built libzstd_static target. Without these, libarchive
# silently falls back to piping through an external `zstd` program — which
# doesn't exist on Android, breaking .vcv (tar.zst) patch loading.
set(HAVE_LIBZSTD 1 CACHE INTERNAL "")
set(HAVE_ZSTD_compressStream 1 CACHE INTERNAL "")
set(ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
set(ENABLE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(ENABLE_NETTLE OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBB2 OFF CACHE BOOL "" FORCE)
set(ENABLE_LZ4 OFF CACHE BOOL "" FORCE)
set(ENABLE_LZO OFF CACHE BOOL "" FORCE)
set(ENABLE_LZMA OFF CACHE BOOL "" FORCE)
set(ENABLE_BZip2 OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBXML2 OFF CACHE BOOL "" FORCE)
set(ENABLE_EXPAT OFF CACHE BOOL "" FORCE)
set(ENABLE_PCREPOSIX OFF CACHE BOOL "" FORCE)
set(ENABLE_PCRE2POSIX OFF CACHE BOOL "" FORCE)
set(ENABLE_CNG OFF CACHE BOOL "" FORCE)
set(ENABLE_TAR OFF CACHE BOOL "" FORCE)
set(ENABLE_CPIO OFF CACHE BOOL "" FORCE)
set(ENABLE_CAT OFF CACHE BOOL "" FORCE)
set(ENABLE_UNZIP OFF CACHE BOOL "" FORCE)
set(ENABLE_ACL OFF CACHE BOOL "" FORCE)
set(ENABLE_XATTR OFF CACHE BOOL "" FORCE)
set(ENABLE_ICONV OFF CACHE BOOL "" FORCE)
set(ENABLE_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libarchive
	GIT_REPOSITORY https://github.com/libarchive/libarchive.git
	# v3.7.4
	GIT_TAG 313aa1fa10b657de791e3202c168a6c833bc3543)
FetchContent_MakeAvailable(libarchive)

# libsamplerate (used by Fundamental's Delay)
set(LIBSAMPLERATE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIBSAMPLERATE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libsamplerate
	GIT_REPOSITORY https://github.com/libsndfile/libsamplerate.git
	# 0.2.2
	GIT_TAG b9c20b93660c3683fda12e3c2a01f0021bf96c56)
FetchContent_MakeAvailable(libsamplerate)

# speexdsp resampler (single-file build, standard OUTSIDE_SPEEX recipe)
set(SPEEXDSP_DIR ${RACK_DIR}/dep/speexdsp)
set(INCLUDE_STDINT "#include <stdint.h>")
set(SIZE16 int16_t)
set(USIZE16 uint16_t)
set(SIZE32 int32_t)
set(USIZE32 uint32_t)
configure_file(
	${SPEEXDSP_DIR}/include/speex/speexdsp_config_types.h.in
	${CMAKE_CURRENT_BINARY_DIR}/dep_include/speex/speexdsp_config_types.h)
# speex_resampler.h includes it with a quoted, directory-relative path, so it
# must also be reachable without the speex/ prefix.
configure_file(
	${SPEEXDSP_DIR}/include/speex/speexdsp_config_types.h.in
	${CMAKE_CURRENT_BINARY_DIR}/dep_include/speexdsp_config_types.h)

add_library(speexdsp_resampler STATIC ${SPEEXDSP_DIR}/libspeexdsp/resample.c)
# RANDOM_PREFIX must be "speex" so the OUTSIDE_SPEEX name-mangling in
# speex_resampler.h yields the standard speex_resampler_* symbols that
# Rack's dsp/resampler.hpp (compiled without OUTSIDE_SPEEX) references.
target_compile_definitions(speexdsp_resampler PRIVATE
	OUTSIDE_SPEEX FLOATING_POINT EXPORT= RANDOM_PREFIX=speex)
target_include_directories(speexdsp_resampler PRIVATE
	${SPEEXDSP_DIR}/include/speex
	${CMAKE_CURRENT_BINARY_DIR}/dep_include/speex)

# pffft (FFT used by dsp::minBLEP and dsp/fft.hpp)
add_library(pffft STATIC ${RACK_DIR}/dep/pffft/pffft.c ${RACK_DIR}/dep/pffft/fftpack.c)
target_include_directories(pffft PUBLIC ${RACK_DIR}/dep/pffft)

# tinyexpr (math expression parsing for parameter entry)
add_library(tinyexpr STATIC ${RACK_DIR}/dep/tinyexpr/tinyexpr.c)
target_compile_definitions(tinyexpr PRIVATE TE_POW_FROM_RIGHT TE_NAT_LOG)
target_include_directories(tinyexpr PUBLIC ${RACK_DIR}/dep/tinyexpr)

# nanovg core (no GL backend; that comes with the UI phase). Needed headless
# too: Rack's color constants call nvgRGB* in global initializers.
add_library(nanovg STATIC ${RACK_DIR}/dep/nanovg/src/nanovg.c)
target_include_directories(nanovg PUBLIC ${RACK_DIR}/dep/nanovg/src)

# osdialog: common part from Rack's dep + platform backend (Android system
# dialogs via JNI, or a logging stub for headless host builds)
if(ANDROID)
	add_library(osdialog STATIC
		${RACK_DIR}/dep/osdialog/osdialog.c
		${CMAKE_CURRENT_SOURCE_DIR}/port/osdialog_android.cpp)
	# Rack's own headers too: the backend compares a prompt against
	# string::translate() so it can recognise one of them in any language.
	target_include_directories(osdialog PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/port
		${RACK_DIR}/include
		${RACK_DIR}/dep/include)
else()
	add_library(osdialog STATIC
		${RACK_DIR}/dep/osdialog/osdialog.c
		${CMAKE_CURRENT_SOURCE_DIR}/port/osdialog_stub.c)
endif()
target_include_directories(osdialog PUBLIC ${RACK_DIR}/dep/osdialog)

# ------------------------------------------------------------------
# Rack engine subset
# ------------------------------------------------------------------

set(RACK_SOURCES
	# Infrastructure
	${RACK_DIR}/src/common.cpp
	${RACK_DIR}/src/string.cpp
	${RACK_DIR}/src/random.cpp
	${RACK_DIR}/src/logger.cpp
	${RACK_DIR}/src/system.cpp
	${RACK_DIR}/src/asset.cpp
	${RACK_DIR}/src/settings.cpp
	${RACK_DIR}/src/tag.cpp
	${RACK_DIR}/src/color.cpp
	${RACK_DIR}/src/Quantity.cpp
	# IO frameworks (device drivers are registered by the port layer)
	${RACK_DIR}/src/audio.cpp
	${RACK_DIR}/src/midi.cpp
	${RACK_DIR}/src/midiloopback.cpp
	# Plugin framework
	${RACK_DIR}/src/plugin.cpp
	${RACK_DIR}/src/plugin/Plugin.cpp
	# DSP + engine
	${RACK_DIR}/src/dsp/minblep.cpp
	${RACK_DIR}/src/engine/Cable.cpp
	${RACK_DIR}/src/engine/Engine.cpp
	${RACK_DIR}/src/engine/LightInfo.cpp
	${RACK_DIR}/src/engine/Module.cpp
	${RACK_DIR}/src/engine/ParamQuantity.cpp
	${RACK_DIR}/src/engine/PortInfo.cpp
)

# Desktop sources excluded on every platform, replaced by port/ equivalents:
#   src/network.cpp (curl) -> port/network_stub.cpp
#   src/rtaudio.cpp        -> port/audio_oboe.cpp (Android)
#   src/rtmidi.cpp         -> (phase 4: AMidi driver)
#   src/window/Window.cpp  -> port/window_android.cpp (EGL/GLES3)
#   src/dep.cpp (GL2/GLEW) -> port/dep_android.cpp (GLES3)
#   src/gamepad.cpp (GLFW joystick) -> not available on Android

set(PORT_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/port/network_stub.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/port/glfw_stub.cpp
)

if(RACKDROID_UI)
	# Touch-friendly menus: blendish sizes menu rows, the menu bar, text
	# fields and dropdowns from BND_WIDGET_HEIGHT and draws their text at
	# BND_LABEL_FONT_SIZE. Both are compile-time #defines; we generate a
	# patched copy of blendish.h with bigger values (no submodule edit) and
	# put it FIRST on the include path so both blendish.c (angle-bracket
	# <blendish.h>) and Rack's MenuEntry.cpp pick it up.
	set(BLENDISH_PATCH_DIR ${CMAKE_CURRENT_BINARY_DIR}/blendish_touch)
	file(READ ${RACK_DIR}/dep/oui-blendish/blendish.h _bnd_h)
	string(REPLACE "#define BND_WIDGET_HEIGHT 21" "#define BND_WIDGET_HEIGHT 40" _bnd_h "${_bnd_h}")
	string(REPLACE "#define BND_LABEL_FONT_SIZE 13" "#define BND_LABEL_FONT_SIZE 16" _bnd_h "${_bnd_h}")
	string(REPLACE "#define BND_SCROLLBAR_WIDTH 13" "#define BND_SCROLLBAR_WIDTH 20" _bnd_h "${_bnd_h}")
	file(WRITE ${BLENDISH_PATCH_DIR}/blendish.h "${_bnd_h}")

	# blendish (UI theme/widgets drawing; Rack's fork ships it as a .c file)
	add_library(blendish STATIC ${RACK_DIR}/dep/oui-blendish/blendish.c)
	target_include_directories(blendish PRIVATE ${BLENDISH_PATCH_DIR} ${RACK_DIR}/dep/oui-blendish ${RACK_DIR}/dep/nanovg/src)
	# blendish.c uses bare C99 `inline` helpers: without optimization no
	# out-of-line definition is emitted and the link fails in Debug builds.
	target_compile_definitions(blendish PRIVATE "BND_INLINE=static inline")
	target_compile_options(blendish PRIVATE -O2)
	list(APPEND RACK_EXTRA_LIBS blendish)

	# Full UI build: upstream UI stack + Core plugin + Android window/touch.
	file(GLOB RACK_APP_SOURCES ${RACK_DIR}/src/app/*.cpp)
	file(GLOB RACK_UI_SOURCES ${RACK_DIR}/src/ui/*.cpp)
	file(GLOB RACK_WIDGET_SOURCES ${RACK_DIR}/src/widget/*.cpp)
	# Legacy fixed-function GL demo; replaced by port/openglwidget_android.cpp
	list(REMOVE_ITEM RACK_WIDGET_SOURCES ${RACK_DIR}/src/widget/OpenGlWidget.cpp)
	# A selected module is washed in 25% red upstream, hiding the artwork the
	# selection points at -- and on a touch screen selecting is routine, so that
	# wash is on screen constantly. Generate a patched copy with the fill made
	# transparent (the thin outline stays); port/selection_glow.cpp then draws a
	# red halo around the module instead. No submodule edit, same trick as the
	# blendish sizes above.
	set(RACK_PATCH_DIR ${CMAKE_CURRENT_BINARY_DIR}/rack_patched)
	file(READ ${RACK_DIR}/src/app/ModuleWidget.cpp _mw_cpp)
	string(REPLACE "nvgFillColor(args.vg, nvgRGBAf(1, 0, 0, 0.25));"
		"nvgFillColor(args.vg, nvgRGBAf(1, 0, 0, 0.0)); // RackDroid: halo instead"
		_mw_cpp "${_mw_cpp}")
	file(WRITE ${RACK_PATCH_DIR}/ModuleWidget.cpp "${_mw_cpp}")
	list(REMOVE_ITEM RACK_APP_SOURCES ${RACK_DIR}/src/app/ModuleWidget.cpp)
	list(APPEND RACK_APP_SOURCES ${RACK_PATCH_DIR}/ModuleWidget.cpp)
	file(GLOB RACK_CORE_SOURCES ${RACK_DIR}/src/core/*.cpp)
	list(APPEND RACK_SOURCES
		${RACK_DIR}/src/context.cpp
		${RACK_DIR}/src/plugin/Model.cpp
		${RACK_DIR}/src/patch.cpp
		${RACK_DIR}/src/history.cpp
		${RACK_DIR}/src/library.cpp
		${RACK_DIR}/src/keyboard.cpp
		${RACK_DIR}/src/window/Svg.cpp
		${RACK_APP_SOURCES}
		${RACK_UI_SOURCES}
		${RACK_WIDGET_SOURCES}
		${RACK_CORE_SOURCES}
	)
	list(APPEND PORT_SOURCES
		${CMAKE_CURRENT_SOURCE_DIR}/port/dep_android.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/port/window_android.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/port/openglwidget_android.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/port/label_overlay.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/port/menu_touch.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/port/selection_glow.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/port/tour_demo.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/port/cable_park.cpp
	)
	if(ANDROID)
		list(APPEND PORT_SOURCES
			${CMAKE_CURRENT_SOURCE_DIR}/port/touch_input.cpp
			# rack_engine is a shared lib: the JNI bridge used by its
			# osdialog backend, clipboard and native menus must live inside it.
			${CMAKE_CURRENT_SOURCE_DIR}/port/jni_bridge.cpp
			${CMAKE_CURRENT_SOURCE_DIR}/port/menu_native.cpp
			${CMAKE_CURRENT_SOURCE_DIR}/port/browser_native.cpp
			${CMAKE_CURRENT_SOURCE_DIR}/port/keyboard_native.cpp)
	endif()

else()
	# Headless host build: no UI stack; lightweight replacements instead.
	list(APPEND PORT_SOURCES
		${CMAKE_CURRENT_SOURCE_DIR}/port/context_headless.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/port/model_headless.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/port/core_stub.cpp
	)
endif()

# Shared, like desktop libRack.so: the app AND the dlopen'd plugin .so files
# must all bind against one single copy of the Rack runtime.
add_library(rack_engine SHARED ${RACK_SOURCES} ${PORT_SOURCES})
if(ANDROID)
	# window_android/dep_android (EGL/GLES3) and log live inside rack_engine
	target_link_libraries(rack_engine PUBLIC android log EGL GLESv3)
endif()

target_include_directories(rack_engine PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/compat
	${RACK_DIR}/include
	${RACK_DIR}/dep/filesystem/include
	${RACK_DIR}/dep/fuzzysearchdatabase/src
	${RACK_DIR}/dep/glfw/include
	${RACK_DIR}/dep/nanovg/src
	${RACK_DIR}/dep/nanovg/example
	${RACK_DIR}/dep/nanosvg/src
	${RACK_DIR}/dep/oui-blendish
	${RACK_DIR}/dep/simde
	${SPEEXDSP_DIR}/include
	${CMAKE_CURRENT_BINARY_DIR}/dep_include
)

if(RACKDROID_UI)
	# MenuEntry.cpp (in rack_engine) sets menu-row height from
	# BND_WIDGET_HEIGHT — make it see the touch-patched blendish.h first.
	target_include_directories(rack_engine BEFORE PUBLIC ${BLENDISH_PATCH_DIR})
endif()

target_compile_definitions(rack_engine PUBLIC
	_RACK_VERSION=${RACK_VERSION}
	# Never let glfw3.h drag in desktop <GL/gl.h>; GL types come from the
	# compat GLEW shim (GLES3 on Android).
	GLFW_INCLUDE_NONE
)

# Match upstream compile.mk spirit: optimized, frame pointers kept for traces.
target_compile_options(rack_engine PRIVATE
	-O3 -funsafe-math-optimizations -fno-omit-frame-pointer
	-Wall -Wextra -Wno-unused-parameter
)

target_link_libraries(rack_engine PUBLIC
	jansson
	archive_static
	libzstd_static
	speexdsp_resampler
	pffft
	tinyexpr
	nanovg
	osdialog
	${RACK_EXTRA_LIBS}
)

# jansson's exported include dir
target_include_directories(rack_engine PUBLIC ${jansson_BINARY_DIR}/include)
# libarchive headers
target_include_directories(rack_engine PUBLIC ${libarchive_SOURCE_DIR}/libarchive)

find_package(Threads REQUIRED)
target_link_libraries(rack_engine PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# ------------------------------------------------------------------
# Statically linked plugins (Android forbids dlopen from app storage)
# ------------------------------------------------------------------

if(RACKDROID_UI)
	# Plugins are compiled as real shared libraries and dlopen'd at startup,
	# exactly like desktop Rack (libRack.so + plugin.so). This is mandatory:
	# different plugins reuse global-namespace names (pluginInstance,
	# modelVCO, VCOWidget vtables...), which silently alias each other in a
	# static link (observed: Bogaudio's VCOWidget built on Fundamental's
	# vtable -> crash). dlopen(RTLD_LOCAL) gives each plugin its own symbol
	# scope. The .so files live in the APK lib dir (dlopen from app storage
	# is forbidden on Android, from the APK lib dir it's fine); manifests
	# and res/ are extracted to <systemDir>/plugins/<slug>/ from system.zip.

	# --- Fundamental ---
	set(FUNDAMENTAL_DIR ${THIRD_PARTY}/Fundamental)
	if(NOT EXISTS ${FUNDAMENTAL_DIR}/plugin.json)
		message(FATAL_ERROR "Fundamental sources not found. Run scripts/setup.sh.")
	endif()
	file(GLOB FUNDAMENTAL_SOURCES ${FUNDAMENTAL_DIR}/src/*.cpp)
	add_library(plugin_fundamental SHARED
		${FUNDAMENTAL_SOURCES}
		${FUNDAMENTAL_DIR}/src/dr_wav.c
	)
	# NOTE: Fundamental/src must NOT be an include dir, or rack.hpp's
	# #include <plugin.hpp> would resolve to the plugin's own header. Module
	# sources use quoted includes (same-directory), like upstream plugin.mk.
	target_link_libraries(plugin_fundamental PUBLIC rack_engine PRIVATE samplerate)
	target_compile_options(plugin_fundamental PRIVATE -O3 -funsafe-math-optimizations)

	# --- Bogaudio (~120 modules) ---
	set(BOGAUDIO_DIR ${THIRD_PARTY}/BogaudioModules)
	if(NOT EXISTS ${BOGAUDIO_DIR}/plugin.json)
		message(FATAL_ERROR "Bogaudio sources not found. Run scripts/setup.sh.")
	endif()
	file(GLOB BOGAUDIO_SOURCES
		${BOGAUDIO_DIR}/src/*.cpp
		${BOGAUDIO_DIR}/src/dsp/*.cpp
		${BOGAUDIO_DIR}/src/dsp/filters/*.cpp)
	add_library(plugin_bogaudio SHARED ${BOGAUDIO_SOURCES})
	target_link_libraries(plugin_bogaudio PUBLIC rack_engine)
	# Its -I dirs must come AFTER Rack's (src/dsp/math.hpp would shadow
	# Rack's <math.hpp>): compile options are emitted after include dirs.
	target_compile_options(plugin_bogaudio PRIVATE
		-I${BOGAUDIO_DIR}/src -I${BOGAUDIO_DIR}/src/dsp -I${BOGAUDIO_DIR}/lib
		-O3 -funsafe-math-optimizations)
	target_compile_definitions(plugin_bogaudio PRIVATE RACK_SIMD=1)

	# --- Valley (Plateau, Topograph, Dexter, Interzone...) ---
	# Everything in the repo is GPLv3 (code AND panel art -- unlike e.g.
	# Count Modula, whose license explicitly forbids reusing its graphics),
	# so it is commercially redistributable with attribution + source.
	set(VALLEY_DIR ${THIRD_PARTY}/ValleyRackFree)
	if(NOT EXISTS ${VALLEY_DIR}/plugin.json)
		message(FATAL_ERROR "Valley sources not found. Run scripts/setup.sh.")
	endif()
	# Compile from a patched copy of src/, not the submodule: VecLPG.hpp
	# names members `__decay`, which newer clang (NDK) reserves as a builtin
	# type-trait keyword (used by libc++'s own <type_traits>, so a global
	# -D remap is not an option). Same keep-the-submodule-pristine approach
	# as the generated blendish.h above; the copy is only refreshed when
	# missing, so wipe the build dir after bumping the submodule.
	set(VALLEY_SRC ${CMAKE_CURRENT_BINARY_DIR}/valley_src)
	if(NOT EXISTS ${VALLEY_SRC})
		file(COPY ${VALLEY_DIR}/src DESTINATION ${VALLEY_SRC})
		file(READ ${VALLEY_SRC}/src/dsp/filters/VecLPG.hpp VECLPG_CONTENT)
		string(REPLACE "__decay" "__lpgDecay" VECLPG_CONTENT "${VECLPG_CONTENT}")
		file(WRITE ${VALLEY_SRC}/src/dsp/filters/VecLPG.hpp "${VECLPG_CONTENT}")
	endif()
	file(GLOB_RECURSE VALLEY_SOURCES ${VALLEY_SRC}/src/*.cpp)
	# Dexter/Terrorform wavetable ROMs ship as .bin blobs linked into the
	# plugin. Upstream plugin.mk does `xxd -i src/X.bin | cc -xc -`, which
	# emits `unsigned char src_X_bin[] = {...}` -- symbol derived from the
	# path relative to the plugin root. Reproduce exactly that (same
	# relative path, hence WORKING_DIRECTORY) with generated .c files.
	file(GLOB VALLEY_BINARIES RELATIVE ${VALLEY_DIR} ${VALLEY_DIR}/src/*.bin)
	set(VALLEY_BLOB_SOURCES "")
	foreach(BIN ${VALLEY_BINARIES})
		string(MAKE_C_IDENTIFIER ${BIN} BLOB_ID)
		set(BLOB_C ${CMAKE_CURRENT_BINARY_DIR}/valley_blobs/${BLOB_ID}.c)
		add_custom_command(
			OUTPUT ${BLOB_C}
			COMMAND xxd -i ${BIN} > ${BLOB_C}
			WORKING_DIRECTORY ${VALLEY_DIR}
			DEPENDS ${VALLEY_DIR}/${BIN}
			COMMENT "Embedding ${BIN}"
			VERBATIM)
		list(APPEND VALLEY_BLOB_SOURCES ${BLOB_C})
	endforeach()
	add_library(plugin_valley SHARED ${VALLEY_SOURCES} ${VALLEY_BLOB_SOURCES})
	target_link_libraries(plugin_valley PUBLIC rack_engine)
	# Valley includes its own headers as <gui/...>, <dsp/...> etc. relative
	# to src (upstream CXXFLAGS has -Isrc); emitted after Rack's include
	# dirs, like Bogaudio, so nothing can shadow Rack headers. simde
	# provides the SSE2 intrinsics on ARM (valley_sse_include.h expects
	# simde/x86/sse2.h; Rack already vendors simde).
	target_compile_options(plugin_valley PRIVATE
		-I${VALLEY_SRC}/src -I${RACK_DIR}/dep/simde
		-O3 -funsafe-math-optimizations)
	if(NOT ANDROID AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
		# valley_sse_include.h includes the real <pmmintrin.h> whenever
		# __SSE2__ is set -- which collides with the _mm_* macro aliases
		# Rack's simd/common.hpp pulls from simde when __SSE4_2__ is NOT
		# set. Upstream desktop builds never hit this because Rack's
		# compile.mk forces -march=nehalem (SSE4.2) everywhere; match that
		# here for the host build only. Android/ARM is unaffected (no
		# __SSE2__, everything goes through simde consistently).
		target_compile_options(plugin_valley PRIVATE -march=nehalem)
	endif()

	# --- HetrickCV (~78 modules, CC0 public domain incl. art) ---
	set(HETRICKCV_DIR ${THIRD_PARTY}/HetrickCV)
	if(NOT EXISTS ${HETRICKCV_DIR}/plugin.json)
		message(FATAL_ERROR "HetrickCV sources not found. Run scripts/setup.sh.")
	endif()
	if(NOT EXISTS ${HETRICKCV_DIR}/Gamma/src/arr.cpp)
		message(FATAL_ERROR "HetrickCV's Gamma submodule not initialized. Run scripts/setup.sh.")
	endif()
	file(GLOB HETRICKCV_SOURCES
		${HETRICKCV_DIR}/src/*.cpp
		${HETRICKCV_DIR}/src/DSP/*.cpp
		${HETRICKCV_DIR}/src/DSP/Phasors/*.cpp)
	add_library(plugin_hetrickcv SHARED
		${HETRICKCV_SOURCES}
		# The three Gamma translation units upstream's Makefile lists (MIT).
		${HETRICKCV_DIR}/Gamma/src/arr.cpp
		${HETRICKCV_DIR}/Gamma/src/Domain.cpp
		${HETRICKCV_DIR}/Gamma/src/scl.cpp)
	target_link_libraries(plugin_hetrickcv PUBLIC rack_engine)
	target_compile_options(plugin_hetrickcv PRIVATE
		-I${HETRICKCV_DIR}/Gamma
		-O3 -funsafe-math-optimizations)

	# --- Batch of license-clean packs (BSD/MIT/EUPL, art included) -------
	# Shared shape: plain src/*.cpp glob, no BINARIES, no external deps.
	function(add_simple_plugin TARGET DIR)
		if(NOT EXISTS ${DIR}/plugin.json)
			message(FATAL_ERROR "${TARGET} sources not found. Run scripts/setup.sh.")
		endif()
		add_library(${TARGET} SHARED ${ARGN})
		target_link_libraries(${TARGET} PUBLIC rack_engine)
		target_compile_options(${TARGET} PRIVATE
			-I${DIR}/src -O3 -funsafe-math-optimizations -w)
	endfunction()

	# JW-Modules (48 modules, BSD-3): built without OSC_ON, so oscpack --
	# desktop-only network remote control -- is not needed.
	set(JW_DIR ${THIRD_PARTY}/JW-Modules)
	file(GLOB JW_SOURCES ${JW_DIR}/src/*.cpp)
	add_simple_plugin(plugin_jw ${JW_DIR} ${JW_SOURCES})

	# ML_modules (30 modules, BSD-3; bundled freeverb is public domain)
	set(ML_DIR ${THIRD_PARTY}/ML_modules)
	file(GLOB ML_SOURCES ${ML_DIR}/src/*.cpp ${ML_DIR}/freeverb/*.cpp)
	add_simple_plugin(plugin_ml ${ML_DIR} ${ML_SOURCES})
	target_compile_options(plugin_ml PRIVATE -I${ML_DIR}/freeverb)

	# RJModules (63 modules, MIT). Compiled from a patched copy (same
	# keep-the-submodule-pristine approach as Valley): upstream EssEff never
	# initializes its tsf* member, so the first lazy loadFile() on the audio
	# thread calls free() on stack garbage -- an upstream bug that desktop
	# builds survive by allocator luck (reproduced here as a segfault).
	set(RJ_DIR ${THIRD_PARTY}/RJModules)
	if(NOT EXISTS ${RJ_DIR}/plugin.json)
		message(FATAL_ERROR "RJModules sources not found. Run scripts/setup.sh.")
	endif()
	set(RJ_SRC ${CMAKE_CURRENT_BINARY_DIR}/rj_src)
	if(NOT EXISTS ${RJ_SRC})
		file(COPY ${RJ_DIR}/src DESTINATION ${RJ_SRC})
		file(READ ${RJ_SRC}/src/EssEff.cpp ESSEFF_CONTENT)
		string(REPLACE "tsf* tee_ess_eff;" "tsf* tee_ess_eff = TSF_NULL;"
			ESSEFF_CONTENT "${ESSEFF_CONTENT}")
		file(WRITE ${RJ_SRC}/src/EssEff.cpp "${ESSEFF_CONTENT}")
	endif()
	file(GLOB RJ_SOURCES ${RJ_SRC}/src/*.cpp)
	add_simple_plugin(plugin_rj ${RJ_DIR} ${RJ_SOURCES})
	target_compile_options(plugin_rj PRIVATE -I${RJ_SRC}/src)

	# computerscare (22 modules, BSD-style)
	set(CS_DIR ${THIRD_PARTY}/computerscare)
	file(GLOB CS_SOURCES ${CS_DIR}/src/*.cpp ${CS_DIR}/src/Portaloof/*.cpp)
	list(FILTER CS_SOURCES EXCLUDE REGEX "src/test\\.cpp$")
	add_simple_plugin(plugin_computerscare ${CS_DIR} ${CS_SOURCES})

	# Little-Utils (7 modules, EUPL-1.2)
	set(LU_DIR ${THIRD_PARTY}/Little-Utils)
	file(GLOB LU_SOURCES ${LU_DIR}/src/*.cpp)
	add_simple_plugin(plugin_littleutils ${LU_DIR} ${LU_SOURCES})

	# Autinn (37 modules): pure GPLv3 -- LICENSE has no separate artwork
	# clause, so its panels ship as-is (the "noncommercial" string in its
	# LICENSE is GPL boilerplate, clause 6c).
	set(AUTINN_DIR ${THIRD_PARTY}/Autinn)
	file(GLOB AUTINN_SOURCES ${AUTINN_DIR}/src/*.cpp)
	add_simple_plugin(plugin_autinn ${AUTINN_DIR} ${AUTINN_SOURCES})

	# Frozen Wasteland (49 modules): code GPLv3, but everything under res/
	# is CC BY-NC-ND -- so the app ships REGENERATED panels/components from
	# graphics/frozenwasteland-res (see regen_graphics.py) and none of the
	# upstream art. Sources span several subdirs incl. .cc (stmlib).
	set(FW_DIR ${THIRD_PARTY}/FrozenWasteland)
	file(GLOB FW_SOURCES
		${FW_DIR}/src/*.cpp
		${FW_DIR}/src/xml/*.cpp
		${FW_DIR}/src/filters/*.cpp
		${FW_DIR}/src/dsp-compressor/*.cpp
		${FW_DIR}/src/dsp-noise/*.cpp
		${FW_DIR}/src/dsp-filter/*.cpp
		${FW_DIR}/src/stmlib/*.cc)
	add_simple_plugin(plugin_frozenwasteland ${FW_DIR} ${FW_SOURCES})

	# Audible Instruments (20 modules): GPLv3 plugin code, MIT DSP (Mutable
	# Instruments eurorack submodule). Panel art is (c) Emilie Gillet
	# "distributed with permission" -- a grant to VCV only -- so the app
	# ships REGENERATED panels from graphics/audible-res. Source list
	# mirrors the upstream Makefile.
	set(AI_DIR ${THIRD_PARTY}/AudibleInstruments)
	set(AI_EU ${AI_DIR}/eurorack)
	file(GLOB AI_SOURCES
		${AI_DIR}/src/*.cpp
		${AI_EU}/plaits/dsp/*.cc
		${AI_EU}/plaits/dsp/engine/*.cc
		${AI_EU}/plaits/dsp/speech/*.cc
		${AI_EU}/plaits/dsp/physical_modelling/*.cc)
	list(APPEND AI_SOURCES
		${AI_EU}/stmlib/utils/random.cc
		${AI_EU}/stmlib/dsp/atan.cc
		${AI_EU}/stmlib/dsp/units.cc
		${AI_EU}/braids/macro_oscillator.cc
		${AI_EU}/braids/analog_oscillator.cc
		${AI_EU}/braids/digital_oscillator.cc
		${AI_EU}/braids/quantizer.cc
		${AI_EU}/braids/resources.cc
		${AI_EU}/plaits/resources.cc
		${AI_EU}/clouds/dsp/correlator.cc
		${AI_EU}/clouds/dsp/granular_processor.cc
		${AI_EU}/clouds/dsp/mu_law.cc
		${AI_EU}/clouds/dsp/pvoc/frame_transformation.cc
		${AI_EU}/clouds/dsp/pvoc/phase_vocoder.cc
		${AI_EU}/clouds/dsp/pvoc/stft.cc
		${AI_EU}/clouds/resources.cc
		${AI_EU}/elements/dsp/exciter.cc
		${AI_EU}/elements/dsp/ominous_voice.cc
		${AI_EU}/elements/dsp/resonator.cc
		${AI_EU}/elements/dsp/tube.cc
		${AI_EU}/elements/dsp/multistage_envelope.cc
		${AI_EU}/elements/dsp/part.cc
		${AI_EU}/elements/dsp/string.cc
		${AI_EU}/elements/dsp/voice.cc
		${AI_EU}/elements/resources.cc
		${AI_EU}/rings/dsp/fm_voice.cc
		${AI_EU}/rings/dsp/part.cc
		${AI_EU}/rings/dsp/string_synth_part.cc
		${AI_EU}/rings/dsp/string.cc
		${AI_EU}/rings/dsp/resonator.cc
		${AI_EU}/rings/resources.cc
		${AI_EU}/tides/generator.cc
		${AI_EU}/tides/resources.cc
		${AI_EU}/tides2/poly_slope_generator.cc
		${AI_EU}/tides2/ramp_extractor.cc
		${AI_EU}/tides2/resources.cc
		${AI_EU}/warps/dsp/modulator.cc
		${AI_EU}/warps/dsp/oscillator.cc
		${AI_EU}/warps/dsp/vocoder.cc
		${AI_EU}/warps/dsp/filter_bank.cc
		${AI_EU}/warps/resources.cc
		${AI_EU}/frames/keyframer.cc
		${AI_EU}/frames/resources.cc
		${AI_EU}/frames/poly_lfo.cc
		${AI_EU}/peaks/processors.cc
		${AI_EU}/peaks/resources.cc
		${AI_EU}/peaks/drums/bass_drum.cc
		${AI_EU}/peaks/drums/fm_drum.cc
		${AI_EU}/peaks/drums/high_hat.cc
		${AI_EU}/peaks/drums/snare_drum.cc
		${AI_EU}/peaks/modulations/lfo.cc
		${AI_EU}/peaks/modulations/multistage_envelope.cc
		${AI_EU}/peaks/pulse_processor/pulse_shaper.cc
		${AI_EU}/peaks/pulse_processor/pulse_randomizer.cc
		${AI_EU}/peaks/number_station/number_station.cc
		${AI_EU}/stages/segment_generator.cc
		${AI_EU}/stages/ramp_extractor.cc
		${AI_EU}/stages/resources.cc
		${AI_EU}/marbles/random/t_generator.cc
		${AI_EU}/marbles/random/x_y_generator.cc
		${AI_EU}/marbles/random/output_channel.cc
		${AI_EU}/marbles/random/lag_processor.cc
		${AI_EU}/marbles/random/quantizer.cc
		${AI_EU}/marbles/ramp/ramp_extractor.cc
		${AI_EU}/marbles/resources.cc
		${AI_EU}/streams/resources.cc
		${AI_EU}/streams/processor.cc
		${AI_EU}/streams/follower.cc
		${AI_EU}/streams/lorenz_generator.cc
		${AI_EU}/streams/envelope.cc
		${AI_EU}/streams/svf.cc
		${AI_EU}/streams/vactrol.cc
		${AI_EU}/streams/compressor.cc)
	add_simple_plugin(plugin_audible ${AI_DIR} ${AI_SOURCES})
	target_compile_options(plugin_audible PRIVATE
		-DTEST -I${AI_EU} -Wno-unused-local-typedefs)

	# Impromptu Modular (32 modules): GPL code; res/ art CC BY-NC-ND ->
	# ships regenerated art from graphics/impromptu-res.
	set(IM_DIR ${THIRD_PARTY}/ImpromptuModular)
	file(GLOB IM_SOURCES ${IM_DIR}/src/*.cpp ${IM_DIR}/src/comp/*.cpp)
	add_simple_plugin(plugin_impromptu ${IM_DIR} ${IM_SOURCES})

	# Count Modula (136 modules): GPL code; LICENSE clause 18 reserves all
	# rights on logo/panel/component art -> ships regenerated art from
	# graphics/countmodula-res (all 7 panel themes).
	set(CM_DIR ${THIRD_PARTY}/CountModula)
	file(GLOB CM_SOURCES
		${CM_DIR}/src/*.cpp
		${CM_DIR}/src/modules/*.cpp
		${CM_DIR}/src/lib/*.cpp
		${CM_DIR}/src/components/*.cpp
		${CM_DIR}/src/dev/*.cpp)
	add_simple_plugin(plugin_countmodula ${CM_DIR} ${CM_SOURCES})

	# stoermelder PackOne (60 modules): GPL, art included. Vendored deps:
	# ORCA (the Ahab livecoding module) and SoundTouch (Siren repitch).
	set(P1_DIR ${THIRD_PARTY}/PackOne)
	file(GLOB_RECURSE P1_SOURCES ${P1_DIR}/src/*.cpp)
	list(FILTER P1_SOURCES EXCLUDE REGEX "/test/|\\.test\\.cpp$")
	list(APPEND P1_SOURCES
		${P1_DIR}/dep/orca-c/field.c
		${P1_DIR}/dep/orca-c/gbuffer.c
		${P1_DIR}/dep/orca-c/osc_out.c
		${P1_DIR}/dep/orca-c/sim.c
		${P1_DIR}/dep/orca-c/sysmisc.c
		${P1_DIR}/dep/orca-c/vmio.c
		${P1_DIR}/dep/orca-c/thirdparty/oso.c
		${P1_DIR}/dep/soundtouch/source/SoundTouch/AAFilter.cpp
		${P1_DIR}/dep/soundtouch/source/SoundTouch/BPMDetect.cpp
		${P1_DIR}/dep/soundtouch/source/SoundTouch/FIFOSampleBuffer.cpp
		${P1_DIR}/dep/soundtouch/source/SoundTouch/FIRFilter.cpp
		${P1_DIR}/dep/soundtouch/source/SoundTouch/InterpolateCubic.cpp
		${P1_DIR}/dep/soundtouch/source/SoundTouch/InterpolateLinear.cpp
		${P1_DIR}/dep/soundtouch/source/SoundTouch/InterpolateShannon.cpp
		${P1_DIR}/dep/soundtouch/source/SoundTouch/PeakFinder.cpp
		${P1_DIR}/dep/soundtouch/source/SoundTouch/RateTransposer.cpp
		${P1_DIR}/dep/soundtouch/source/SoundTouch/SoundTouch.cpp
		${P1_DIR}/dep/soundtouch/source/SoundTouch/TDStretch.cpp
		${P1_DIR}/dep/soundtouch/source/SoundTouch/cpu_detect_x86.cpp
		${P1_DIR}/dep/soundtouch/source/SoundTouch/mmx_optimized.cpp
		${P1_DIR}/dep/soundtouch/source/SoundTouch/sse_optimized.cpp)
	add_simple_plugin(plugin_packone ${P1_DIR} ${P1_SOURCES})
	target_compile_options(plugin_packone PRIVATE
		-I${P1_DIR}/dep
		-I${P1_DIR}/dep/soundtouch/include
		-I${P1_DIR}/dep/soundtouch/source/SoundTouch)
	if(NOT ANDROID)
		# STTypes.h skips the autoconf-generated soundtouch_config.h only
		# when ANDROID is defined; give the host build an empty one.
		file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/soundtouch_stub/soundtouch_config.h "")
		target_compile_options(plugin_packone PRIVATE
			-I${CMAKE_CURRENT_BINARY_DIR}/soundtouch_stub)
	endif()

	# Bidoo (49 modules): GPL code; res/ art CC BY-NC-ND -> ships
	# regenerated art from graphics/bidoo-res. Vendored DSP deps incl. C
	# sources (gverb, pffft).
	set(BD_DIR ${THIRD_PARTY}/Bidoo)
	# Patched copy (submodule stays pristine): ANTN is an internet-radio
	# module needing libcurl, which Android/host builds don't ship. Its
	# translation unit is excluded and its addModel() call removed; the
	# manifest entry is then skipped by the lenient bundled-plugin loader.
	set(BD_SRC ${CMAKE_CURRENT_BINARY_DIR}/bidoo_src)
	if(NOT EXISTS ${BD_SRC})
		file(COPY ${BD_DIR}/src DESTINATION ${BD_SRC})
		file(REMOVE ${BD_SRC}/src/ANTN.cpp)
		file(READ ${BD_SRC}/src/plugin.cpp BIDOO_PLUGIN_CONTENT)
		string(REPLACE "p->addModel(modelANTN);" ""
			BIDOO_PLUGIN_CONTENT "${BIDOO_PLUGIN_CONTENT}")
		file(WRITE ${BD_SRC}/src/plugin.cpp "${BIDOO_PLUGIN_CONTENT}")
	endif()
	file(GLOB BD_SOURCES
		${BD_SRC}/src/*.cpp
		${BD_SRC}/src/dep/*.cpp
		${BD_SRC}/src/dep/filters/*.cpp
		${BD_SRC}/src/dep/freeverb/*.cpp
		${BD_SRC}/src/dep/gverb/src/*.c
		${BD_SRC}/src/dep/lodepng/*.cpp
		${BD_SRC}/src/dep/pffft/*.c
		${BD_SRC}/src/dep/resampler/*.cpp)
	add_simple_plugin(plugin_bidoo ${BD_DIR} ${BD_SOURCES})
	target_compile_options(plugin_bidoo PRIVATE
		-I${BD_DIR}/dep/include -I${BD_SRC}/src/dep/dr_wav
		-I${BD_SRC}/src/dep/filters -I${BD_SRC}/src/dep/freeverb
		-I${BD_SRC}/src/dep/gverb/include -I${BD_SRC}/src/dep/minimp3
		-I${BD_SRC}/src/dep/lodepng -I${BD_SRC}/src/dep/pffft
		-I${BD_SRC}/src/dep/AudioFile -I${BD_SRC}/src/dep/resampler
		-I${BD_SRC}/src/dep)

	# Venom (71 modules): GPL-3 code AND art -- ships as-is.
	set(VN_DIR ${THIRD_PARTY}/VenomModules)
	file(GLOB VN_SOURCES ${VN_DIR}/src/*.cpp)
	add_simple_plugin(plugin_venom ${VN_DIR} ${VN_SOURCES})

	# Grande (25 modules): GPL code; art CC BY-NC-ND -> ships regenerated
	# art from graphics/grande-res.
	set(GR_DIR ${THIRD_PARTY}/GrandeModular)
	file(GLOB GR_SOURCES ${GR_DIR}/src/*.cpp)
	add_simple_plugin(plugin_grande ${GR_DIR} ${GR_SOURCES})

	# Sonus Modular (28 modules): pure GPL-3, art ships as-is.
	set(SN_DIR ${THIRD_PARTY}/sonusmodular)
	file(GLOB SN_SOURCES ${SN_DIR}/src/*.cpp)
	add_simple_plugin(plugin_sonus ${SN_DIR} ${SN_SOURCES})

	# Befaco (34 modules): GPL code; panels are (c) Befaco licensed to VCV
	# only and components are VCV CC BY-NC -> ships regenerated art from
	# graphics/befaco-res. Vendored DSP: OwlProgram (+KissFFT) and Iroi.
	set(BF_DIR ${THIRD_PARTY}/Befaco)
	file(GLOB BF_SOURCES
		${BF_DIR}/src/*.cpp
		${BF_DIR}/src/random8_core/*.cpp
		${BF_DIR}/src/noise-plethora/*/*.cpp
		${BF_DIR}/libs/OwlProgram/LibSource/*.cpp
		${BF_DIR}/libs/OwlProgram/LibSource/*.c
		${BF_DIR}/libs/OwlProgram/Libraries/KissFFT/*.c)
	list(FILTER BF_SOURCES EXCLUDE REGEX
		"LibSource/(ColourScreenPatch|MonochromeScreenPatch|PatchParameter)\\.cpp$")
	add_simple_plugin(plugin_befaco ${BF_DIR} ${BF_SOURCES})
	target_compile_options(plugin_befaco PRIVATE
		-DVCV -I${BF_DIR}/libs/OwlProgram/LibSource
		-I${BF_DIR}/libs/OwlProgram/Source
		-I${BF_DIR}/libs/OwlProgram/Libraries/KissFFT
		-I${BF_DIR}/libs/OwlProgram/Libraries
		-I${BF_DIR}/libs/Iroi)
	if(ANDROID)
		# bionic has no exp10f (GNU extension used throughout OwlProgram)
		target_compile_options(plugin_befaco PRIVATE "-Dexp10f(x)=powf(10.f,(x))")
	endif()
	set_target_properties(plugin_befaco PROPERTIES CXX_STANDARD 17)

	# Nonlinear Circuits (21 modules): CC0-1.0, code and art alike.
	set(NLC_DIR ${THIRD_PARTY}/nonlinearcircuits)
	file(GLOB NLC_SOURCES ${NLC_DIR}/src/*.cpp)
	add_simple_plugin(plugin_nlc ${NLC_DIR} ${NLC_SOURCES})

	# RackDroid Drums (6 modules): first-party, GPL-3.0-or-later, DSP and
	# panels both original to this repo -- lives directly under
	# rack-android/drums, not third_party/.
	set(DRUMS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../drums)
	file(GLOB DRUMS_SOURCES ${DRUMS_DIR}/src/*.cpp)
	add_simple_plugin(plugin_drums ${DRUMS_DIR} ${DRUMS_SOURCES})

	# QuickJS (MIT, Bellard) -- JS engine for Aria's Qqqq/Quack/Q quantizers
	# (they run TonalJS). Same pinned fork+commit as Aria's upstream Makefile.
	set(QJS_DIR ${THIRD_PARTY}/QuickJS)
	add_library(quickjs STATIC
		${QJS_DIR}/quickjs.c
		${QJS_DIR}/libregexp.c
		${QJS_DIR}/libunicode.c
		${QJS_DIR}/cutils.c
		${QJS_DIR}/quickjs-libc.c)
	file(READ ${QJS_DIR}/VERSION QJS_VERSION)
	string(STRIP "${QJS_VERSION}" QJS_VERSION)
	target_compile_definitions(quickjs PRIVATE
		CONFIG_VERSION="${QJS_VERSION}" _GNU_SOURCE)
	target_compile_options(quickjs PRIVATE -w)

	# Aria Salvatrice (19 modules): GPL code, art CC BY-SA 4.0 (commercial
	# OK, ships as-is with attribution) EXCEPT the copyrighted signature
	# logo (replaced by a blank SVG at packaging) and the Arcane/Atout/
	# Aleister tarot modules (CC BY-NC-SA art + network fetches) which are
	# excluded via patched copy.
	set(AR_DIR ${THIRD_PARTY}/AriaModules)
	set(AR_SRC ${CMAKE_CURRENT_BINARY_DIR}/aria_src)
	if(NOT EXISTS ${AR_SRC})
		file(COPY ${AR_DIR}/src DESTINATION ${AR_SRC})
		file(REMOVE ${AR_SRC}/src/Arcane.cpp)
		file(READ ${AR_SRC}/src/plugin.cpp ARIA_PLUGIN_CONTENT)
		foreach(M modelArcane modelAtout modelAleister)
			string(REPLACE "p->addModel(${M});" ""
				ARIA_PLUGIN_CONTENT "${ARIA_PLUGIN_CONTENT}")
		endforeach()
		file(WRITE ${AR_SRC}/src/plugin.cpp "${ARIA_PLUGIN_CONTENT}")
	endif()
	file(GLOB AR_SOURCES ${AR_SRC}/src/*.cpp)
	add_simple_plugin(plugin_aria ${AR_DIR} ${AR_SOURCES})
	target_compile_options(plugin_aria PRIVATE -I${AR_SRC}/src -I${THIRD_PARTY} -I${QJS_DIR})
	target_link_libraries(plugin_aria PRIVATE quickjs)
	# Upstream Makefile's include dirs
	target_compile_options(plugin_frozenwasteland PRIVATE
		-I${FW_DIR}/src/xml -I${FW_DIR}/src/ui -I${FW_DIR}/src/model
		-I${FW_DIR}/src/dsp-compressor -I${FW_DIR}/src/dsp-delay
		-I${FW_DIR}/src/dsp-filter/utils -I${FW_DIR}/src/dsp-filter/filters
		-I${FW_DIR}/src/dsp-filter/third-party/falco)
endif()

# ------------------------------------------------------------------
# Targets
# ------------------------------------------------------------------

if(ANDROID)
	# Oboe and the port layer need C++17. Only targets created from here on
	# are affected; rack_engine above keeps Rack's C++11.
	set(CMAKE_CXX_STANDARD 17)

	# Oboe (low-latency audio)
	add_subdirectory(${THIRD_PARTY}/oboe ${CMAKE_CURRENT_BINARY_DIR}/oboe)

	add_library(rackdroid SHARED
		${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
		port/main_android.cpp
		port/asset_extract.cpp
		port/audio_oboe.cpp
		port/adpf.cpp
		port/amidi_driver.cpp
		port/static_plugins.cpp
	)
	# Not linked: dlopen'd at runtime. Declared so they get built + packaged.
	add_dependencies(rackdroid plugin_fundamental plugin_bogaudio)
	target_include_directories(rackdroid PRIVATE
		${ANDROID_NDK}/sources/android/native_app_glue
	)
	target_link_libraries(rackdroid
		rack_engine
		oboe
		amidi
		android
		log
		EGL
		GLESv3
	)
	# native_app_glue's ANativeActivity_onCreate must survive the linker.
	# Oboe headers need C++17; only the port layer, rack_engine stays C++11.
	set_target_properties(rackdroid PROPERTIES
		LINK_FLAGS "-u ANativeActivity_onCreate"
		CXX_STANDARD 17)
else()
	if(RACKDROID_HOST_UI)
		add_executable(rack_ui_smoke host/main_ui_host.cpp port/static_plugins.cpp)
		add_dependencies(rack_ui_smoke plugin_fundamental plugin_bogaudio plugin_valley plugin_hetrickcv
			plugin_jw plugin_ml plugin_rj plugin_computerscare plugin_littleutils
			plugin_autinn plugin_frozenwasteland plugin_audible
			plugin_impromptu plugin_countmodula plugin_packone plugin_bidoo
			plugin_venom plugin_grande plugin_sonus plugin_aria
			plugin_befaco plugin_nlc plugin_drums)
		target_compile_definitions(rack_ui_smoke PRIVATE RACKDROID_RACK_DIR="${RACK_DIR}")
		# dlopen("libplugin_*.so") resolves via the executable's runpath
		set_target_properties(rack_ui_smoke PROPERTIES BUILD_RPATH "$ORIGIN")
		# Mesa: the GLES3 entry points live in libGLESv2.so
		target_link_libraries(rack_ui_smoke rack_engine EGL GLESv2)
		target_link_libraries(rack_engine PUBLIC EGL GLESv2)
	else()
		add_executable(rack_smoke host/main_host.cpp)
		target_compile_definitions(rack_smoke PRIVATE RACKDROID_RACK_DIR="${RACK_DIR}")
		target_link_libraries(rack_smoke rack_engine)
	endif()
endif()
