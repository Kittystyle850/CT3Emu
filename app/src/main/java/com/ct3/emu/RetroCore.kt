package com.ct3.emu

/**
 * Thin JNI bridge to retro_shim.c / the loaded libretro core.
 * All methods are safe to call only from the dedicated emulation thread,
 * except [nativeSetButton] which is written from the UI thread and read
 * from the emulation thread (see retro_shim.c for the locking story).
 */
object RetroCore {
    init {
        System.loadLibrary("retro_shim")
    }

    external fun nativeLoadCore(corePath: String, systemDir: String, saveDir: String): Boolean
    external fun nativeLoadGame(romPath: String): Boolean
    external fun nativeRunFrame()
    external fun nativeGetFrame(outDims: IntArray): IntArray?
    external fun nativeGetAudio(outBuffer: ShortArray): Int
    external fun nativeSetButton(id: Int, down: Boolean)
    external fun nativeGetFps(): Double
    external fun nativeGetSampleRate(): Double
    external fun nativeReset()
    external fun nativeUnload()

    // Battery save (SRAM) - the actual in-game save data, persists across sessions.
    external fun nativeLoadSram(path: String): Boolean
    external fun nativeSaveSram(path: String): Boolean

    // Full save states - exact machine snapshot, for quick save/load mid-match.
    external fun nativeSaveState(path: String): Boolean
    external fun nativeLoadState(path: String): Boolean

    // Mirrors RETRO_DEVICE_ID_JOYPAD_* from libretro.h
    const val BUTTON_B = 0
    const val BUTTON_Y = 1
    const val BUTTON_SELECT = 2
    const val BUTTON_START = 3
    const val BUTTON_UP = 4
    const val BUTTON_DOWN = 5
    const val BUTTON_LEFT = 6
    const val BUTTON_RIGHT = 7
    const val BUTTON_A = 8
    const val BUTTON_X = 9
    const val BUTTON_L = 10
    const val BUTTON_R = 11
}
