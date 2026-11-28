# OutRun 2006 Coast 2 Coast - Android Port

Native Android port of OutRun 2006 Coast 2 Coast with Vulkan backend, custom SDL2 input integration, and the complete [emoose/OutRun2006Tweaks](https://github.com/emoose/OutRun2006Tweaks) enhancement suite.

Optimized for modern Android handheld gaming consoles, specifically the **Retroid Pocket 5** (Qualcomm Snapdragon 865, Adreno 650 GPU).

---

## Features

- **Native Vulkan Graphics**: Direct 1080p (1920x1080) presentation utilizing Qualcomm Adreno 650 hardware Vulkan drivers with zero translation layer overhead.
- **Locked 60.00 FPS Pacing**: Microsecond frame pacer with FIFO presentation, eliminating physics fast-forward and maintaining rock-solid 60 FPS.
- **Complete OutRun2006Tweaks Suite**:
  - Auto-detected 16:9 widescreen / 4:3 aspect ratio (configurable live in **Options > Settings** and persisted to `options.ini`)
  - High-definition car and driver models (`UseHiDefCharacters`)
  - Full pedal checks calibration fix for analog triggers (`FixFullPedalChecks`)
  - Enhanced UI scaling and high-resolution reflections
  - Audio stream enhancements
- **Integrated Android App Icon**: Custom high-resolution retro arcade app launcher icon across all standard and adaptive mipmap densities.
- **Physical Handheld Gamepad Support**: Built-in SDL2 GameController mapping with 10% Hall Effect analog deadband filter.

---

## Game Data Setup

This port requires original game assets from the **Steam** PC release of *OutRun 2006 Coast 2 Coast* (`OR2006C2C.EXE`, `Cars/`, `Stage/`, `Media/`, `Scripts/`, `Sprite/`, etc.).

1. Install `outrun2-c2c-android-v1.0.7-preview.apk` on your device.
2. Copy your Steam PC game installation folder to any of these auto-scanned paths (or anywhere you prefer and pick it via **Choose Folder** inside the first-launch prompt):
   - MicroSD: `/storage/<SD_ID>/ROMs/ports/outrun2006/`
   - Internal Storage: `/sdcard/ROMs/ports/outrun2006/`
   - App-Private Storage: `/sdcard/Android/data/org.outrun2.c2c/files/` *(requires no `MANAGE_EXTERNAL_STORAGE` grant)*
3. Launch the app:
   - If you stored files in MicroSD or `/sdcard/ROMs/...`, tap **All Files Access** (or **Choose Folder**) when prompted.
   - On very first launch, the app unpacks and verifies `OR2006C2C.EXE` against SHA-256 hashes and generates `OR2006C2C.cache` automatically.
   - Diagnostics are always written to `/sdcard/Android/data/org.outrun2.c2c/files/outrun2006.log`.

---

## Default Controls (Retroid Pocket 5 / Xbox Mode)

| Action | Physical Button / Axis |
| --- | --- |
| **Steer** | Left Analog Stick / D-Pad |
| **Accelerate** | Button A / Right Trigger (RT / ZR) |
| **Brake** | Button B / Left Trigger (LT / ZL) |
| **Gear Shift Up** | Button Y / Right Shoulder (R1) |
| **Gear Shift Down** | Button X / Left Shoulder (L1) |
| **View Change** | Button Y |
| **Pause / Menu** | Start Button |
| **Back / Cancel** | Select / Back Button |

---

## Building from Source

### Prerequisites
- Android NDK (r27 or newer recommended, e.g., NDK 29)
- Android SDK (API level 28 minimum, API level 35 target)
- CMake 3.25+ and Ninja
- JDK 17

### Build Steps

1. Configure and build native shared library (`libmain.so`):
   ```bash
   cmake -S . -B build-android -G Ninja \
     -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
     -DANDROID_ABI=arm64-v8a \
     -DANDROID_PLATFORM=android-28 \
     -DCMAKE_BUILD_TYPE=Release
   cmake --build build-android
   ```
2. Strip unneeded symbols:
   ```bash
   llvm-strip --strip-unneeded build-android/libmain.so
   ```
3. Copy to JNI libraries directory:
   ```bash
   cp build-android/libmain.so android/app/src/main/jniLibs/arm64-v8a/libmain.so
   ```
4. Assemble release APK:
   ```bash
   cd android
   ./gradlew assembleRelease
   ```
   The output APK will be located at `app/build/outputs/apk/release/app-release.apk`.
