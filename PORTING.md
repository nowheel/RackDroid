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
- **Il regolatore deve diffidare delle proprie misure.** Ogni difetto trovato
  testando su S22 era della stessa natura: credeva a finestre che non erano
  misure. Ora ne scarta quattro tipi — quelle con un tocco, con un cambio di
  superficie, con una riapertura dello stream, e i primi cinque secondi dopo
  l'avvio. La rotazione ha richiesto un percorso suo: l'activity dichiara
  `configChanges="orientation|screenSize|..."`, quindi non passa **mai** da
  `APP_CMD_TERM_WINDOW` — arriva solo un resize, che nessuno ascoltava, e i
  suoi underrun finivano addebitati alla patch. Senza questo, un ciclo di
  background/ritorno/rotazione lo faceva vagare 2 → 1 → 2 → 5 → 3 → 4 in
  venticinque secondi.
- **La manutenzione non può fermarsi con il rendering.** Il ciclo dei frame
  faceva `ALooper_pollOnce(-1)` e saltava tutto quando la superficie spariva,
  ma l'audio in background continua per scelta: un telefono che scalda mentre
  l'app è in secondo piano andava in underrun senza nessuno sveglio a
  correggere. Ora il ciclo gira a 10 Hz senza superficie e la manutenzione è
  divisa — ciò che regola motore e audio gira sempre, ciò che tocca la scena,
  un dialogo o un riavvio aspetta la superficie.
- **Salire è facile, scendere no.** La scala si muove solo quando fa underrun,
  quindi qualunque cosa transitoria la spinga in alto ce la lascia per il resto
  della sessione: un S22 portato a 7 thread da un carico artificiale ci restava
  a carico finito, 712% di 800% su una patch che girava pulita a 4 (422%). Ora
  un conteggio che regge pulito per un minuto spende una finestra a provare
  quello sotto, e l'intervallo raddoppia dopo ogni sonda fallita (max 10 min).
- **Pavimento a 2 thread.** Su nessun dispositivo 1 è mai risultato il gradino
  migliore: sull'8T 1 e 2 erano entrambi puliti, sull'S22 1 ha prodotto 78
  underrun in una finestra dove 2 ne faceva 7. Provarlo non guadagna mai nulla.
- **Tolleranza proporzionata alla fiducia**: un gradino già dimostrato pulito
  regge fino a 4 underrun isolati per finestra (max 3 finestre), uno mai
  provato solo 1. Un underrun isolato è tanto probabilmente una notifica quanto
  la patch, e scappare da un gradino buono costa più di quanto risolva.
- **Il block size è l'ultima risorsa, non la prima.** Raddoppiarlo compra
  respiro pagando in latenza e non viene mai annullato (il messaggio dice
  all'utente di rimetterlo a mano). Scattava al primo underrun al soffitto, che
  cadeva nei secondi in cui il regolatore sta scendendo di proposito: su S22 la
  scala risultava esaurita 6 secondi dopo il lancio. Ora aspetta che il
  regolatore non abbia più niente da provare, e **mai durante una
  registrazione** — riaprire lo stream toglie la callback per ~0,7 s, che in un
  WAV è un buco muto senza niente che lo segnali.
- **Niente log dalla callback audio.** `WARN` di Rack prende un mutex che anche
  il render thread tiene e finisce in `fflush()`; `logger.cpp` di upstream lo
  dice da sé: *"logging is not used in performance critical code"*. Era chiamato
  a ogni underrun, cioè quando la callback era già in ritardo: un anello di
  retroazione. Ora la callback pubblica degli atomici e il ciclo dei frame
  scrive, una riga al secondo (a una riga per frame le prove si spingevano fuori
  dal file da 10 MB in cui devono essere trovate).
- **ADPF c'è, ma quasi nessuno lo concede.** `port/adpf.cpp` dichiara al
  sistema la scadenza della callback (block size ÷ sample rate) e le riporta la
  durata reale, così le frequenze salgono *prima* dell'underrun invece che
  dopo. La sessione copre la callback audio più tutti i worker — l'API la
  descrive come "un gruppo di thread con carico correlato", che è esattamente
  la nostra situazione. Risolta con `dlsym`: l'NDK marca quelle funzioni
  `__INTRODUCED_IN(33)` e il minSdk è 29, e la flag dei simboli deboli
  cambierebbe il link dell'intero modulo per sei funzioni. **Nessun beneficio
  misurato**: l'S22 rifiuta la sessione e dice perché —
  `perf_hint: PerformanceHint cannot create session. PowerHintSessions are not
  supported!` — il servizio esiste su Android 16 ma il power HAL del vendor non
  lo implementa. Stessa forma dell'allowlist audio di OnePlus: l'API c'è, il
  produttore non l'ha fatta. Dove viene rifiutata costa zero: un dlopen, una
  chiamata rifiutata, e la callback salta del tutto le sue due `clock_gettime`.
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
- **Tenuta termica misurata (S22, 12 minuti di carico continuo + churn di
  lifecycle):** picco AP 49,0 °C, picco SKIN 38,9 °C, throttling mai oltre il
  livello 1, batteria in salita con un alimentatore da ~10 W. Dopo il primo
  minuto la curva è piatta: l'S22 regge questo carico indefinitamente.
  L'unico sensore che va in allarme è SKIN (la superficie), non il SoC — è un
  limite al tatto, non di silicio, quindi una ventola sul retro agisce proprio
  sul sensore che causa il throttling. Per contrasto, l'8T si è riavviato due
  volte sotto lo stesso tipo di test.
- **NEON c'è già, verificato nel binario.** Rack usa intrinseche SSE che SIMDE
  traduce in NEON su arm64; `libplugin_fundamental.so` contiene 228 `fmla v.4s`,
  277 `fmul v.4s`, 140 `fadd v.4s` contro 270 `fmul` scalari. Anche `-O3` e
  `-funsafe-math-optimizations` sono già in `native/CMakeLists.txt`. E
  `cpuPause()` emette davvero `YIELD` nello spin loop: `ARCH_ARM64` arriva da
  `arch.hpp` via `__aarch64__`, che l'NDK imposta da sé (verificato
  disassemblando `Engine::stepBlock`, due `yield`, uno per barriera).
- **Perché la barriera per-sample non si tocca.** `Engine_stepFrameCables()`
  copia i valori dei cavi *tra* un sample e l'altro: è così che un segnale
  attraversa un cavo in 20 µs. Sincronizzare per blocco (96.000 barriere/s →
  375) farebbe attraversare ogni cavo in 5,3 ms e cambierebbe il suono delle
  patch con retroazione a frequenza audio. Non è uno spreco: è portante. La
  strada praticabile resta usare meno thread, non sincronizzarli meglio.
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
