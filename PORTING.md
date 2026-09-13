# Roadmap tecnica del porting

Stato: **release candidate 0.1.2.1**. Motore, rendering, input touch, MIDI,
dialoghi, registrazione e plugin installabili sono operativi; il lavoro residuo
è soprattutto collaudo su più dispositivi e rifinitura UX.

## Fase 1 — Motore headless ✅

Obiettivo: il motore DSP di Rack compila e gira su toolchain non-desktop.

- [x] Rack v2.6.4 + Oboe vendorizzati come submodule, sorgenti upstream intatti
- [x] Build CMake dell'"engine subset" (~30 file di src/ + dep sorgente:
      nanovg core, pffft, tinyexpr, speexdsp resampler, osdialog common)
- [x] Dipendenze C via FetchContent pinnate: jansson 2.14, zstd 1.5.6,
      libarchive 3.7.4 (tar+zip, per patch .tar.zst e asset APK)
- [x] Sostituzioni headless: `context_headless` (Context senza UI),
      `model_headless` (Model senza menu), stub network/osdialog/Core/GLFW,
      shim `GL/glew.h`
- [x] Smoke test host: bring-up completo, 1 s di audio in ~2 ms, shutdown pulito
- [x] Driver audio Oboe (`rack::audio::Driver`), full-duplex, error recovery
- [x] App NativeActivity: estrazione asset, engine avviato, EGL/GLES3, tono di test

Verifica su dispositivo: installare `assembleDebug`; schermo grigio scuro,
tono a 440 Hz, log `rackdroid` in logcat.

## Fase 2 — Rendering della scena ✅ (build; da validare su dispositivo)

Obiettivo: vedere il rack renderizzato e interagirci.

- [x] `dep_android.cpp`: `NANOVG_GLES3_IMPLEMENTATION` + nanosvg + stb;
      blendish compilato dal `.c` del fork di Rack (con fix inline C99)
- [x] `window_android.cpp`: `window::Window` completo su EGL/ANativeWindow —
      contesto GLES3 con stencil, surface sostituibile a caldo (rotazione,
      background) senza perdere il contesto GL né la Scene, pixelRatio dalla
      densità display, font/image cache come upstream
- [x] Stack UI upstream compilato intero: `src/app`, `src/ui`, `src/widget`
      (tranne `OpenGlWidget.cpp`, sostituito: usava GL fixed-function),
      `src/window/Svg.cpp`, `src/patch.cpp`, `src/history.cpp`,
      `src/context.cpp`, `src/library.cpp`, `src/keyboard.cpp`;
      rimossi i sostituti headless dal build Android
- [x] Plugin Core reale (`src/core/`): il modulo Audio pilota il driver Oboe
      (unico driver registrato → default automatico); TestTonePort rimosso
- [x] `APP->patch->launch("")` all'avvio: autosave o template.vcv, con
      autosave alla chiusura dell'app
- [x] Clipboard reale via JNI al posto degli stub GLFW (vedi fase 4)
- [x] Validazione su dispositivo: Galaxy S22 (Android 16), rendering della
      scena, cavi e FramebufferWidget (curve ADSR, scope) su ES3 verificati

## Fase 3 — Input touch (base fatta in fase 2)

- [x] Un dito = mouse sinistro (hover+press/drag/release): manopole, cavi, menu
- [x] Long-press fermo (0,6 s) = click destro → menu contestuali
- [x] Due dita = pan (scroll); pinch = zoom (Ctrl+scroll emulato)
- [x] Tastiera software: prompt Android per i TextField (fase 4) e campo di
      ricerca in-place nella palette (`showSoftInput`, ModulePalette)
- [x] Tolleranze touch per i cavi: rilascio vicino a un jack agganciato alla
      porta compatibile più vicina tramite `cableParkNearestPort`, sia per i
      cavi normali sia per le estremità parcheggiate
- [x] Porte compatibili evidenziate durante il trascinamento di un cavo; il
      target che verrebbe scelto è mostrato con un anello più marcato
- [x] Inerzia dello scroll e drag dei moduli dalla palette al punto di rilascio
- [x] Selezione multipla a tocco (`multiSelect` in `touch_input.cpp`): mentre è
      attiva il dito su un modulo non arriva a Rack, il rilascio inverte la
      selezione, il trascinamento fa scorrere la vista e solo la pressione
      prolungata invia il click trattenuto e sposta la selezione. L'hover
      necessario a riconoscere il modulo viene poi annullato con `handleLeave()`,
      altrimenti compare il tooltip della manopola sottostante
- [x] Alone rosso sui moduli selezionati (`port/selection_glow.cpp`): overlay
      sulla scena che disegna il box gradient dell'ombra di trascinamento con il
      pannello ritagliato come buco. Il velo rosso di upstream è tolto da una
      copia patchata di `ModuleWidget.cpp` generata da `native/CMakeLists.txt`,
      senza modificare il sorgente del submodule
- [ ] Doppio tap (rifinitura opzionale, nessuna funzione dipende da questo)

## Fase 4 — MIDI, dialoghi e file ✅ (build; da validare su dispositivo)

- [x] Driver `rack::midi` su **AMidi**: MainActivity apre i device MidiManager
      (USB/BLE/virtuali, hotplug incluso) e li passa al driver nativo; input
      con parser running-status su thread di polling, output supportato
- [x] **Dialoghi Android** al posto degli stub osdialog: messaggi OK/Annulla,
      prompt di testo, salvataggio patch (nome file) e apertura (lista dei
      .vcv nella cartella utente) — File > Salva/Apri ora funzionano
- [x] **Clipboard reale** (ClipboardManager via JNI) per copia/incolla
      preset, moduli e testo
- [x] **Editing testo touch**: tap su un TextField apre un prompt Android con
      tastiera di sistema (ricerca nel browser moduli, Notes, ...)
- [~] Integrazione storage: `.rdmod` selezionabili con Storage Access Framework,
      patch `.vcv` importabili tramite VIEW/SEND e condivisibili tramite
      FileProvider; manca solo un browser documenti generico per scegliere
      direttamente una destinazione di export
- [x] Import di patch .vcv da altre app (intent filter VIEW/SEND nel
      manifest + `MainActivity.handleImportIntent`)

## Fase 5 — Ecosistema plugin (primo passo fatto)

- [x] Meccanismo di **plugin bundled** (`port/static_plugins.cpp`): ogni
      plugin è una vera shared library nell'APK (lib dir: lì dlopen È
      permesso), caricata con RTLD_LOCAL come su desktop. Necessario: plugin
      diversi riusano nomi globali (pluginInstance, modelVCO, vtable di
      VCOWidget...) che in un link statico si aliasano tra loro → crash.
      Architettura finale: librack_engine.so + librackdroid.so (app) +
      libplugin_*.so, manifest/res estratti in systemDir/plugins/<slug>/
- [x] **Fundamental 2.6.4** compilato nell'APK (40 moduli: VCO, VCF, VCA,
      LFO, ADSR, Delay, mixer, SEQ3, Scope, ...) + libsamplerate vendorizzata;
      la patch template di default ora si carica intera
- [x] Fix libarchive/zstd: i risultati dei check sono pre-seedati, altrimenti
      libarchive ripiega sul programma esterno `zstd` (inesistente su
      Android) e i file .vcv non si aprono
- [x] APK base snello con 66 moduli: Core, Fundamental e RackDroid Drums.
      Bogaudio e gli altri plugin non-base non appesantiscono l'APK
- [x] **21 pacchetti `.rdmod` opzionali** generabili da
      `scripts/make_rdmods.sh`, inclusi Bogaudio, Valley, Befaco, Audible,
      HetrickCV e altri; set separati `arm64-v8a`/`x86_64`, con risorse e
      thumbnail nel rispettivo pacchetto
- [x] Test host `rack_ui_smoke N --all-modules`, che istanzia e renderizza ogni
      modello registrato come farebbe la palette
- [ ] Toolchain generica NDK per plugin fuori dall'APK (solo distribuzione
      fuori Play Store)
- [x] Niente store/account VCV su Android per scelta progettuale
      (`network_stub` resta e ogni richiesta fallisce in modo controllato)

## Audio e prestazioni (misurato su hardware)

- **Il percorso audio veloce non lo decide l'app.** Oboe chiede
  `SharingMode::Exclusive` e su alcuni firmware riceve `Shared` senza errore: il
  motivo compare solo nel log di sistema (`getListValueByUid(aaudio-compatible-apps)
  but return null` → `aaudio denied with imcompatible policy`). È una allowlist
  per-app del vendor (Oplus/OnePlus), non AOSP: sul OnePlus 8T nega anche a un
  synth Oboe commerciale. Non esiste API, flag di manifest o impostazione per
  entrarci. Escluse come cause: Bluetooth, altre app che tengono il device,
  audio focus, capacità MMAP del device (`isMMapSupported()` = 1), Dolby Atmos.
- **Con `Shared`, più thread peggiorano l'audio.** `Engine_stepFrame()`
  sincronizza ogni worker a due barriere di spin **per sample**: il costo cresce
  col numero di thread, non con la patch. Sull'8T, stessa patch: 1–2 thread
  0 underrun/60 s, 4 thread ~8,4/s, 7 thread ~9,2/s a ~515% di 800% di CPU. Una
  patch da 76 moduli converge comunque a 2 thread. Sull'S22 con Exclusive
  concesso l'ordine si inverte: 224 moduli, 1 thread 1522 underrun, 8 thread 22.
  Nessuna formula su `cores` è giusta su entrambi.
- **Quindi i thread non sono più un'impostazione utente.** La riga sparisce dal
  menu Engine (filtrata in `menu_native.cpp` per etichetta tradotta; upstream
  intatto) e una sola funzione (`checkThreadCount()` in `main_android.cpp`)
  possiede il numero. La prima ipotesi viene dallo sharing mode (Exclusive →
  ceiling, Shared → 2), poi sale e scende in base agli underrun misurati. Le
  finestre che contengono un tocco vengono scartate, e così i primi cinque
  secondi: caricare una patch o riaprire lo stream non è una misura.
- **Il core riservato si sceglie per frequenza, non per indice**: su SM8250
  cpu7 è il core *prime*, quindi la vecchia regola `cores-1` regalava via il
  core più veloce (`pickReservedCpu()` legge `cpuinfo_max_freq`).
- **Il block size non può superare la capacità del buffer dello stream**: 4096
  frame in un buffer da 1536 è una callback in ritardo per costruzione
  (misurati 225 underrun/30 s). La scala è limitata da
  `audioMaxUsefulBlockSize()`, con una riparazione all'avvio per un valore
  persistito troppo grande.
- **Zoom limitato a 2×** (`MAX_RACK_ZOOM`): a 4× ogni modulo visibile viene
  rasterizzato in un framebuffer con sedici volte i pixel **e** quel framebuffer
  viene riallocato a ogni passo di zoom. Quella tempesta di allocazioni sul
  render thread spezzava l'audio allo zoom massimo. Limite applicato in due
  punti: `touch_input.cpp` non chiede più del muro, `checkZoomCeiling()` copre
  ogni altra via (menu View, patch salvata su desktop a 4×).
- **Niente inerzia dopo un pinch**: due dita non si alzano mai insieme, il
  centroide salta su quella rimasta e produce una velocità che nessuna mano ha
  fatto. Il pan mantiene la sua inerzia, lo zoom no.

## Debiti tecnici correnti

- `minSdk 29` (Android 10): risolto lo shim `<execinfo.h>` che teneva il
  minimo a 33 (`native/compat/execinfo.h`, backtrace via `<unwind.h>` sotto
  API 33). Il pavimento reale ora è l'API MIDI nativa AMidi, anche lei
  introdotta in API 29. Bluetooth LE MIDI usa BLUETOOTH_SCAN/CONNECT su
  API 31+ e ACCESS_FINE_LOCATION + BLUETOOTH/BLUETOOTH_ADMIN legacy su
  API 29-30 (branch in `MainActivity.showBleMidiScanner`). Inset/immersive
  mode e cross-window blur passano per androidx WindowCompat/
  WindowInsetsControllerCompat invece delle API dirette (che richiedevano
  30/31/33), così girano fino ad API 29 senza NoSuchMethodError.
- La rejection del manifest Core in fase 1 è attesa (stub senza modelli)
- Branding e grafica distributiva sono stati sostituiti con asset originali;
      le attribuzioni residue sono documentate in `graphics/NOTICE-graphics.md`.
- La lista modelli pubblicata dal render thread è protetta da mutex e
      generazione monotona; `ModulePalette` attende la generazione richiesta senza
      timer empirici, anche dopo installazione o rimozione di un pacchetto.
- Preferiti: rimossi insieme al browser a tutto schermo; se servono vanno
  reintrodotti nella palette (JNI e campo JSON `favorite` sono stati tolti).
- Geometria letta dal thread UI: tutto ciò che Java chiede al nativo mentre il
  render thread disegna deve passare da variabili atomiche ripubblicate ogni
  frame (`selectionCount`, il rettangolo della barra cavi per il tour). Il
  contesto di Rack è thread-local: calcolarlo dentro la JNI chiamata da Java
  significa dereferenziare un puntatore nullo, ed è già costato un SIGSEGV.
- I sottomenu (per esempio Preset di un modulo) arrivano a `present()` **senza**
  `parentMenu`, perché il bottom sheet li ripropone come lista di primo livello.
  Chi deve sapere da dove viene un menu non può dedurlo dalla struttura: il
  flag `moduleMenuActive` segue l'interazione che ha aperto la catena.
