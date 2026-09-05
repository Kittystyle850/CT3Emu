package com.ct3.emu

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Rect
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.widget.Button
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import java.io.File
import java.io.FileOutputStream
import kotlin.concurrent.thread

class EmulatorActivity : AppCompatActivity() {

    private lateinit var surfaceView: SurfaceView
    private var audioTrack: AudioTrack? = null

    @Volatile private var running = false
    private var emuThread: Thread? = null

    private var frameBitmap: Bitmap? = null
    private var frameW = 0
    private var frameH = 0

    // Set once a core+game are loaded; native state then persists for the
    // life of the process, so onResume can just restart the loop.
    private var gameLoaded = false
    private var fps = 60.0

    private lateinit var sramFile: File
    private lateinit var quickSaveFile: File

    // Autosave the battery save every ~15s of gameplay so progress survives
    // a crash or a task-kill, not just a clean pause/exit.
    private var framesSinceAutosave = 0
    private val autosaveIntervalFrames get() = (fps * 15).toInt()

    private val pickRom =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
            uri?.let { onRomPicked(it) }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_emulator)

        surfaceView = findViewById(R.id.surfaceView)
        sramFile = File(filesDir, "save/game.srm")
        quickSaveFile = File(filesDir, "save/quicksave.state")

        setupGamepad()
        setupMenuButtons()

        findViewById<Button>(R.id.btnLoadRom).setOnClickListener {
            pickRom.launch(arrayOf("*/*"))
        }

        // If a ROM was bundled into the APK at build time (assets/rom/game.rom),
        // load it automatically and skip the file picker entirely.
        val bundledRomAssetPath = "rom/game.rom"
        val hasBundledRom = try {
            assets.open(bundledRomAssetPath).use { true }
        } catch (_: Exception) {
            false
        }
        if (hasBundledRom) {
            findViewById<View>(R.id.btnLoadRom).visibility = View.GONE
            val romFile = File(filesDir, "current_rom.bin")
            assets.open(bundledRomAssetPath).use { input ->
                FileOutputStream(romFile).use { output -> input.copyTo(output) }
            }
            loadAndStart(romFile)
        }
    }

    private fun onRomPicked(uri: Uri) {
        val romFile = File(filesDir, "current_rom.bin")
        contentResolver.openInputStream(uri)?.use { input ->
            FileOutputStream(romFile).use { output -> input.copyTo(output) }
        }
        loadAndStart(romFile)
    }

    private fun loadAndStart(romFile: File) {
        val abi = Build.SUPPORTED_ABIS.firstOrNull { supportedAbis.contains(it) }
            ?: Build.SUPPORTED_ABIS[0]

        val coreFile = File(filesDir, "libretro_core.so")
        assets.open("cores/$abi/libretro.so").use { input ->
            FileOutputStream(coreFile).use { output -> input.copyTo(output) }
        }

        val systemDir = File(filesDir, "system").apply { mkdirs() }
        val saveDir = File(filesDir, "save").apply { mkdirs() }

        if (!RetroCore.nativeLoadCore(coreFile.absolutePath, systemDir.absolutePath, saveDir.absolutePath)) {
            toast("Impossibile caricare il core emulatore")
            return
        }

        if (!RetroCore.nativeLoadGame(romFile.absolutePath)) {
            toast("Impossibile caricare la ROM (formato non supportato?)")
            return
        }

        // Restore the in-game battery save, if one exists from a previous session.
        if (sramFile.exists()) {
            RetroCore.nativeLoadSram(sramFile.absolutePath)
        }

        gameLoaded = true
        findViewById<View>(R.id.btnLoadRom).visibility = View.GONE

        fps = RetroCore.nativeGetFps().takeIf { it > 0 } ?: 60.0
        val sampleRate = RetroCore.nativeGetSampleRate().toInt().takeIf { it > 0 } ?: 32000
        setupAudioTrack(sampleRate)

        startLoop()
    }

    /** (Re)starts the emulation thread. Safe to call after onPause/onResume
     *  since core+game state lives in native memory for the process lifetime. */
    private fun startLoop() {
        if (running || !gameLoaded) return
        running = true
        val frameIntervalNanos = (1_000_000_000.0 / fps).toLong()

        emuThread = thread(start = true, name = "EmuThread") {
            val outDims = IntArray(2)
            val audioBuf = ShortArray(4096)

            var nextFrameTime = System.nanoTime()
            while (running) {
                RetroCore.nativeRunFrame()

                RetroCore.nativeGetFrame(outDims)?.let { pixels ->
                    renderFrame(pixels, outDims[0], outDims[1])
                }

                val framesWritten = RetroCore.nativeGetAudio(audioBuf)
                if (framesWritten > 0) {
                    audioTrack?.write(audioBuf, 0, framesWritten * 2)
                }

                framesSinceAutosave++
                if (framesSinceAutosave >= autosaveIntervalFrames) {
                    framesSinceAutosave = 0
                    RetroCore.nativeSaveSram(sramFile.absolutePath)
                }

                nextFrameTime += frameIntervalNanos
                val sleepNanos = nextFrameTime - System.nanoTime()
                if (sleepNanos > 0) {
                    try {
                        Thread.sleep(sleepNanos / 1_000_000, (sleepNanos % 1_000_000).toInt())
                    } catch (_: InterruptedException) {
                    }
                } else {
                    nextFrameTime = System.nanoTime() // fell behind, resync
                }
            }
        }
    }

    /** Stops the emulation thread and persists the battery save. Does NOT
     *  tear down the native core, so a later resume/restart is instant. */
    private fun stopLoopAndSave() {
        if (!running) return
        running = false
        emuThread?.join(500)
        audioTrack?.pause()
        audioTrack?.flush()
        if (gameLoaded) {
            RetroCore.nativeSaveSram(sramFile.absolutePath)
        }
    }

    private fun setupAudioTrack(sampleRate: Int) {
        val minBufSize = AudioTrack.getMinBufferSize(
            sampleRate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT
        )
        audioTrack = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_GAME)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .build()
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setSampleRate(sampleRate)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .build()
            )
            .setBufferSizeInBytes(minBufSize * 2)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        audioTrack?.play()
    }

    private fun renderFrame(pixels: IntArray, width: Int, height: Int) {
        if (width <= 0 || height <= 0) return

        if (frameBitmap == null || frameW != width || frameH != height) {
            frameBitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
            frameW = width
            frameH = height
        }
        frameBitmap?.setPixels(pixels, 0, width, 0, 0, width, height)

        val holder: SurfaceHolder = surfaceView.holder
        val canvas: Canvas = holder.lockCanvas() ?: return
        try {
            canvas.drawColor(android.graphics.Color.BLACK)
            val viewW = canvas.width
            val viewH = canvas.height
            val scale = minOf(viewW.toFloat() / width, viewH.toFloat() / height)
            val destW = (width * scale).toInt()
            val destH = (height * scale).toInt()
            val left = (viewW - destW) / 2
            val top = (viewH - destH) / 2
            val destRect = Rect(left, top, left + destW, top + destH)
            frameBitmap?.let { canvas.drawBitmap(it, null, destRect, null) }
        } finally {
            holder.unlockCanvasAndPost(canvas)
        }
    }

    private fun setupGamepad() {
        mapOf(
            R.id.btnUp to RetroCore.BUTTON_UP,
            R.id.btnDown to RetroCore.BUTTON_DOWN,
            R.id.btnLeft to RetroCore.BUTTON_LEFT,
            R.id.btnRight to RetroCore.BUTTON_RIGHT,
            R.id.btnA to RetroCore.BUTTON_A,
            R.id.btnB to RetroCore.BUTTON_B,
            R.id.btnX to RetroCore.BUTTON_X,
            R.id.btnY to RetroCore.BUTTON_Y,
            R.id.btnL to RetroCore.BUTTON_L,
            R.id.btnR to RetroCore.BUTTON_R,
            R.id.btnStart to RetroCore.BUTTON_START,
            R.id.btnSelect to RetroCore.BUTTON_SELECT,
        ).forEach { (viewId, buttonId) ->
            findViewById<View>(viewId).setOnTouchListener { _, event ->
                when (event.action) {
                    MotionEvent.ACTION_DOWN -> RetroCore.nativeSetButton(buttonId, true)
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL ->
                        RetroCore.nativeSetButton(buttonId, false)
                }
                true
            }
        }
    }

    private fun setupMenuButtons() {
        findViewById<View>(R.id.btnQuickSave).setOnClickListener {
            if (!gameLoaded) return@setOnClickListener
            val ok = RetroCore.nativeSaveState(quickSaveFile.absolutePath)
            toast(if (ok) "Salvato" else "Salvataggio non riuscito")
        }
        findViewById<View>(R.id.btnQuickLoad).setOnClickListener {
            if (!gameLoaded || !quickSaveFile.exists()) {
                toast("Nessun salvataggio rapido presente")
                return@setOnClickListener
            }
            val ok = RetroCore.nativeLoadState(quickSaveFile.absolutePath)
            toast(if (ok) "Caricato" else "Caricamento non riuscito")
        }
    }

    private fun toast(msg: String) {
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
    }

    override fun onPause() {
        stopLoopAndSave()
        super.onPause()
    }

    override fun onResume() {
        super.onResume()
        // If a game was already loaded before this pause (i.e. the process
        // survived backgrounding), just restart the loop - no reload needed.
        if (gameLoaded) {
            audioTrack?.play()
            startLoop()
        }
    }

    override fun onDestroy() {
        stopLoopAndSave()
        if (gameLoaded) {
            RetroCore.nativeUnload()
        }
        audioTrack?.release()
        super.onDestroy()
    }

    companion object {
        // ABIs we know the CI workflow builds the core for.
        private val supportedAbis = setOf("arm64-v8a", "armeabi-v7a", "x86_64")
    }
}
