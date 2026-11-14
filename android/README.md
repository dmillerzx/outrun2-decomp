# OutRun 2006 Coast 2 Coast - Android Port

Native Android port of OutRun 2006 Coast 2 Coast with Vulkan backend, custom SDL2 input integration, and the complete [emoose/OutRun2006Tweaks](https://github.com/emoose/OutRun2006Tweaks) enhancement suite.

Optimized for modern Android handheld gaming consoles, specifically the **Retroid Pocket 5** (Qualcomm Snapdragon 865, Adreno 650 GPU).

---

## Features

- **Native Vulkan Graphics**: Direct 1080p (1920x1080) presentation utilizing Qualcomm Adreno 650 hardware Vulkan drivers with zero translation layer overhead.
- **Locked 60.00 FPS Pacing**: Microsecond frame pacer with FIFO presentation, eliminating physics fast-forward and maintaining rock-solid 60 FPS.
- **Complete OutRun2006Tweaks Suite**:
  - Native 16:9 widescreen presentation and correct aspect ratio
  - High-definition car and driver models (`UseHiDefCharacters`)
  - Full pedal checks calibration fix for analog triggers (`FixFullPedalChecks`)
  - Enhanced UI scaling and high-resolution reflections
  - Audio stream enhancements
- **Integrated Android App Icon**: Custom high-resolution retro arcade app launcher icon across all standard and adaptive mipmap densities.
- **Physical Handheld Gamepad Support**: Built-in SDL2 GameController mapping with 10% Hall Effect analog deadband filter.

---

## Game Data Setup

This port requires original game assets from the PC retail / Steam release of *OutRun 2006 Coast 2 Coast*.

1. Install the standalone APK on your device:
   ```bash
   adb install -r outrun2-c2c-android.apk
   ```
2. Grant storage permissions:
   ```bash
   adb shell appops set org.outrun2.c2c MANAGE_EXTERNAL_STORAGE allow
   ```
3. Copy your PC game installation folder (containing `OR2006C2C.EXE`, `Media/`, `Sound/`, etc.) to one of the following paths on your device:
   - MicroSD: `/storage/<SD_ID>/ROMs/ports/outrun2006/`
   - Internal Storage: `/sdcard/ROMs/ports/outrun2006/`
   - Scoped App Storage: `/sdcard/Android/data/org.outrun2.c2c/files/`

On the very first launch, the port reads code tables and constants directly from `OR2006C2C.EXE`, verifies SHA-256 hashes, and generates `OR2006C2C.cache` in the game folder. Subsequent launches boot instantly from cache.

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
