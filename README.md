# CT3Emu

App Android che fa girare Captain Tsubasa III (SNES) usando **[webretro](https://github.com/BinBashBanana/webretro)**
(RetroArch compilato in WebAssembly) dentro una WebView, invece di un core emulatore
nativo scritto a mano. Nessun codice C/JNI/NDK: l'intero emulatore (core `snes9x`
incluso) è JavaScript + WebAssembly già pronto e testato da migliaia di utenti,
caricato interamente offline dagli assets dell'app.

Questo è un cambio di approccio deliberato rispetto a una versione precedente basata
su un core libretro nativo (compilato con NDK): quella strada continuava a fallire
all'avvio senza un modo affidabile di leggere il log di crash. Questa versione elimina
l'intera categoria di bug (mismatch JNI, gestione buffer nativa, linking NDK) affidandosi
a un progetto maturo e già funzionante.

## Come funziona

- `app/src/main/assets/webretro/` — build di webretro (versione ridotta: solo il core
  `snes9x`, non tutti i ~24 core disponibili, per tenere l'APK leggero), con la ROM già
  dentro in `roms/game.sfc`.
- `app/src/main/java/com/ct3/emu/MainActivity.kt` — una `WebView` che carica
  `index.html` di webretro tramite `WebViewAssetLoader` (serve gli assets locali su un
  dominio virtuale `https://appassets.androidplatform.net/...`, necessario perché
  webretro usa `fetch()`/IndexedDB, che su `file://` diretto sono limitati o bloccati).
  L'URL include `?core=snes9x&rom=game.sfc&nobundle&forcestartbutton`, quindi il gioco
  parte già selezionato, senza mostrare il menu di scelta core/ROM di webretro.
- Il pad virtuale (stessi pulsanti di prima: D-pad, A/B/X/Y, L/R, Start/Select, più
  SALVA/CARICA) invia veri eventi tastiera (`KeyboardEvent`) dentro la pagina via
  `WebView.evaluateJavascript`, sugli stessi tasti che webretro si aspetta di default
  (frecce per il D-pad, H/G/Y/T per A/B/X/Y, ecc. — vedi `defaultKeybinds` in
  `assets/webretro/assets/base.js`). SALVA/CARICA sono mappati su F2/F3, gli hotkey di
  save-state **nativi di webretro** — non abbiamo dovuto reimplementare nulla.
- `.github/workflows/build-apk.yml` — CI molto più semplice di prima: **niente NDK,
  niente compilazione di core nativi**. Solo un build Gradle/Android standard.

## Salvataggi

webretro salva stati di gioco e SRAM in **IndexedDB**, nello storage privato della
WebView (persistente tra un avvio e l'altro dell'app, sopravvive a chiusura/riapertura,
si perde solo se disinstalli l'app o cancelli i dati dell'app da Android). SALVA/CARICA
(F2/F3) fanno save-state completo, non solo SRAM.

## Limiti noti

- L'APK generato dalla CI è **debug, non firmato**: va bene per uso personale.
- Il pad virtuale invia eventi tastiera sintetici: se webretro dovesse cambiare i
  keybind di default in una versione futura, andrebbero riallineati in
  `setupGamepad()` dentro `MainActivity.kt`.
- Prestazioni: girare un core WASM dentro una WebView ha un overhead maggiore rispetto
  a un core nativo, ma per un gioco SNES (hardware anni '90) qualunque telefono recente
  regge tranquillamente 60fps.
- Se in futuro vuoi aggiornare webretro a una versione più recente, basta sostituire il
  contenuto di `app/src/main/assets/webretro/` (tranne `roms/game.sfc`, che è la tua ROM).
