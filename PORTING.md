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
- **Le callback vanno allineate alla raffica del dispositivo.** Rack offre i
  block size in potenze di due perché è così che lavora l'hardware audio da
  scrivania. I telefoni non sono d'accordo, e **non sono d'accordo fra loro**:
  S22 e Nothing A024 dichiarano una raffica da **96 frame**, il OnePlus 8T da
  **192**. Nessuno dei due divide 512, che ne attraversa rispettivamente 5,33 e
  2,67 cadendo sempre a metà. Ogni altro numero nel log dello stream è un
  multiplo della raffica — il buffer con cui apre, quello a cui cresce — e il
  nostro era l'unico che non lo era. Non esiste quindi un valore giusto da
  scrivere nel codice: va letto dal dispositivo.
  `alignToBurst()` in `audio_oboe.cpp` chiede il numero intero di raffiche più
  vicino (512 → 480 a 96, 512 → 576 a 192, 1024 → 1056); il motore
  non se ne accorge, `onAudioReady()` processa qualunque conteggio riceva, e il
  block size scelto dall'utente resta quello. La raffica è leggibile solo da uno
  stream già aperto, quindi la prima apertura su un dispositivo mai visto paga
  una riapertura e il valore finisce in un memo accanto al block size.
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
  possiede il numero. Si parte dal memo se c'è, altrimenti **dal pavimento**,
  e si sale solo se la patch lo richiede; poi si sale e si scende in base a
  quanto misurato. Le finestre che contengono un tocco vengono scartate, e così
  i primi cinque secondi: caricare una patch o riaprire lo stream non è una
  misura.
- **Partire in alto è un errore udibile, partire in basso no.** La prima
  ipotesi era il ceiling con Exclusive. Su un Nothing A024 (SM8735, api 36)
  appena installato questo significava aprire a 7 thread su una patch di sette
  moduli: **885 underrun e quarantacinque secondi** di scricchiolio continuo
  prima di arrivare ai 2 che voleva davvero. I due errori non costano uguale —
  troppo in alto lo sente l'utente, troppo in basso è una finestra tranquilla
  con meno core del possibile — e in basso è anche la risposta più probabile su
  un telefono, per via della barriera per campione.
- **Misurare la scadenza, non aspettare gli underrun.** Un underrun è un evento
  raro: giudicarci sopra un gradino costa cinque secondi. La stessa domanda al
  contrario è continua — ogni callback deve produrre i suoi frame in
  `numFrames / sampleRate` e quanto ne ha usato si sa appena ritorna, 47 volte
  al secondo, ed è anche la prima misura diretta di quanto costa il percorso
  Shared: stessa patch, **15–22%** su S22 e Nothing con Exclusive concesso,
  **40%** sull'8T che passa dal mixer di AudioFlinger.
  `audioEngineLoadPeak()` in `audio_oboe.cpp` pubblica il picco (il
  cronometraggio c'era già per ADPF, semplicemente non è più condizionato a una
  sessione). Oltre il **115%** i frame durano meno di quanto ci mettono a
  nascere, e la finestra si chiude lì invece di aspettarne la prova. Le finestre
  di ricerca passano da 5 s a 1,5 s e i punteggi si normalizzano a una durata
  comune, altrimenti non sarebbero confrontabili. Misurato su S22 con otto
  cicli occupati a contendere i core: **la scala si percorre tutta in 2,8 s
  invece di 25**. A riposo la lettura è 15–22% su una patch leggera, che è
  anche il primo numero che questo progetto abbia su quanto margine resta.
- **Tre trappole nella lettura del carico**, tutte trovate su hardware. È tempo
  reale attorno a una callback, quindi: un verdetto di carico non può essere
  scusato da una finestra disturbata (la prima finestra di ogni candidato porta
  sempre la mossa che l'ha creata, e la guardia esistente buttava via la
  rilevazione veloce proprio lì); va ignorato nei primi 0,4 s di finestra
  (misurato **139%** su un gradino che poi girava pulito al 20% — erano i worker
  che ripartivano) e finché lo stream non è su da 1,5 s (misurato **42705%** su
  un gradino con *un* underrun, mezzo secondo dopo un `closeStreams` da 722 ms);
  e un verdetto senza speranza vale un punteggio fisso, non la percentuale
  grezza, perché `scores[]` confronta i gradini fra loro e un numero a cinque
  cifre accanto a un conteggio a due cifre rende il confronto privo di senso.
- **Un regolatore alla volta.** `checkBlockSizeStepDown()` scatta una volta per
  lancio, una decina di secondi dopo l'avvio, e riapre lo stream. Finché la
  ricerca dei thread durava quaranta secondi la sonda cadeva dentro; resa
  veloce la ricerca, cadeva comunque dentro, e i due si addebitavano a vicenda
  le riaperture: su un A024 appena installato il conteggio ha fatto
  2 → 3 → 4 → 5 → 6 → 7 → 3 → 2 in nove secondi senza imparare niente di vero,
  e la sonda ha concluso che 256 frame "non tengono" su prove che erano
  interamente del regolatore dei thread. Ora aspetta `g_threadTunerSettled`.
  Con l'ordine giusto, lo stesso telefono appena installato: **zero underrun**,
  pulito a 2 thread al 15% della scadenza dopo 10,4 s, blocco sceso a 256 e
  **13,6 ms** di latenza totale.
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
- **La callback audio ha un core suo, e nessun worker ci può andare.** "Si
  interrompe mentre si fa lo zoom" su un Nothing A024, patch di sette moduli a
  2 thread: il log degli stalli (`audioReportSlowCallbacks()`) ha misurato
  `1633.4 ms wall, 1628.0 ms cpu, switches 0 voluntary` — un secondo e mezzo di
  CPU per un blocco che di norma ne usa il 14% di 10 ms, senza mai dormire. La
  callback **girava a vuoto** sulla `SpinBarrier` di `Engine_stepFrame()`. È
  SCHED_FIFO (glielo concede AAudio), i worker sono thread normali a nice -19:
  quando lo scheduler li mette sullo stesso core, lo spinner real-time
  preempta proprio il worker che sta aspettando, e lì nessuno può farlo girare
  finché non interviene il throttle RT del kernel o il load balancer. Stalli
  misurati da 0,3 a 1,9 s, 600 underrun in dieci secondi. Emergeva col pinch
  perché è lì che il render thread (50–100 ms a frame in draw) tiene occupati
  gli altri core; e più thread volevano dire più occasioni di collisione,
  infatti il regolatore che saliva a 7 peggiorava. Ora `applyWorkerAffinity()`
  fissa la callback sul core più veloce (`pickAudioCpu()`, cpu7 sull'SM8735) e
  tiene i worker fuori da quello e da quello riservato: su 8 core ne restano 6,
  esattamente quanti ne chiede il soffitto di 7 thread. Stessa prova dopo:
  **zero callback lente e zero underrun** durante il pinch, a 1024 e a 512
  frame, audio pulito all'ascolto. Trovato per strada: l'id del thread della
  callback veniva pubblicato una volta sola, ma ogni riapertura dello stream
  richiama su un thread nuovo, quindi ADPF (e ora il pin) puntavano a un thread
  morto. Ora si pubblica per thread, e il pin segue la riapertura.
- **Un underrun senza una callback in ritardo non è colpa dei thread.** Il
  contatore xrun si legge nella callback ed è in ritardo sullo stallo che lo ha
  causato: lo stallo di sopra a 2 thread è stato contato come
  "137 underruns at 3 threads (10% of the deadline, peak 97%)", e il regolatore
  è salito a 4, 5, 6, 7 dove la barriera rendeva il motore davvero in ritardo
  (1360%, 2099%). Una finestra con picco sotto il 100% ora non viene contata.
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
- **Durante un pinch nessun framebuffer viene ricreato.** Ogni passo di zoom
  sporca il framebuffer di ogni pannello, manopola e presa, e cambiandone la
  dimensione Rack **cancella e rialloca la texture** prima di ridisegnarla.
  Misurato con i marcatori di `windowInstallDrawMarkers()`: 40–250 ms per il
  solo riquadro delle rotaie, 50–100 ms per un pannello (Nothing A024, OnePlus
  8T), in crescita con i pixel del framebuffer e **non** con la complessità
  dell'SVG — le rotaie portate da 667 forme a 51 costavano uguale. Il budget di
  upstream non basta: `FramebufferWidget::draw()` ricrea comunque il primo
  framebuffer sporco del frame, e il primo sono sempre le rotaie. Ora durante
  il gesto `getFrameDurationRemaining()` è negativo e `Window::step()` fa
  partire `fbCount()` da 1 ("il primo è già stato fatto"): tutto viene
  disegnato scalato e torna nitido 0,1 s dopo che le dita si fermano. Stessa
  prova dopo: nessun frame lento durante il gesto, gesto fluido e sfocatura
  "quasi inesistente" a occhio, scatto al rilascio non percepito (su A024 due
  frame da ~60 ms). Costo: un modulo mai disegnato che entra in vista durante
  il pinch resta vuoto fino al rilascio. Il pinch applica lo zoom circa una
  volta per frame, non più a 30 Hz: la ricostruzione avviene al disegno, e a
  30 Hz su uno schermo a 60–120 Hz lo zoom avanzava a scatti.
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
