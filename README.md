# CT3Emu — Minimal Cordova APK

Progetto Cordova ridotto al minimo per eseguire `index.html` come applicazione Android.

## Struttura

```text
CT3Emu/
├── index.html
├── package.json
├── config.xml
├── README.md
└── .github/workflows/
    └── cordova-build.yml
```

`index.html` è volutamente l'unico file applicativo: la versione presente nel progetto contiene già il codice webretro necessario, incluso il core SNES9x e la ROM incorporati nel file.

## Build locale

```bash
npm install
npm run prepare
npm run build:android
```

L'APK di debug viene generato da Cordova e può essere usato per il test su Android.

Per una build release:

```bash
npm run build:android:release
```

La release non viene firmata automaticamente: per distribuzione pubblica serve una firma Android con una keystore propria.

## GitHub Actions

Il workflow `cordova-build.yml`:

1. installa Node.js 20 e Java 17;
2. installa le dipendenze NPM;
3. aggiunge `cordova-android`;
4. compila l'APK debug;
5. verifica che l'APK esista;
6. lo pubblica come Artifact.

Il workflow parte su push a `main` oppure manualmente da **Actions → Build APK with Cordova**.

## Perché questa struttura

Sono stati rimossi tutti i componenti Android nativi precedenti (`app/`, Kotlin, Gradle wrapper e workflow duplicati). Non vengono aggiunti permessi di storage esterno: LocalStorage e IndexedDB dell'app web non ne hanno bisogno.
