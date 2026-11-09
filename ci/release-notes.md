<p align="center">
  <img src="https://raw.githubusercontent.com/{{REPO}}/main/app/src/main/res/mipmap-xxxhdpi/ic_launcher.png" width="96" alt="Most Wanted">
</p>

<h1 align="center">Most Wanted for Android {{VERSION}}</h1>

<p align="center">
  An unofficial native Android port of the original Xbox version of <b>Need for Speed: Most Wanted</b>,<br>
  statically recompiled with <a href="https://github.com/TGP482/XboxXBE-Recomp">XboxXBE-Recomp</a> and drawn with Vulkan.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Android-10%2B-3DDC84?logo=android&logoColor=white" alt="Android 10+">
  <img src="https://img.shields.io/badge/arch-arm64--v8a-blue" alt="arm64-v8a">
  <img src="https://img.shields.io/badge/Vulkan-1.1-AC162C?logo=vulkan&logoColor=white" alt="Vulkan 1.1">
  <img src="https://img.shields.io/badge/SDL-3-1D4F91" alt="SDL3">
</p>

> [!IMPORTANT]
> This release contains **no game assets**. You need the files from your own legally acquired copy of *Need for Speed: Most Wanted* (Xbox, Black Edition, USA release).

## 📦 Download

| File | Size | SHA-256 |
| --- | --- | --- |
| [`{{APK}}`](https://github.com/{{REPO}}/releases/download/{{TAG}}/{{APK}}) | {{SIZE}} | `{{SHA256}}` |

## 🚀 Installation

1. Install **`{{APK}}`** on your phone.
2. Open **Most Wanted** and allow **all files access** when it asks: the game needs it to start.
3. Copy the game into **`/storage/emulated/0/MostWanted`**, or into a `MostWanted` folder on your SD card: the disc image (`.iso`), or `default.xbe` with the disc's `NFS` folder beside it.
4. The game starts as soon as it finds the files.

> [!TIP]
> Saves are in `MostWanted/hdd`, logs and crash reports in `MostWanted/logs`. Updating from a version that kept the saves in `Android/data/com.racas.mostwanted/files/hdd` copies them over at the first launch. Settings and the shader cache stay in `Android/data/com.racas.mostwanted/files`.

## ✨ Features

| | |
| --- | --- |
| 🖥️ **Graphics** | Render resolution 20–200%, aspect ratio from 1:1 to full screen, FXAA / SSAA 2x, sharpen, vivid, scanline and CRT filters, unlocked frame rate, V-Sync, FPS counter, anisotropic filtering, car and road reflections, shadows, rain, motion blur, night mode, shader preloading |
| 🚗 **Mods Loader** | PC add-on cars from nfsaddons.com installed on the phone (zip, 7z, rar, packs of many cars), and NextGenGraphics texture packs at the size you pick |
| 🎬 **Cinematic Mode** | A free camera from the pause menu, keyframe paths, photos, and the path saved as a video |
| 📼 **Replay** | The race played again with its sound, with chase, TV, free, bumper and auto cameras, and saved as a video |
| 🎵 **My Music** | Your own songs from `MostWanted/music` in place of EA Trax, silent in movies, cutscenes and loading |
| 👆 **Touch Controls** | A full Xbox pad on screen, arrows, wheel or tilt steering, a layout editor for the controls and the HUD, tap and swipe in the game's menus |
| 🎮 **Other Input** | SDL3 controllers with rumble, keyboard with rebindable controls |
| 🕹️ **Cheats** | Infinite nitrous and speedbreaker, max cash, no police, no traffic, unlock all, auto shift in drag races |
| ⚙️ **General** | Settings in the game's own OPTIONS menu, language (with Indonesian), skip movies, save backups, audio output, update check, pause while in the background, crash and freeze reports in the MostWanted folder |

The settings are in the game's OPTIONS menu, under ANDROID OPTIONS. The gear at the top of the screen opens the cheats. Android's back button is the controller's Start.

## 📱 Requirements

- **Android** 10 or newer, arm64
- **GPU** with a Vulkan 1.1 driver (Adreno 6xx+, Mali-G7x+, Xclipse)
- **4 GB** RAM, **4 GB** free storage

## 🙏 Credits

Built on [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) by **TGP482** and the Android host of [Black-Android](https://github.com/RanggaCasper/Black-Android). Button Design by **bagasromadon**. Uses [SDL](https://github.com/libsdl-org/SDL), [glslang](https://github.com/KhronosGroup/glslang), [toml++](https://github.com/marzer/tomlplusplus) and [Dear ImGui](https://github.com/ocornut/imgui).

*Need for Speed* is a trademark of Electronic Arts Inc. This project is not affiliated with or endorsed by them.
