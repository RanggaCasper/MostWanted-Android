# Need for Speed: Most Wanted for Android
**English** | [Bahasa Indonesia](README.id.md)

An unofficial native Android port of the original Xbox version of *Need for Speed: Most Wanted* (Black Edition), with touch controls, controller and keyboard support and extended graphics options.

The game's x86 code is statically recompiled ahead of time to C++ with [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp), then built for arm64. Drawing goes through Vulkan, and windowing, input and audio through SDL3. No emulator is involved. The Android host (Vulkan renderer, DirectSound, touch controls, settings panel) comes from [Black-Android](https://github.com/RanggaCasper/Black-Android), as [ShaolinMonks-Android](https://github.com/RanggaCasper/ShaolinMonks-Android) carries it.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of the game to install or build.

> [!WARNING]
> Work in progress. See [Status](#status).

## Gameplay

<p align="center">
  <a href="https://youtu.be/x5bczwTuyhw">
    <img src="https://img.youtube.com/vi/0ILO1_gulrg/maxresdefault.jpg" alt="Black Android gameplay on POCO F6" width="720">
  </a>
  <br>
  <sub>▶ Gameplay on Xiaomi Pad 7</sub>
</p>

## Installation
1. Install the APK.
2. Open **Most Wanted** and allow **all files access** when it asks (on Android 10, the **storage** permission). It is required: the game doesn't start without it.
3. Copy the game into `/storage/emulated/0/MostWanted`, or into a `MostWanted` folder at the top of your SD card: either the disc image (`.iso`) or `default.xbe` with the disc's `NFS` folder beside it. The app creates the folder on the phone's storage once access is allowed, and starts the game as soon as it finds the files.

Saves go in `/storage/emulated/0/MostWanted/hdd` and logs in `/storage/emulated/0/MostWanted/logs`, so they stay if the app is uninstalled. Updating from a version that kept the saves in `Android/data/com.racas.mostwanted/files/hdd` copies them there on the first start; the old copy is left as it was. Settings (`mostwanted.toml`, `android.toml`, `layout.toml`) and the shader cache stay in `Android/data/com.racas.mostwanted/files`.

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
- Game-specific work so far: the settings live in the game's own OPTIONS menu (ANDROID OPTIONS, in the main menu and the pause menu); quality switches for car and road reflections, shadows, particles, rain, screen raindrops, the yellow tint and motion blur; widescreen (Hor+) for any screen shape; SKIP MOVIES; menus by touch; a HUD layout editor; a RESET button that puts the car back on the road; and cheats (infinite nitrous and speedbreaker, max cash, no police, unlock all) on the gear's panel.
- Fixed in the next release: the game crashed as a movie started or stopped (at boot, after a race), on phones with several cores. EA's stream reader read data the stream thread was still writing.
- Known: a story film (after the first race) has been seen to stop on its first picture. The engine's own cutscenes (rival intros, the career's opening) can't be skipped yet.

Logs are in `/storage/emulated/0/MostWanted/logs`. On Android 11 and newer, a crash is written there as `crash_<date>.txt` the next time you open the game. Please include `mostwanted.log`, and the crash file if there is one, when reporting a problem.

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

## Linux
The game also builds and runs natively on Linux (x86-64), from the same sources: the Vulkan renderer, sound, controllers and settings are the Android version's, with a desktop window in place of the phone's screen. Linux builds from source.

- Requirements: a 64-bit distribution with a Vulkan 1.1 driver (Mesa or NVIDIA), and Clang, CMake, Ninja, Git, pkg-config, Python 3 and the Vulkan loader and headers. SDL3 is taken from the system when it has 3.2 or newer, else downloaded and built in. `build.sh` lists the packages for Arch, Debian/Ubuntu and Fedora when something is missing.
- Put `default.xbe` with the game files, or the disc image (`.iso`), in `assets/`, then run `./build.sh` (`./build.sh debug` for the info lines in the log). It builds the recompiler, recompiles the xbe into `generated/game`, then builds `out/build/linux/mostwanted`. The first build compiles the whole game and takes 15 to 60 minutes.
- Run `out/build/linux/mostwanted`. The game is found in an `assets` or `iso` folder beside the executable or in any folder above it, so the repo's own `assets` works as it is.
- Settings (`mostwanted.toml`, `android.toml`, `layout.toml`), saves (`hdd`), logs and the shader cache are written beside the executable.
- Keyboard, mouse (in the settings panel) and controllers through SDL3, on Wayland or X11. F10 opens the settings panel, F11 or Alt+Enter switches between windowed and full screen. `display_mode` (`fullscreen`, `borderless`, `windowed`) with `width` and `height` in `mostwanted.toml` set the window.
- The game's menus get LINUX OPTIONS in place of ANDROID OPTIONS. The touch controls show only on a touch screen, after it is touched.

## Credits
- [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) by TGP482: the static recompiler and the portable runtime (Xbox kernel, XDK libraries and Direct3D 8) this port is built on.
- [Black-Android](https://github.com/RanggaCasper/Black-Android) and [ShaolinMonks-Android](https://github.com/RanggaCasper/ShaolinMonks-Android) by RanggaCasper: the Android host this port is built from.
- Zukiru: the HDR screen filter.
- [bagasromadon](https://github.com/bagasromadon): Button Design.
- [SDL](https://github.com/libsdl-org/SDL), [glslang](https://github.com/KhronosGroup/glslang), [toml++](https://github.com/marzer/tomlplusplus) and [Dear ImGui](https://github.com/ocornut/imgui).
- [xemu](https://github.com/xemu-project/xemu) and [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded), which XboxXBE-Recomp used as references for Xbox hardware and API behavior.

*Need for Speed* is a trademark of Electronic Arts Inc. This project is not affiliated with or endorsed by them.
