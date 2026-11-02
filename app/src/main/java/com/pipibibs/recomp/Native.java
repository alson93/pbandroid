package com.pipibibs.recomp;

/** JNI bridge to the lifted game (see src/main/cpp/pipibibs_jni.cpp). */
final class Native {
    static { System.loadLibrary("pipibibs"); }
    private Native() {}

    /** Loads the ROMs from romDir and starts the game; false if a ROM file is missing/unreadable. */
    static native boolean init(String romDir, String settingsFile);
    /** Fills a 320x240 ARGB array with the current picture. */
    static native void render(int[] argb);
    /** Renders 48 kHz interleaved stereo; the audio thread paces the sound board with this. */
    static native void audio(short[] pcm, int frames);
    /** Joystick/button bits for player 1, player 2 and the system port (coin/start). */
    static native void input(int p1, int p2, int sys);
    static native long frameCount();
    static native void requestSave();
    static native void requestLoad();
    /** swap_players from the settings file (start with the touch controls on player 2). */
    static native boolean swapSetting();
    /** Newest save/load status message or null. */
    static native String message();
    /** Diagnostics line (game counters, last error). */
    static native String status();
}
