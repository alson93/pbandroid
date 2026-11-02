// pipibibs_jni.cpp - thin JNI layer between the Android app (Java) and the lifted game (68000 + Z80/YM3812 sound board).
// The Java side owns the window, touch controls and AudioTrack; this file only starts the emulation and moves frames,
// audio buffers and input bits across.
#include <jni.h>
#include <android/log.h>
#include <thread>
#include <pthread.h>
#include <cstdio>
#include <string>
#include <cstdlib>
#include <cstring>
#include "m68k_rt.h"
#include "hw.h"
#include "sound.h"
#include "video.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "pipibibs", __VA_ARGS__)
void vec_reset();
extern unsigned long irq_serviced;
unsigned long long sound_debug_insns();
extern void (*fatal_hook)(const char*);

static bool g_started = false;
static uint32_t g_frame[VIDEO_W * VIDEO_H];
static unsigned g_msg_seq = 0;

static char g_err[256] = "";
static void on_fatal(const char* msg) { std::snprintf(g_err, sizeof g_err, "%s", msg); __android_log_print(ANDROID_LOG_ERROR, "pipibibs", "emulation error: %s", msg); }

// The lifted 68000 code is deeply nested: run it on a thread with a big stack (a std::thread only gets about 1 MB on Android).
static void* emu_main(void*) { a[7] = rd32(0); sr_sys = 0x2700; vec_reset(); __android_log_print(ANDROID_LOG_ERROR, "pipibibs", "the game code returned (stopped)"); std::snprintf(g_err, sizeof g_err, "game code stopped"); return nullptr; }

extern "C" {

// Native.init(romDir, settingsFile): loads the ROMs and starts the game. Returns false when the ROM files are not all there.
JNIEXPORT jboolean JNICALL Java_com_pipibibs_recomp_Native_init(JNIEnv* env, jclass, jstring jrom, jstring jsettings) {
    if (g_started) return JNI_TRUE;
    const char* rom = env->GetStringUTFChars(jrom, nullptr);
    const char* set = env->GetStringUTFChars(jsettings, nullptr);
    setenv("PIPIBIBS_SETTINGS", set, 1);              // settings (and the save file next to it) live in the app's files folder
    bool ok = load_roms(rom) && video_init(rom);
    if (*hw_settings_warnings()) LOG("settings warnings: %s (file %s)", hw_settings_warnings(), hw_settings_source());
    env->ReleaseStringUTFChars(jrom, rom);
    env->ReleaseStringUTFChars(jsettings, set);
    if (!ok) return JNI_FALSE;
    fatal_hook = on_fatal;
    sound_boot(z80rom, hw_shared_ram());
    sound_set_pulled(true);                           // the Java AudioTrack thread is the clock of the sound board
    hw_start_realtime();                              // 60 Hz VBLANK (speed setting applies)
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u * 1024 * 1024); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t th; if (pthread_create(&th, &at, emu_main, nullptr) != 0) { std::snprintf(g_err, sizeof g_err, "cannot start the game thread"); }
    g_started = true;
    return JNI_TRUE;
}

// Native.render(int[] argb320x240)
JNIEXPORT void JNICALL Java_com_pipibibs_recomp_Native_render(JNIEnv* env, jclass, jintArray arr) {
    if (!g_started) return;
    video_render(g_frame);
    for (int i = 0; i < VIDEO_W * VIDEO_H; ++i) g_frame[i] |= 0xFF000000u;
    env->SetIntArrayRegion(arr, 0, VIDEO_W * VIDEO_H, (const jint*)g_frame);
}

// Native.audio(short[] stereoInterleaved, frames): renders 48 kHz stereo
JNIEXPORT void JNICALL Java_com_pipibibs_recomp_Native_audio(JNIEnv* env, jclass, jshortArray arr, jint frames) {
    if (!g_started) return;
    jshort* p = env->GetShortArrayElements(arr, nullptr);
    sound_render((int16_t*)p, (size_t)frames, 48000);
    env->ReleaseShortArrayElements(arr, p, 0);
}

// Native.input(p1, p2, sys): port bits exactly as in the other front ends (see README)
JNIEXPORT void JNICALL Java_com_pipibibs_recomp_Native_input(JNIEnv*, jclass, jint p1, jint p2, jint sys) {
    hw_set_input(0x19C031, (uint8_t)p1);
    hw_set_input(0x19C035, (uint8_t)p2);
    hw_set_input(0x19C02D, (uint8_t)sys);
}

JNIEXPORT jlong JNICALL Java_com_pipibibs_recomp_Native_frameCount(JNIEnv*, jclass) { return (jlong)hw_latch_count(); }
JNIEXPORT void JNICALL Java_com_pipibibs_recomp_Native_requestSave(JNIEnv*, jclass) { hw_state_request_save(); }
JNIEXPORT void JNICALL Java_com_pipibibs_recomp_Native_requestLoad(JNIEnv*, jclass) { hw_state_request_load(); }
JNIEXPORT jboolean JNICALL Java_com_pipibibs_recomp_Native_swapSetting(JNIEnv*, jclass) { return hw_settings_swap_players() ? JNI_TRUE : JNI_FALSE; }

// Native.message(): the newest save/load status text, or null when nothing new
JNIEXPORT jstring JNICALL Java_com_pipibibs_recomp_Native_message(JNIEnv* env, jclass) {
    char b[512];
    if (!hw_state_message(g_msg_seq, b, sizeof b)) return nullptr;
    return env->NewStringUTF(b);
}

// Native.status(): one line of diagnostics for the on-screen info (main loop / vblank counters of the game, last error)
JNIEXPORT jstring JNICALL Java_com_pipibibs_recomp_Native_status(JNIEnv* env, jclass) {
    char b[400];
    std::snprintf(b, sizeof b, "main=%u vbl=%u frames=%llu irq=%lu sr=%04x irqp=%d a7=%08x z80=%llu%s%s", (unsigned)rd16(0x080564), (unsigned)rd16(0x080566),
                  (unsigned long long)hw_latch_count(), irq_serviced, (unsigned)sr_sys, (int)irq_level_pending, (unsigned)a[7], sound_debug_insns(), g_err[0] ? "  ERR: " : "", g_err);
    return env->NewStringUTF(b);
}

}  // extern "C"
