#!/usr/bin/env bash
# Build the host UI harness, stage every plugin's manifest/resources, require all
# plugin libraries to load, and instantiate/render every registered model.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
BUILD="${RACKDROID_HOST_BUILD_DIR:-build-host-ui}"
STAGE="${RACKDROID_HOST_STAGE_DIR:-${TMPDIR:-/tmp}/rackdroid-host-system}"
FRAMES="${RACKDROID_HOST_SMOKE_FRAMES:-30}"

rm -rf "$STAGE"
mkdir -p "$STAGE/plugins"
for item in res translations Core.json template.vcv LICENSE-GPLv3.txt; do
  [[ -e "$ROOT/third_party/Rack/$item" ]] && \
    ln -s "$ROOT/third_party/Rack/$item" "$STAGE/$item"
done

stage_plugin() {
  local slug=$1 source=$2
  [[ -f "$source/plugin.json" ]] || {
    echo "Missing plugin manifest for $slug: $source/plugin.json" >&2
    exit 1
  }
  ln -s "$source" "$STAGE/plugins/$slug"
}

# Base APK plugins.
stage_plugin Fundamental "$ROOT/third_party/Fundamental"
stage_plugin RackDroidDrums "$ROOT/drums"

# Optional .rdmod plugins. Keep this mapping synchronized with
# scripts/make_rdmods.sh and app/build.gradle.kts packaging excludes.
extra=()
add_extra() {
  local slug=$1 soname=$2 source=$3
  stage_plugin "$slug" "$source"
  extra+=("$slug=$soname")
}
add_extra Bogaudio libplugin_bogaudio.so "$ROOT/third_party/BogaudioModules"
add_extra Valley libplugin_valley.so "$ROOT/third_party/ValleyRackFree"
add_extra HetrickCV libplugin_hetrickcv.so "$ROOT/third_party/HetrickCV"
add_extra JW-Modules libplugin_jw.so "$ROOT/third_party/JW-Modules"
add_extra ML_modules libplugin_ml.so "$ROOT/third_party/ML_modules"
add_extra RJModules libplugin_rj.so "$ROOT/third_party/RJModules"
add_extra computerscare libplugin_computerscare.so "$ROOT/third_party/computerscare"
add_extra LittleUtils libplugin_littleutils.so "$ROOT/third_party/Little-Utils"
add_extra Autinn libplugin_autinn.so "$ROOT/third_party/Autinn"
add_extra Venom libplugin_venom.so "$ROOT/third_party/VenomModules"
add_extra SonusModular libplugin_sonus.so "$ROOT/third_party/sonusmodular"
add_extra NonlinearCircuits libplugin_nlc.so "$ROOT/third_party/nonlinearcircuits"
add_extra AriaSalvatrice libplugin_aria.so "$ROOT/third_party/AriaModules"
add_extra Stoermelder-P1 libplugin_packone.so "$ROOT/third_party/PackOne"
add_extra FrozenWasteland libplugin_frozenwasteland.so "$ROOT/third_party/FrozenWasteland"
add_extra AudibleInstruments libplugin_audible.so "$ROOT/third_party/AudibleInstruments"
add_extra ImpromptuModular libplugin_impromptu.so "$ROOT/third_party/ImpromptuModular"
add_extra Bidoo libplugin_bidoo.so "$ROOT/third_party/Bidoo"
add_extra GrandeModular libplugin_grande.so "$ROOT/third_party/GrandeModular"
add_extra CountModula libplugin_countmodula.so "$ROOT/third_party/CountModula"
add_extra Befaco libplugin_befaco.so "$ROOT/third_party/Befaco"

[[ ${#extra[@]} -eq 21 ]] || {
  echo "Expected 21 optional plugins, staged ${#extra[@]}" >&2
  exit 1
}
extra_plugins=$(IFS=:; echo "${extra[*]}")
cmake -S native -B "$BUILD" -G Ninja -DRACKDROID_HOST_UI=ON
cmake --build "$BUILD"

RACKDROID_SYSTEM_DIR="$STAGE" \
RACKDROID_EXTRA_PLUGINS="$extra_plugins" \
EGL_PLATFORM=surfaceless \
LIBGL_ALWAYS_SOFTWARE=1 \
  "$BUILD/rack_ui_smoke" "$FRAMES" ${RACKDROID_HOST_SMOKE_MODE:---all-modules}
