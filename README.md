# CT3Emu - Captain Tsubasa III Emulator

APK compilata con **Cordova** usando il file `index.html` di webretro.

## 🎮 Features
- 📱 **Orientamento**: Landscape (Orizzontale)
- 💾 **Storage**: LocalStorage + IndexedDB
- 🌐 **Internet**: Abilitato
- 🎮 **JavaScript**: Completamente funzionante
- ⚡ **Lightweight**: Niente codice Android inutile

## 📲 Come compilare

### Automatico (GitHub Actions)
1. Fai push su `main`
2. Vai su **Actions** → **Build APK with Cordova**
3. Scarica l'APK dagli **Artifacts**

### Locale
```bash
npm install -g cordova
npm install
cordova platform add android
cordova build android --release
```

## 📁 Struttura
```
CT3Emu/
├── index.html          # App principale (webretro)
├── package.json        # Dipendenze Cordova
├── config.xml          # Configurazione app
└── .github/workflows/
    └── cordova-build.yml  # Workflow build
```

## 🔧 Permessi Android
- ✅ INTERNET
- ✅ READ_EXTERNAL_STORAGE
- ✅ WRITE_EXTERNAL_STORAGE
- ✅ ACCESS_NETWORK_STATE
