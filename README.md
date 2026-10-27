# Need for Speed: Most Wanted for Android
An unofficial native Android port of the original Xbox version of *Need for Speed: Most Wanted* (Black Edition), with touch controls, controller and keyboard support and extended graphics options.

The game's x86 code is statically recompiled ahead of time to C++ with [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp), then built for arm64. Drawing goes through Vulkan, and windowing, input and audio through SDL3. No emulator is involved. The Android host (Vulkan renderer, DirectSound, touch controls, settings panel) comes from [Black-Android](https://github.com/RanggaCasper/Black-Android), as [ShaolinMonks-Android](https://github.com/RanggaCasper/ShaolinMonks-Android) carries it.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of the game to install or build.

> [!WARNING]
> Work in progress. See [Status](#status).

## Installation
1. Install the APK.
2. Open **Most Wanted** and allow **all files access** when it asks (on Android 10, the **storage** permission).
3. Copy the game into `/storage/emulated/0/MostWanted`, or into a `MostWanted` folder at the top of your SD card: either the disc image (`.iso`) or `default.xbe` with the disc's `NFS` folder beside it. The app creates the folder on the phone's storage once access is allowed, and starts the game as soon as it finds the files.

If you don't grant that access, `Android/data/com.racas.mostwanted/files/assets` works too.

Settings (`mostwanted.toml`, `android.toml`, `layout.toml`), saves (`hdd`) and the shader cache are stored in `Android/data/com.racas.mostwanted/files`. Logs go in `/storage/emulated/0/MostWanted/logs` (in `Android/data/com.racas.mostwanted/files/logs` without all files access).

The supported disc is the USA Black Edition (`default.xbe` SHA-1 `f636a16af04171089f879a4bed646e185c734233`).

## Features
The settings panel opens from the gear at the top left of the screen (or F10 on a keyboard). Android's back button is the controller's Start, which pauses the game.

### Graphics
- **Render Resolution**: 20% to 200% of the screen's height, instead of the original 640x480.
- **V-Sync**, **Max Frame Rate** (unlimited, the display's refresh rate or a custom cap) and an **FPS Counter**.
- **Anisotropic Filtering**, **Anti-Aliasing** (FXAA, SSAA 2x) and **Screen Filters**.

### Touch Controls
A full Xbox controller on the screen:
- The left side of the screen is the **left stick**, wherever your thumb comes down. A drag on the right side of the screen is the **right stick**.
- **A**, **B**, **X**, **Y** in the controller's diamond and colours, **Black** and **White**, the **triggers** (RT gas, LT brake); the face buttons show the game's own controls (A E-brake, B nitrous, X speedbreaker, Y look back), **Start**, **Back** and a **D-pad**.
- **Controls Layout**: *Default* (triggers at the top corners) or *Low Triggers*, with the controls' opacity and overall size.
- **Edit Controls Position**: drag each control where you want it. To resize one, select it and tap **−** or **+**, or pinch it with a second finger.

### Other Input
- **Controller**: any controller SDL3 supports, with rumble. The touch controls hide while a controller is in use.
- **Keyboard**, with every controller button and both sticks rebindable.

### General
- **Language**: English, French, German, Italian or Spanish. The game takes it from the console's language setting, which this sets; it applies at the next start. Indonesian translates the settings panel and plays the game in English.
- **Audio Output**: automatic, mono, stereo or 5.1 surround.
- **Background Pause**: the game and its sound pause while the app is in the background.

## Status
Tested on a Xiaomi Pad 7 (Snapdragon 7+ Gen 3): boot, the menus, the career intro and races play, about 60 FPS in a race.

- `xbe_symbols` finds 165 of the 238 XDK functions the host knows and all 12 data symbols; the others are not linked into this image. Eleven more were resolved by hand in `config/game.toml`.
- Most Wanted runs many of its vertex programs from the GPU's program memory (`LoadVertexShaderProgram` and `SelectVertexShaderDirect`), pushes its vertex constants inline, sets its pixel combiners with render states and gives its own frame and depth buffers; the host handles each.
- Visibility tests (the sun's glare) are not counted yet: the glare stays off.
- Known: a story film (after the first race) has been seen to stop on its first picture; a rare crash in the game's own code has been seen twice.
- Nothing game-specific is patched yet: no frame rate unlock or cheats.

Logs are in `/storage/emulated/0/MostWanted/logs` (or `Android/data/com.racas.mostwanted/files/logs` without all files access). Please include the log when reporting a problem.

## System Requirements
- **Android** 10 or newer, arm64.
- **GPU** with a Vulkan 1.1 driver, for example Adreno 6xx or newer, Mali-G7x or newer, or Xclipse.
- **Memory**: 4 GB.
- **Storage**: 4 GB for the game files and saves.

## Building
Building requires a Windows PC.

### 1. Clone this repository

### 2. Add the game files
Put `default.xbe` in `assets/`, or put the disc image (`.iso`) in `assets/` or `iso/`. The build extracts `default.xbe` from the image.

### 3. Install the dependencies
- [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) with *Desktop development with C++*. The recompiler runs on the PC.
- [Android Studio](https://developer.android.com/studio). In its SDK Manager, install Android SDK Platform 34, NDK `28.2.13676358` and CMake `3.31.6`. Gradle needs Java 17 to 21 (`JAVA_HOME`).
- [Python](https://www.python.org/), needed only to extract `default.xbe` from a disc image.

### 4. Build
Run `build.bat`. It builds the recompiler, recompiles the xbe into `generated/game`, then builds `app/build/outputs/apk/release/app-release.apk` with Gradle. The first build compiles the whole game for arm64 and takes 30 to 90 minutes.

Install the APK with:
```
adb install -r app/build/outputs/apk/release/app-release.apk
```

The SDK is vendored in `sdk/`, at the upstream commit in `sdk/UPSTREAM_COMMIT`, with one change: the console's language comes from `app::ConsoleLanguage()` (`sdk/runtime/kernel/xboxkrnl.cpp`).

## Credits
- [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) by TGP482: the static recompiler and the portable runtime (Xbox kernel, XDK libraries and Direct3D 8) this port is built on.
- [Black-Android](https://github.com/RanggaCasper/Black-Android) and [ShaolinMonks-Android](https://github.com/RanggaCasper/ShaolinMonks-Android) by RanggaCasper: the Android host this port is built from.
- Zukiru: the HDR screen filter.
- [SDL](https://github.com/libsdl-org/SDL), [glslang](https://github.com/KhronosGroup/glslang), [toml++](https://github.com/marzer/tomlplusplus) and [Dear ImGui](https://github.com/ocornut/imgui).
- [xemu](https://github.com/xemu-project/xemu) and [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded), which XboxXBE-Recomp used as references for Xbox hardware and API behavior.

*Need for Speed* is a trademark of Electronic Arts Inc. This project is not affiliated with or endorsed by them.
