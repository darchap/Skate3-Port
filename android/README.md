# Skate 3 Port — Android app

ARM64 Android target. The Xbox 360 PowerPC guest code is statically recompiled to native
AArch64 and linked with the ReXGlue runtime, SDL3, and Vulkan. No retail game or
title-update files are included in this repository or the APK.

## Device requirements

- Android 13 or newer (API 33+), `arm64-v8a`, Vulkan
- ARMv8.2 CPU with FP16 and dot-product extensions
- A physical controller, or the built-in touch controls

Development target: Poco F3 (Snapdragon 870 / Adreno 650).

## What the launcher does

The launcher is deliberately minimal (`LauncherActivity.java`):

1. **Select My Skate 3 ISO**: pick an ISO dumped from your own supported Xbox 360 copy
   with Android's file picker. The app validates `default.xex` and
   `data/webkit/EAWebkit.xex` by SHA-256 and extracts them to app-specific storage.
2. **Title Update 3**: downloaded and verified automatically, with a manual
   "Select Title Update File" fallback.
3. **Play Skate 3**: starts the game (`Skate3Activity`, an SDL activity).

Also available: repair / reinstall, start over, and a setup log. There is no updater,
bug reporter, mod store, language picker, or custom GPU driver import; those belong to
the future launcher.

Game files live at:

```text
/storage/emulated/0/Android/data/io.skate3port.game.dev/files/game/   (debug builds)
/storage/emulated/0/Android/data/io.skate3port.game/files/game/       (release builds)
```

Uninstalling the app removes this folder, so keep the original ISO. The extracted
install is about 6 GB; keep about 8 GB free.

## Build

On Windows, from the repository root:

```powershell
.\build-android.ps1            # debug APK -> out\Skate3-Port-debug.apk
.\build-android.ps1 -Install   # ...and adb install
```

Native libraries are staged into `app/libs/arm64-v8a/` (ignored by git) and the APK is
built with `gradlew.bat assembleDebug`.

## In-game overlay

Press **RB + Start** to open the runtime settings overlay. Every graphics option under
**Video** is yours and survives restarts: **3D Scene Resolution** (512x288 to 1280x720,
applies live), **World Detail** (Full or Simplified), MSAA, shadows, SSAO, bloom and
the rest. **Android Device Profile** currently has one entry, Custom.
