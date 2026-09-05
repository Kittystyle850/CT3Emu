// retro_shim.c
//
// Minimal libretro FRONTEND implemented as a JNI shim.
// It dlopen()s a libretro core (e.g. libretro.so built from snes9x2010),
// wires up the callbacks required by the libretro API, and exposes a
// small JNI surface so a Kotlin Activity can drive the emulation loop,
// pull video frames, pull audio samples, and push controller input.
//
// This file intentionally implements only what is needed to run a single
// SNES core with a single cartridge loaded from local storage. It is not
// a general-purpose RetroArch replacement.

#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

#define TAG "retro_shim"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ---------------------------------------------------------------------
// Minimal subset of libretro.h needed here (values verified against the
// upstream libretro-common/include/libretro.h shipped with the core).
// ---------------------------------------------------------------------

#define RETRO_ENVIRONMENT_SET_ROTATION            1
#define RETRO_ENVIRONMENT_GET_OVERSCAN            2
#define RETRO_ENVIRONMENT_GET_CAN_DUPE            3
#define RETRO_ENVIRONMENT_SET_PIXEL_FORMAT        10
#define RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS   11
#define RETRO_ENVIRONMENT_GET_LOG_INTERFACE       27
#define RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY    9
#define RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY      31
#define RETRO_ENVIRONMENT_GET_VARIABLE            15
#define RETRO_ENVIRONMENT_SET_VARIABLES           16
#define RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE     17

// RETRO_MEMORY_* id for retro_get_memory_data/size (battery-backed save RAM)
#define RETRO_MEMORY_SAVE_RAM 0

#define RETRO_DEVICE_JOYPAD 1

#define RETRO_DEVICE_ID_JOYPAD_B      0
#define RETRO_DEVICE_ID_JOYPAD_Y      1
#define RETRO_DEVICE_ID_JOYPAD_SELECT 2
#define RETRO_DEVICE_ID_JOYPAD_START  3
#define RETRO_DEVICE_ID_JOYPAD_UP     4
#define RETRO_DEVICE_ID_JOYPAD_DOWN   5
#define RETRO_DEVICE_ID_JOYPAD_LEFT   6
#define RETRO_DEVICE_ID_JOYPAD_RIGHT  7
#define RETRO_DEVICE_ID_JOYPAD_A      8
#define RETRO_DEVICE_ID_JOYPAD_X      9
#define RETRO_DEVICE_ID_JOYPAD_L      10
#define RETRO_DEVICE_ID_JOYPAD_R      11
#define RETRO_DEVICE_ID_JOYPAD_L2     12
#define RETRO_DEVICE_ID_JOYPAD_R2     13
#define RETRO_DEVICE_ID_JOYPAD_L3     14
#define RETRO_DEVICE_ID_JOYPAD_R3     15
#define RETRO_NUM_JOYPAD_BUTTONS      16

enum retro_pixel_format {
    RETRO_PIXEL_FORMAT_0RGB1555 = 0,
    RETRO_PIXEL_FORMAT_XRGB8888 = 1,
    RETRO_PIXEL_FORMAT_RGB565   = 2,
};

struct retro_game_geometry {
    unsigned base_width;
    unsigned base_height;
    unsigned max_width;
    unsigned max_height;
    float    aspect_ratio;
};

struct retro_system_timing {
    double fps;
    double sample_rate;
};

struct retro_system_av_info {
    struct retro_game_geometry geometry;
    struct retro_system_timing timing;
};

struct retro_game_info {
    const char *path;
    const void *data;
    size_t      size;
    const char *meta;
};

struct retro_system_info {
    const char *library_name;
    const char *library_version;
    const char *valid_extensions;
    bool        need_fullpath;
    bool        block_extract;
};

typedef bool   (*retro_environment_t)(unsigned cmd, void *data);
typedef void   (*retro_video_refresh_t)(const void *data, unsigned width, unsigned height, size_t pitch);
typedef void   (*retro_audio_sample_t)(int16_t left, int16_t right);
typedef size_t (*retro_audio_sample_batch_t)(const int16_t *data, size_t frames);
typedef void   (*retro_input_poll_t)(void);
typedef int16_t (*retro_input_state_t)(unsigned port, unsigned device, unsigned index, unsigned id);

typedef void (*retro_set_environment_t)(retro_environment_t);
typedef void (*retro_set_video_refresh_tf)(retro_video_refresh_t);
typedef void (*retro_set_audio_sample_tf)(retro_audio_sample_t);
typedef void (*retro_set_audio_sample_batch_tf)(retro_audio_sample_batch_t);
typedef void (*retro_set_input_poll_tf)(retro_input_poll_t);
typedef void (*retro_set_input_state_tf)(retro_input_state_t);
typedef void (*retro_init_t)(void);
typedef void (*retro_deinit_t)(void);
typedef unsigned (*retro_api_version_t)(void);
typedef void (*retro_get_system_info_t)(struct retro_system_info *);
typedef void (*retro_get_system_av_info_t)(struct retro_system_av_info *);
typedef void (*retro_set_controller_port_device_t)(unsigned, unsigned);
typedef void (*retro_reset_t)(void);
typedef void (*retro_run_t)(void);
typedef bool (*retro_load_game_t)(const struct retro_game_info *);
typedef void (*retro_unload_game_t)(void);
typedef size_t (*retro_serialize_size_t)(void);
typedef bool (*retro_serialize_t)(void *data, size_t size);
typedef bool (*retro_unserialize_t)(const void *data, size_t size);
typedef void   *(*retro_get_memory_data_t)(unsigned id);
typedef size_t  (*retro_get_memory_size_t)(unsigned id);

// ---------------------------------------------------------------------
// Core handle + function pointers
// ---------------------------------------------------------------------

static void *g_core_handle = NULL;

static retro_set_environment_t          core_set_environment;
static retro_set_video_refresh_tf       core_set_video_refresh;
static retro_set_audio_sample_tf        core_set_audio_sample;
static retro_set_audio_sample_batch_tf  core_set_audio_sample_batch;
static retro_set_input_poll_tf          core_set_input_poll;
static retro_set_input_state_tf         core_set_input_state;
static retro_init_t                     core_init;
static retro_deinit_t                   core_deinit;
static retro_api_version_t              core_api_version;
static retro_get_system_info_t          core_get_system_info;
static retro_get_system_av_info_t       core_get_system_av_info;
static retro_set_controller_port_device_t core_set_controller_port_device;
static retro_reset_t                    core_reset;
static retro_run_t                      core_run;
static retro_load_game_t                core_load_game;
static retro_unload_game_t              core_unload_game;
static retro_serialize_size_t           core_serialize_size;
static retro_serialize_t                core_serialize;
static retro_unserialize_t              core_unserialize;
static retro_get_memory_data_t          core_get_memory_data;
static retro_get_memory_size_t          core_get_memory_size;

// ---------------------------------------------------------------------
// Emulation state shared with Java
// ---------------------------------------------------------------------

static enum retro_pixel_format g_pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;

static uint32_t *g_argb_frame = NULL;   // converted ARGB_8888 frame buffer
static unsigned  g_frame_w = 0, g_frame_h = 0;
static pthread_mutex_t g_frame_lock = PTHREAD_MUTEX_INITIALIZER;

static double g_fps = 60.0;
static double g_sample_rate = 32000.0;

// Simple ring buffer for interleaved stereo 16-bit audio samples.
#define AUDIO_RING_CAPACITY (1 << 16) // in int16_t units (L/R interleaved)
static int16_t g_audio_ring[AUDIO_RING_CAPACITY];
static volatile unsigned g_audio_write = 0;
static volatile unsigned g_audio_read = 0;
static pthread_mutex_t g_audio_lock = PTHREAD_MUTEX_INITIALIZER;

static volatile int16_t g_joypad_state = 0; // bitmask, bit index = RETRO_DEVICE_ID_JOYPAD_*

static char g_system_dir[512];
static char g_save_dir[512];

// ---------------------------------------------------------------------
// libretro callbacks
// ---------------------------------------------------------------------

static bool env_cb(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *(bool *)data = true;
            return true;

        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            g_pixel_format = *(const enum retro_pixel_format *)data;
            LOGI("Core requested pixel format %d", (int)g_pixel_format);
            return true;

        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *(const char **)data = g_system_dir;
            return true;

        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *(const char **)data = g_save_dir;
            return true;

        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        case RETRO_ENVIRONMENT_GET_VARIABLE:
        case RETRO_ENVIRONMENT_SET_VARIABLES:
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        case RETRO_ENVIRONMENT_SET_ROTATION:
        case RETRO_ENVIRONMENT_GET_OVERSCAN:
        default:
            // Not supported / not needed for a minimal single-core frontend.
            return false;
    }
}

// Converts one incoming frame to ARGB_8888 into g_argb_frame.
static void video_refresh_cb(const void *data, unsigned width, unsigned height, size_t pitch) {
    if (!data || width == 0 || height == 0) {
        return; // duplicate frame (GET_CAN_DUPE == true) - nothing to do
    }

    pthread_mutex_lock(&g_frame_lock);

    if (g_frame_w != width || g_frame_h != height || g_argb_frame == NULL) {
        free(g_argb_frame);
        g_argb_frame = (uint32_t *)malloc((size_t)width * height * sizeof(uint32_t));
        g_frame_w = width;
        g_frame_h = height;
    }

    const uint8_t *src = (const uint8_t *)data;

    for (unsigned y = 0; y < height; y++) {
        const uint8_t *row = src + y * pitch;
        uint32_t *dst = g_argb_frame + (size_t)y * width;

        if (g_pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) {
            memcpy(dst, row, (size_t)width * sizeof(uint32_t));
        } else if (g_pixel_format == RETRO_PIXEL_FORMAT_RGB565) {
            const uint16_t *row16 = (const uint16_t *)row;
            for (unsigned x = 0; x < width; x++) {
                uint16_t px = row16[x];
                uint8_t r = (uint8_t)(((px >> 11) & 0x1F) * 255 / 31);
                uint8_t g = (uint8_t)(((px >> 5) & 0x3F) * 255 / 63);
                uint8_t b = (uint8_t)((px & 0x1F) * 255 / 31);
                dst[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        } else { // RETRO_PIXEL_FORMAT_0RGB1555
            const uint16_t *row16 = (const uint16_t *)row;
            for (unsigned x = 0; x < width; x++) {
                uint16_t px = row16[x];
                uint8_t r = (uint8_t)(((px >> 10) & 0x1F) * 255 / 31);
                uint8_t g = (uint8_t)(((px >> 5) & 0x1F) * 255 / 31);
                uint8_t b = (uint8_t)((px & 0x1F) * 255 / 31);
                dst[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
    }

    pthread_mutex_unlock(&g_frame_lock);
}

static void push_audio_frames(const int16_t *data, size_t frames) {
    pthread_mutex_lock(&g_audio_lock);
    for (size_t i = 0; i < frames; i++) {
        unsigned next = (g_audio_write + 2) % AUDIO_RING_CAPACITY;
        if (next == g_audio_read) {
            break; // ring full, drop remaining samples rather than block the core
        }
        g_audio_ring[g_audio_write]     = data[i * 2];
        g_audio_ring[g_audio_write + 1] = data[i * 2 + 1];
        g_audio_write = next;
    }
    pthread_mutex_unlock(&g_audio_lock);
}

static void audio_sample_cb(int16_t left, int16_t right) {
    int16_t frame[2] = { left, right };
    push_audio_frames(frame, 1);
}

static size_t audio_sample_batch_cb(const int16_t *data, size_t frames) {
    push_audio_frames(data, frames);
    return frames;
}

static void input_poll_cb(void) {
    // No-op: button state is updated directly by JNI calls from the UI thread.
}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)index;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD || id >= RETRO_NUM_JOYPAD_BUTTONS) {
        return 0;
    }
    return (g_joypad_state & (1 << id)) ? 1 : 0;
}

// ---------------------------------------------------------------------
// JNI exports
// ---------------------------------------------------------------------

static void *load_symbol(void *handle, const char *name) {
    void *sym = dlsym(handle, name);
    if (!sym) {
        LOGE("Missing symbol in core: %s", name);
    }
    return sym;
}

JNIEXPORT jboolean JNICALL
Java_com_ct3_emu_RetroCore_nativeLoadCore(JNIEnv *env, jobject thiz,
                                           jstring corePath, jstring systemDir, jstring saveDir) {
    (void)thiz;
    const char *path = (*env)->GetStringUTFChars(env, corePath, NULL);

    g_core_handle = dlopen(path, RTLD_LAZY);
    (*env)->ReleaseStringUTFChars(env, corePath, path);

    if (!g_core_handle) {
        LOGE("dlopen failed: %s", dlerror());
        return JNI_FALSE;
    }

    const char *sysDir = (*env)->GetStringUTFChars(env, systemDir, NULL);
    strncpy(g_system_dir, sysDir, sizeof(g_system_dir) - 1);
    (*env)->ReleaseStringUTFChars(env, systemDir, sysDir);

    const char *svDir = (*env)->GetStringUTFChars(env, saveDir, NULL);
    strncpy(g_save_dir, svDir, sizeof(g_save_dir) - 1);
    (*env)->ReleaseStringUTFChars(env, saveDir, svDir);

    core_set_environment          = (retro_set_environment_t)load_symbol(g_core_handle, "retro_set_environment");
    core_set_video_refresh        = (retro_set_video_refresh_tf)load_symbol(g_core_handle, "retro_set_video_refresh");
    core_set_audio_sample         = (retro_set_audio_sample_tf)load_symbol(g_core_handle, "retro_set_audio_sample");
    core_set_audio_sample_batch   = (retro_set_audio_sample_batch_tf)load_symbol(g_core_handle, "retro_set_audio_sample_batch");
    core_set_input_poll           = (retro_set_input_poll_tf)load_symbol(g_core_handle, "retro_set_input_poll");
    core_set_input_state          = (retro_set_input_state_tf)load_symbol(g_core_handle, "retro_set_input_state");
    core_init                     = (retro_init_t)load_symbol(g_core_handle, "retro_init");
    core_deinit                   = (retro_deinit_t)load_symbol(g_core_handle, "retro_deinit");
    core_api_version              = (retro_api_version_t)load_symbol(g_core_handle, "retro_api_version");
    core_get_system_info          = (retro_get_system_info_t)load_symbol(g_core_handle, "retro_get_system_info");
    core_get_system_av_info       = (retro_get_system_av_info_t)load_symbol(g_core_handle, "retro_get_system_av_info");
    core_set_controller_port_device = (retro_set_controller_port_device_t)load_symbol(g_core_handle, "retro_set_controller_port_device");
    core_reset                    = (retro_reset_t)load_symbol(g_core_handle, "retro_reset");
    core_run                      = (retro_run_t)load_symbol(g_core_handle, "retro_run");
    core_load_game                = (retro_load_game_t)load_symbol(g_core_handle, "retro_load_game");
    core_unload_game              = (retro_unload_game_t)load_symbol(g_core_handle, "retro_unload_game");
    core_serialize_size           = (retro_serialize_size_t)load_symbol(g_core_handle, "retro_serialize_size");
    core_serialize                = (retro_serialize_t)load_symbol(g_core_handle, "retro_serialize");
    core_unserialize              = (retro_unserialize_t)load_symbol(g_core_handle, "retro_unserialize");
    core_get_memory_data          = (retro_get_memory_data_t)load_symbol(g_core_handle, "retro_get_memory_data");
    core_get_memory_size          = (retro_get_memory_size_t)load_symbol(g_core_handle, "retro_get_memory_size");

    if (!core_set_environment || !core_init || !core_load_game || !core_run) {
        LOGE("Core missing required libretro symbols");
        return JNI_FALSE;
    }

    core_set_environment(env_cb);
    core_set_video_refresh(video_refresh_cb);
    core_set_audio_sample(audio_sample_cb);
    core_set_audio_sample_batch(audio_sample_batch_cb);
    core_set_input_poll(input_poll_cb);
    core_set_input_state(input_state_cb);

    core_init();
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_ct3_emu_RetroCore_nativeLoadGame(JNIEnv *env, jobject thiz, jstring romPath) {
    (void)thiz;
    const char *path = (*env)->GetStringUTFChars(env, romPath, NULL);

    struct retro_game_info info;
    info.path = path;
    info.data = NULL;
    info.size = 0;
    info.meta = NULL;

    bool ok = core_load_game(&info);
    (*env)->ReleaseStringUTFChars(env, romPath, path);

    if (ok) {
        struct retro_system_av_info av;
        core_get_system_av_info(&av);
        g_fps = av.timing.fps > 0 ? av.timing.fps : 60.0;
        g_sample_rate = av.timing.sample_rate > 0 ? av.timing.sample_rate : 32000.0;
        core_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
        LOGI("Game loaded: %ux%u @ %.2f fps, sample_rate=%.0f",
             av.geometry.base_width, av.geometry.base_height, g_fps, g_sample_rate);
    } else {
        LOGE("retro_load_game failed for %s", path);
    }

    return ok ? JNI_TRUE : JNI_FALSE;
}

// ---------------------------------------------------------------------
// SRAM (battery save) persistence
// ---------------------------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_ct3_emu_RetroCore_nativeLoadSram(JNIEnv *env, jobject thiz, jstring path) {
    (void)thiz;
    if (!core_get_memory_data || !core_get_memory_size) return JNI_FALSE;

    void *sram = core_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t sram_size = core_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!sram || sram_size == 0) return JNI_FALSE;

    const char *cpath = (*env)->GetStringUTFChars(env, path, NULL);
    FILE *f = fopen(cpath, "rb");
    (*env)->ReleaseStringUTFChars(env, path, cpath);
    if (!f) return JNI_FALSE; // no existing save yet, nothing to load

    size_t read = fread(sram, 1, sram_size, f);
    fclose(f);
    LOGI("SRAM loaded: %zu bytes (expected up to %zu)", read, sram_size);
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_ct3_emu_RetroCore_nativeSaveSram(JNIEnv *env, jobject thiz, jstring path) {
    (void)thiz;
    if (!core_get_memory_data || !core_get_memory_size) return JNI_FALSE;

    void *sram = core_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t sram_size = core_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!sram || sram_size == 0) return JNI_FALSE; // this core/game has no battery save

    const char *cpath = (*env)->GetStringUTFChars(env, path, NULL);
    FILE *f = fopen(cpath, "wb");
    (*env)->ReleaseStringUTFChars(env, path, cpath);
    if (!f) {
        LOGE("Could not open SRAM file for writing");
        return JNI_FALSE;
    }

    size_t written = fwrite(sram, 1, sram_size, f);
    fclose(f);
    return (written == sram_size) ? JNI_TRUE : JNI_FALSE;
}

// ---------------------------------------------------------------------
// Full save states (retro_serialize) - independent of SRAM, captures the
// exact emulated machine state so you can resume mid-match.
// ---------------------------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_ct3_emu_RetroCore_nativeSaveState(JNIEnv *env, jobject thiz, jstring path) {
    (void)thiz;
    if (!core_serialize_size || !core_serialize) return JNI_FALSE;

    size_t size = core_serialize_size();
    if (size == 0) return JNI_FALSE;

    void *buf = malloc(size);
    if (!buf) return JNI_FALSE;

    bool ok = core_serialize(buf, size);
    if (ok) {
        const char *cpath = (*env)->GetStringUTFChars(env, path, NULL);
        FILE *f = fopen(cpath, "wb");
        (*env)->ReleaseStringUTFChars(env, path, cpath);
        if (f) {
            size_t written = fwrite(buf, 1, size, f);
            fclose(f);
            ok = (written == size);
        } else {
            ok = false;
        }
    }

    free(buf);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_ct3_emu_RetroCore_nativeLoadState(JNIEnv *env, jobject thiz, jstring path) {
    (void)thiz;
    if (!core_unserialize) return JNI_FALSE;

    const char *cpath = (*env)->GetStringUTFChars(env, path, NULL);
    FILE *f = fopen(cpath, "rb");
    (*env)->ReleaseStringUTFChars(env, path, cpath);
    if (!f) return JNI_FALSE;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return JNI_FALSE;
    }

    void *buf = malloc((size_t)size);
    if (!buf) {
        fclose(f);
        return JNI_FALSE;
    }

    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);

    bool ok = (read == (size_t)size) && core_unserialize(buf, (size_t)size);
    free(buf);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_ct3_emu_RetroCore_nativeRunFrame(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    if (core_run) core_run();
}

JNIEXPORT jdouble JNICALL
Java_com_ct3_emu_RetroCore_nativeGetFps(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    return g_fps;
}

JNIEXPORT jdouble JNICALL
Java_com_ct3_emu_RetroCore_nativeGetSampleRate(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    return g_sample_rate;
}

// Copies the current frame into an int[] (ARGB_8888) sized w*h, and
// returns a 2-element int[] via the out params for width/height.
JNIEXPORT jintArray JNICALL
Java_com_ct3_emu_RetroCore_nativeGetFrame(JNIEnv *env, jobject thiz, jintArray outDims) {
    (void)thiz;
    pthread_mutex_lock(&g_frame_lock);

    if (!g_argb_frame || g_frame_w == 0 || g_frame_h == 0) {
        pthread_mutex_unlock(&g_frame_lock);
        return NULL;
    }

    jint dims[2] = { (jint)g_frame_w, (jint)g_frame_h };
    (*env)->SetIntArrayRegion(env, outDims, 0, 2, dims);

    jintArray result = (*env)->NewIntArray(env, (jsize)(g_frame_w * g_frame_h));
    (*env)->SetIntArrayRegion(env, result, 0, (jsize)(g_frame_w * g_frame_h), (const jint *)g_argb_frame);

    pthread_mutex_unlock(&g_frame_lock);
    return result;
}

// Drains up to outBuffer.length/2 stereo frames into outBuffer (interleaved L/R).
// Returns the number of stereo frames actually written.
JNIEXPORT jint JNICALL
Java_com_ct3_emu_RetroCore_nativeGetAudio(JNIEnv *env, jobject thiz, jshortArray outBuffer) {
    (void)thiz;
    jsize capacity = (*env)->GetArrayLength(env, outBuffer);
    jshort *buf = (*env)->GetShortArrayElements(env, outBuffer, NULL);

    pthread_mutex_lock(&g_audio_lock);
    jint written = 0;
    while (g_audio_read != g_audio_write && written < capacity) {
        buf[written++] = g_audio_ring[g_audio_read];
        g_audio_read = (g_audio_read + 1) % AUDIO_RING_CAPACITY;
    }
    pthread_mutex_unlock(&g_audio_lock);

    (*env)->ReleaseShortArrayElements(env, outBuffer, buf, 0);
    return written / 2; // stereo frames
}

JNIEXPORT void JNICALL
Java_com_ct3_emu_RetroCore_nativeSetButton(JNIEnv *env, jobject thiz, jint id, jboolean down) {
    (void)env; (void)thiz;
    if (id < 0 || id >= RETRO_NUM_JOYPAD_BUTTONS) return;
    if (down) {
        g_joypad_state |= (int16_t)(1 << id);
    } else {
        g_joypad_state &= (int16_t)~(1 << id);
    }
}

JNIEXPORT void JNICALL
Java_com_ct3_emu_RetroCore_nativeReset(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    if (core_reset) core_reset();
}

JNIEXPORT void JNICALL
Java_com_ct3_emu_RetroCore_nativeUnload(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    if (core_unload_game) core_unload_game();
    if (core_deinit) core_deinit();
    if (g_core_handle) {
        dlclose(g_core_handle);
        g_core_handle = NULL;
    }
    free(g_argb_frame);
    g_argb_frame = NULL;
}
