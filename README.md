# Pipi & Bibis - Android

Android app for the recompiled arcade game (68000 code lifted to C++, emulated Z80 + YM3812 sound board).
No ROMs are included. On the first start the app asks **once** for the ROM files; it copies them into its own storage and goes
straight into the game from then on.

## Controls (touch)
- Left: joystick
- Right: **SHOOT** (button 1) and **A** (button 2)
- Bottom centre: **CREDIT** (coin) and **START**
- Top right: **menu** button. It opens a row with **P1 / P2** (which player the touch controls play; with P2 the coin and start
  buttons become coin 2 / start 2), **SAVE**, **LOAD** and **TXT**.
- **TXT** opens the settings text (`pipibibs_settings.txt`) in an editor. "Save and restart" writes it and restarts the game so the
  settings are read again. The same dialog has "Change ROMs" to select the ROM files again.

## ROMs
Select `tp025-1.bin` ... `tp025-5.bin` (several files at once), or the zip that contains them (MAME set `pipibibs`).

## Files
- Settings and the save game (`pipibibs_save.txt`): the app's folder `Android/data/com.pipibibs.recomp/files/`.
- ROM copies: the app's private storage.

## Build
- Android Studio: open this folder, let it sync (it needs the NDK and CMake 3.22.1 from the SDK Manager), Build > Build APK.
- Without installing anything: put this folder's contents in the root of a GitHub repository; the workflow in
  `.github/workflows/android.yml` builds `app-debug.apk` (Actions tab > latest run > Artifacts > pipibibs-apk).

Layout: `app/src/main/java/...` (activity, touch view, audio), `app/src/main/cpp/pipibibs_jni.cpp` (bridge),
`app/src/main/cpp/engine/` (game code and runtime), `app/src/main/res/drawable-nodpi/` (button images).
