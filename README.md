# CT3Emu

App Android minimale che carica un core libretro ([snes9x2010](https://github.com/libretro/snes9x2010))
e ti permette di caricare una tua ROM SNES posseduta legittimamente tramite file picker.
Non è un porting nativo del gioco: è un frontend libretro dedicato, compilato via GitHub Actions.

**Nessuna ROM è inclusa in questo repository.** La ROM viene scelta dall'utente a runtime
tramite Storage Access Framework e copiata solo nella sandbox privata dell'app
(`/data/data/com.ct3.emu/files`), mai nel repo o nell'APK.

## Come funziona

- `app/src/main/jni/retro_shim.c` — frontend libretro minimale: carica il core `.so` con
  `dlopen`, implementa le callback richieste dalla libretro API (video, audio, input,
  environment) ed espone il tutto via JNI a Kotlin.
- `app/src/main/java/com/ct3/emu/RetroCore.kt` — bridge JNI.
- `app/src/main/java/com/ct3/emu/EmulatorActivity.kt` — loop di emulazione, rendering su
  `SurfaceView`, audio via `AudioTrack`, pad virtuale touch.
- `.github/workflows/build-apk.yml` — CI che:
  1. clona il core `snes9x2010` da GitHub,
  2. lo compila con l'NDK per `armeabi-v7a`, `arm64-v8a`, `x86_64` usando il suo
     `jni/Android.mk` ufficiale,
  3. copia i `.so` risultanti in `app/src/main/assets/cores/<abi>/libretro.so`,
  4. builda l'APK (debug, non firmato) con Gradle,
  5. carica l'APK come artifact scaricabile dalla run della Action.

## Come usarlo

1. Crea un repo GitHub e pusha questo progetto.
2. Vai su **Actions → Build APK → Run workflow** (o pusha su `main`).
3. A build completata, scarica l'artifact `CT3Emu-debug-apk` dalla pagina della run.
4. Installa l'APK sul telefono (abilita "sorgenti sconosciute" se richiesto).
5. Apri l'app, tocca "Carica ROM", seleziona il file della tua ROM (`.smc`/`.sfc`,
   anche dentro uno `.zip` se il core lo supporta) dal file picker di Android.

## Includere la ROM direttamente nell'APK (opzionale)

Di default l'app mostra un pulsante "Carica ROM" e la ROM resta fuori dal repo/APK.
Se invece vuoi un APK già pronto con la tua ROM dentro (comodo per uso personale,
un solo file da installare), puoi bundlarla a build time:

1. **Rendi il repository privato** (Settings → Danger Zone → Change visibility →
   Private) prima di fare qualunque commit della ROM. Un repo pubblico rende il file
   scaricabile da chiunque abbia il link: questo sì sarebbe distribuzione pubblica di
   materiale protetto da copyright, a differenza del caricarla in locale sul tuo
   telefono, che è uso personale di una copia posseduta.
2. Se la tua ROM è dentro uno `.zip`, estrai il file `.smc`/`.sfc` vero e proprio
   (il core legge i byte grezzi della ROM, non l'archivio zip).
3. Copia/rinomina quel file in `app/src/main/assets/rom/game.rom` nel repo.
4. Fai commit e push (il repo dev'essere già privato a questo punto).
5. Rilancia la Action: l'app rileverà automaticamente la ROM negli assets e la
   caricherà all'avvio, senza mostrare il pulsante "Carica ROM".

Se in futuro vuoi tornare al file picker (es. per condividere il codice senza la
ROM), basta rimuovere `app/src/main/assets/rom/game.rom` e ricompilare.

## Salvataggi

- **Save automatico (SRAM)**: la SRAM della cartuccia (il vero "save di gioco") viene
  ricaricata all'avvio se esiste, salvata ogni ~15 secondi di gioco, e salvata di nuovo
  a ogni pausa/uscita dall'app. Il file vive in `files/save/game.srm`, privato dell'app.
- **Salvataggio rapido (save state)**: i pulsanti **SALVA**/**CARICA** in alto catturano
  o ripristinano lo stato esatto della macchina emulata (posizione palla, animazioni,
  timer di partita compresi) in un singolo slot, `files/save/quicksave.state`. Utile per
  interrompere una partita a metà e riprenderla esattamente da lì.
- Uscendo dall'app e tornando (senza che Android uccida il processo in background),
  il gioco riprende da dove l'hai lasciato senza dover ricaricare la ROM.

## Pad virtuale

Layout pensato per un gioco calcistico:
- **D-pad** (basso-sinistra) e **A/B/X/Y** (basso-destra) ingranditi a 64dp e con gap
  ridotti tra i tasti direzionali, per facilitare i movimenti in diagonale (dribbling)
  con un solo pollice.
- **L/R** in alto, larghi, per il dito indice quando tieni il telefono in orizzontale.
- **SALVA/CARICA** piccoli, subito sotto L/R, fuori dal percorso dei dorsali per
  evitare tocchi accidentali durante il gioco.
- **START/SELECT** in alto al centro.

Rimane comunque un layout "a occhio": le posizioni esatte (marginStart/marginEnd/
marginBottom in `activity_emulator.xml`) vanno probabilmente aggiustate di qualche dp
una volta viste sul tuo dispositivo reale — non c'è modo di tararle alla perfezione
senza provarle a mano.

## Limiti noti / cose da rifinire

- L'APK generato dalla CI è **debug, non firmato**: va bene per test su un tuo dispositivo,
  ma se vuoi pubblicarlo o distribuirlo devi aggiungere una keystore di release e
  firmare l'APK (posso aggiungerlo se ti serve).
- Il D-pad è a 4 pulsanti separati, non un vero pad analogico a 8 direzioni: le diagonali
  funzionano solo se il dito riesce a "rollare" da un pulsante all'altro velocemente.
  Un pad più preciso richiederebbe una View custom con zone touch — fattibile se serve.
- Non c'è audio resampling: si presume che `AudioTrack` accetti il sample rate nativo
  del core (di solito ~32000 Hz per SNES), il che è vero sulla stragrande maggioranza
  dei dispositivi Android moderni.
- Il primo build in CI potrebbe comunque richiedere un piccolo aggiustamento (versioni di
  Android Gradle Plugin/NDK che cambiano nel tempo): ho aggiunto controlli che fanno
  fallire la build con un messaggio chiaro invece di un errore criptico a metà, così è
  più facile capire subito dove intervenire. Se fallisce, mandami il log e sistemiamo.

## Perché questo approccio e non un porting nativo

Un vero porting nativo richiederebbe un progetto di decompilazione/disassembly completo
del gioco (come esistono per Super Mario World o Zelda: A Link to the Past), che per
Captain Tsubasa III non esiste. L'emulazione tramite core libretro è quindi l'unica
strada concreta per far girare il gioco su Android partendo dalla tua copia posseduta.
