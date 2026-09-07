package com.ct3.emu

import android.annotation.SuppressLint
import android.graphics.Color
import android.os.Bundle
import android.util.Log
import android.view.MotionEvent
import android.view.View
import android.webkit.RenderProcessGoneDetail
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebSettings
import android.webkit.WebView
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.webkit.WebViewAssetLoader
import androidx.webkit.WebViewClientCompat

/**
 * Loads the bundled webretro (RetroArch/WebAssembly) build from app assets
 * and drives it with a native touch pad overlay. No custom native/JNI code
 * at all - the emulator core itself is the prebuilt snes9x WASM core that
 * ships with webretro, running entirely inside the WebView.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var webView: WebView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        // Any failure here (WebView init, missing view, bad URL...) is shown
        // on screen instead of silently crashing. If you land on this
        // screen, screenshot the text - it tells us exactly what broke.
        try {
            webView = findViewById(R.id.webView)
            setupWebView()
            setupGamepad()

            // core/rom are pre-selected so webretro skips its own picker UI and
            // boots straight into the game. nobundle skips its CDN asset fetch
            // (shader presets etc.) since we're fully offline. forcestartbutton
            // shows a tappable Start overlay, which also satisfies the browser's
            // "must start audio from a user gesture" requirement.
            webView.loadUrl(
                "https://appassets.androidplatform.net/assets/webretro/index.html" +
                    "?core=snes9x&rom=game.sfc&nobundle&forcestartbutton"
            )
        } catch (t: Throwable) {
            showFatalError("onCreate", t)
        }
    }

    private fun showFatalError(where: String, t: Throwable) {
        Log.e("MainActivity", "Fatal error in $where", t)
        val message = "Errore in $where:\n\n${Log.getStackTraceString(t)}"
        runOnUiThread {
            val textView = TextView(this).apply {
                text = message
                setTextColor(Color.WHITE)
                setBackgroundColor(Color.BLACK)
                textSize = 12f
                setPadding(32, 32, 32, 32)
                setTextIsSelectable(true)
            }
            setContentView(ScrollView(this).apply { addView(textView) })
        }
    }

    @SuppressLint("SetJavaScriptEnabled")
    private fun setupWebView() {
        val assetLoader = WebViewAssetLoader.Builder()
            .addPathHandler("/assets/", WebViewAssetLoader.AssetsPathHandler(this))
            .build()

        webView.settings.apply {
            javaScriptEnabled = true
            domStorageEnabled = true   // needed for IndexedDB (save states/SRAM)
            databaseEnabled = true
            mediaPlaybackRequiresUserGesture = false
            cacheMode = WebSettings.LOAD_DEFAULT
            setRenderPriority(WebSettings.RenderPriority.HIGH)
        }

        webView.webViewClient = object : WebViewClientCompat() {
            override fun shouldInterceptRequest(
                view: WebView,
                request: WebResourceRequest
            ): WebResourceResponse? {
                return assetLoader.shouldInterceptRequest(request.url)
            }

            // CRITICAL: if the WebView's renderer process crashes (e.g. out
            // of memory loading the WASM core) and this callback is NOT
            // overridden, Android's default behavior is to kill the whole
            // app process - which looks exactly like "opens, black screen,
            // closes immediately". Overriding it and returning true tells
            // Android "I've handled it, don't kill me" - we show an error
            // and let the person retry instead of a silent force-close.
            override fun onRenderProcessGone(
                view: WebView,
                detail: RenderProcessGoneDetail
            ): Boolean {
                val crashed = detail.didCrash()
                showFatalError(
                    "WebView renderer",
                    RuntimeException(
                        "Il processo di rendering della WebView e' terminato " +
                            "(didCrash=$crashed). Probabile causa: memoria insufficiente " +
                            "per caricare il core WASM su questo dispositivo."
                    )
                )
                view.destroy()
                return true
            }
        }
    }

    private data class KeyDef(val code: String, val keyCode: Int)

    /** Maps each on-screen button to the KeyboardEvent.code that webretro's
     *  DEFAULT keybinds expect (see assets/base.js -> defaultKeybinds), and
     *  dispatches synthetic key events straight into the page's `document`,
     *  exactly like a real keypress would. SALVA/CARICA map to webretro's
     *  built-in quick save-state hotkeys (F2/F3), so we get save states for
     *  free instead of reimplementing them. */
    private fun setupGamepad() {
        val buttonKeys = mapOf(
            R.id.btnUp to KeyDef("ArrowUp", 38),
            R.id.btnDown to KeyDef("ArrowDown", 40),
            R.id.btnLeft to KeyDef("ArrowLeft", 37),
            R.id.btnRight to KeyDef("ArrowRight", 39),
            R.id.btnA to KeyDef("KeyH", 72),
            R.id.btnB to KeyDef("KeyG", 71),
            R.id.btnX to KeyDef("KeyY", 89),
            R.id.btnY to KeyDef("KeyT", 84),
            R.id.btnL to KeyDef("KeyE", 69),
            R.id.btnR to KeyDef("KeyP", 80),
            R.id.btnStart to KeyDef("Enter", 13),
            R.id.btnSelect to KeyDef("Space", 32),
            R.id.btnQuickSave to KeyDef("F2", 113),
            R.id.btnQuickLoad to KeyDef("F3", 114)
        )

        buttonKeys.forEach { (viewId, keyDef) ->
            findViewById<View>(viewId).setOnTouchListener { _, event ->
                when (event.action) {
                    MotionEvent.ACTION_DOWN -> dispatchKey(keyDef, down = true)
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> dispatchKey(keyDef, down = false)
                }
                true
            }
        }
    }

    private fun dispatchKey(key: KeyDef, down: Boolean) {
        val type = if (down) "keydown" else "keyup"
        val js = """
            (function() {
                var e = new KeyboardEvent('$type', {
                    code: '${key.code}',
                    keyCode: ${key.keyCode},
                    which: ${key.keyCode},
                    bubbles: true,
                    cancelable: true
                });
                document.dispatchEvent(e);
            })();
        """.trimIndent()
        if (::webView.isInitialized) {
            webView.evaluateJavascript(js, null)
        }
    }
}
