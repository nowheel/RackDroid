<div align="center">

<img src="graphics/readme/hero-en.svg" width="100%" alt="RackDroid: your modular rack, in your pocket">

<p><img src="https://img.shields.io/badge/version-0.1.2.1-FFDA9F?style=flat-square&labelColor=2B2721" alt="Version 0.1.2.1"> <img src="https://img.shields.io/badge/Android-10%2B-FFDA9F?style=flat-square&labelColor=2B2721" alt="Android 10 or later"> <img src="https://img.shields.io/badge/engine-VCV%20Rack%202.6.4-FFDA9F?style=flat-square&labelColor=2B2721" alt="VCV Rack 2.6.4 engine"> <img src="https://img.shields.io/badge/modules-66%20built--in%20%C2%B7%20900%2B%20in%20packs-FFDA9F?style=flat-square&labelColor=2B2721" alt="66 built-in modules, over 900 in packs"> <img src="https://img.shields.io/badge/license-GPLv3-FFDA9F?style=flat-square&labelColor=2B2721" alt="GPLv3"></p>

<p><b>English</b> · <a href="README.it.md">Italiano</a> · 🌐 <a href="https://rackdroid.org">rackdroid.org</a> · unofficial, not affiliated with VCV</p>

A **touch-first** modular synthesizer for Android: build real patches by
dragging cables between oscillators, filters, envelopes and sequencers, just
like a hardware rack. No compromises: it's the [VCV Rack 2](https://vcvrack.com)
audio engine, made native for your phone.


<img src="graphics/screenshots/patch-rack.png" width="250" alt="A live patch in RackDroid">
<img src="graphics/screenshots/toolbar-menus.png" width="250" alt="Toolbar and menus">
<img src="graphics/screenshots/theme-cream.png" width="250" alt="The Cream theme">

*Real screenshots, from real devices, no mockups.*

</div>

<img src="graphics/readme/head-why-en.svg" width="100%" alt="Why RackDroid">

- **It's VCV Rack, not a clone.** Same engine, same 66 built-in modules,
  same `.vcv` patch format: a patch made on the computer opens on the phone,
  and the other way round, and sounds the same.
- **Touch-first from day one**, not a shrunk-down desktop UI: drag cables with
  a finger, long-press a knob to type a value, pinch to zoom, a module palette
  designed for small screens, and a **cable-parking bar** that solves the "two
  modules never fit on screen at once" problem.
- **Low latency** (Oboe/AAudio, full-duplex): it plays in real time. How low
  is partly up to the phone, because some manufacturers keep the fastest audio
  path for certain apps: RackDroid uses the best one the device grants and
  adjusts to it: see [Performance](#performance).
- **Grows with you**: start with the 66 built-in modules, then add whole
  packages (Bogaudio, Valley, Befaco, HetrickCV…) on the fly, without updating
  the app.
- **Runs on older hardware too**: supported from Android 10 up.

<img src="graphics/readme/head-includes-en.svg" width="100%" alt="What it includes">

| | |
|---|---|
| 🎚️ **Native audio engine** | Oboe/AAudio, full-duplex, low latency where the device grants it; the engine picks its own thread count ([Performance](#performance)) |
| 🧩 **66 built-in modules** | Core (Audio/MIDI), Fundamental (39 modules: VCO, VCF, VCA, ADSR, LFO, SEQ-3, Delay, Mixer, Scope, Quantizer…), RackDroid Drums (14 original 808-style drum voices) |
| 👆 **Touch interface** | one finger pans the rack, drag for cables/modules, pinch to zoom, long-press a knob to type a value |
| 🪟 **Glass toolbar** | File/Edit/View/Engine/Help menus plus sixteen tools on two rows: palette, module manager, cable parking, theme, MIDI, keyboard, recording, info; undo/redo, multi-select, copy/paste, delete, the two padlocks. It collapses into a tab |
| ✅ **Select and edit** | turn on multi-select and a tap picks a module out (tap again to drop it); a hold moves the whole selection. Selected modules get a red halo instead of a wash over the panel, so the artwork stays readable. Delete asks first and says how many are going |
| 🧲 **Module palette** | chips by category (VCO, LFO, VCF, VCA, ENV, SEQ, DRUM, MIX, FX, NOISE, QNT, MIDI, UTIL), draggable previews, ⓘ badge with name/description/tags |
| 🧵 **Cable parking** | a left-edge bar where a cable end waits while you scroll to its destination: it grows from 3 up to 10 holes as you fill them, lights up compatible ports while you aim, collapses to a handle |
| 🎹 **MIDI** | on-screen musical keyboard, USB and Bluetooth LE MIDI |
| ⏺️ **Recording** | output to a WAV file in `Documents/RackDroid/` |
| 🎓 **Guided learning** | a 20-step interface tour on first run that demonstrates itself: a step per menu that says what is inside and then opens it, plus framing your modules, opening the palette, moving a module, zooming and scrolling the rack, and drawing a cable with the compatible jacks lit, then putting everything back. Also 30 step-by-step tutorials across 5 levels, plus a topic-based guide |
| 🔄 **Updates (GitHub build)** | opt-in: RackDroid can ask GitHub once a day whether a newer release exists and install it. Refuse and it never connects. The Play build has no updater and no network permission at all |

<img src="graphics/readme/head-themes-en.svg" width="100%" alt="Five themes">

<img src="graphics/readme/themes-en.svg" width="100%" alt="The five themes: Amber, Blue Night, Emerald, Violet, Cream">

Amber, Blue Night, Emerald, Violet and the light one, Cream: pick one from the palette button in the toolbar. A theme recolours the toolbar, the menus, the rack and the built-in modules; module packs keep their own panels.

<img src="graphics/readme/head-modules-en.svg" width="100%" alt="Additional modules (.rdmod)">

Beyond the built-in modules, you can add packages (Bogaudio, Valley, Audible,
Impromptu, Befaco, HetrickCV…) **on the fly**, without updating the app:

- **From the app**: *Module Manager* tool → *Install from file* → pick one or
  more `.rdmod` files. They load immediately; uninstall them from the same
  manager.
- **From a folder**: copy the `.rdmod` files to
  `Android/data/org.rackdroid/files/Modules/` and restart.
- **All of them at once**: give either route the `all_rdmods.zip` from the
  releases page as it is. A zip holding packs is unpacked and each one
  installed, so there is nothing to extract and nothing to multi-select.

Package format, the native loading mechanism, and instructions for
**creating** a plugin: see **[MODULES.md](MODULES.md)** and the manual at
**[docs/rackdroid-manuale.pdf](docs/rackdroid-manuale.pdf)**.

<img src="graphics/readme/head-requirements-en.svg" width="100%" alt="Requirements">

**Android 10 (API 29)** or later on a 64-bit `arm64-v8a` or `x86_64`
device. Requires OpenGL ES 3.0. `arm64-v8a` is the normal phone/tablet build;
`x86_64` is intended mainly for emulators and compatible ChromeOS devices.

<a name="performance"></a>
<img src="graphics/readme/head-performance-en.svg" width="100%" alt="Performance">

**The engine tunes itself.** There is no thread setting: when you open a
patch, RackDroid chooses how many cores to use for that phone and that patch,
and remembers it. The first time a patch is opened it may take a few seconds
of silence; after that it starts at once.

**Independent parts run in parallel.** Groups of modules with no cable
between them (several voices, or several instruments in one rack) are each
computed on a core of their own. A rack made of separate parts therefore
holds far more than one where everything is connected.

**The audio block** is automatic; the Engine menu lets you fix its size, and
then the app leaves it alone.

If the audio crackles:

- **The patch may be too heavy for the phone.** The app says so and offers to
  halve the engine's sample rate.
- **Heat matters.** A hot phone slows down and holds less.
- **When you unlock the screen or pull down the notification shade** a short
  crackle is possible: at those moments Android takes the fast cores away
  from the app.
- **To report a problem** the logs are what helps: press Home right after it
  happens, then attach `log.txt` and `java-log.txt` from
  `Documents/RackDroid/`, with the phone's model. They are in the app too:
  the ⓘ tool, then **Log**.

Zoom stops at 2×, so that drawing does not take time from the audio.

<img src="graphics/readme/head-build-en.svg" width="100%" alt="Build">

Gradle project at the repo root (`minSdk 29`). All `third_party/`
sources (Rack v2.6.4, Oboe, all plugins) are **vendored in the repo**: a clean
clone compiles as-is, no submodule init needed.

```sh
export JAVA_HOME=~/jdk21; export ANDROID_HOME=~/android-sdk
./gradlew assembleSideloadRelease -PdevKeystore                 # arm64-v8a (default)
./gradlew assembleSideloadRelease -PdevKeystore -PtargetAbis=x86_64  # x86_64
./gradlew bundlePlayRelease -PtargetAbis=arm64-v8a,x86_64       # Play AAB, both 64-bit ABIs
```

- `-PdevKeystore` signs with the public development key (update continuity for
  sideloading; use a private key for Play).
- The base APK is ~40 MB and contains only the built-in modules. Optional
  libraries are built with `-PallPlugins`, excluded from the APK, and
  distributed as ABI-specific `.rdmod` files (`packaging.jniLibs.excludes`,
  see `scripts/make_rdmods.sh`).

<img src="graphics/readme/head-structure-en.svg" width="100%" alt="Structure">

```
app/            Android module (Gradle, manifest, MainActivity + Kotlin UI)
native/
  CMakeLists.txt  Rack engine + dependencies + port layer build
  port/           porting code (Oboe audio, menus, browser, plugin loader…)
  host/           engine/UI smoke tests on Linux
drums/          RackDroid Drums (first-party package, original code + panels)
graphics/       original graphics (panels, thumbnails, rebuilt ComponentLibrary, screenshots)
third_party/    Rack, Oboe and plugin sources (upstream untouched)
scripts/        setup.sh (sources) · make_rdmods.sh (packages the .rdmod files)
docs/           user manual (PDF + HTML source)
MODULES.md      .rdmod format, loading mechanism and plugin creation
```

Guiding principle: **zero patches to Rack's sources**. All platform-specific
code lives in `native/port/`; desktop-only files are excluded from the build
and replaced, so upgrading to new upstream versions remains a simple submodule
bump.

<img src="graphics/readme/head-licenses-en.svg" width="100%" alt="Licenses, trademarks and distribution: important">

- Rack's code is **GPLv3**: this port is GPLv3 and the complete sources are in
  the repository (license obligation satisfied ✓).
- **Trademark**: the app presents itself as "RackDroid" (custom icon,
  rebranded strings); the "VCV" name/logo is not used ✓.
- **Graphics**: the original ComponentLibrary and Core panels are **CC
  BY-NC-ND 4.0** (non-commercial). RackDroid uses **rebuilt** graphics
  (`graphics/`, GPLv3) in their place to be distributable; the
  Fundamental/Bogaudio etc. plugins are GPLv3 with their graphics included ✓.
- **Signing**: `keystore/rackdroid.keystore` is a **development** key with a
  public password (`rackdroid`): it is for update continuity when sideloading, NOT
  for authenticity. For a store, generate a private key (or use Play App
  Signing).
- **Google Play**: distributing native code executed from **outside** Play
  violates their policies; the `.rdmod` folder / file installation are for
  sideload/GitHub builds. For Play, deliver extra packages via *asset packs*.

<img src="graphics/readme/divider.svg" width="100%" alt="">

<div align="center">

RackDroid is a port of VCV Rack (GPLv3). Not affiliated with or endorsed by VCV.

</div>
