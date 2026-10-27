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
2. Open **Most Wanted** and allow **all files access** when it asks.
3. Copy the game into **`/storage/emulated/0/MostWanted`**, or into a `MostWanted` folder on your SD card: the disc image (`.iso`), or `default.xbe` with the disc's `NFS` folder beside it.
4. The game starts as soon as it finds the files.

> [!TIP]
> If you don't grant all files access, put the game in `Android/data/com.racas.mostwanted/files/assets` instead. Settings, saves and the shader cache are stored in `Android/data/com.racas.mostwanted/files`; logs go in `MostWanted/logs` (in `Android/data/com.racas.mostwanted/files/logs` without all files access).

## ✨ Features

| | |
| --- | --- |
| 🖥️ **Graphics** | Render resolution 20–200%, aspect ratio from 1:1 to full screen, FXAA / SSAA 2x, sharpen, vivid, scanline and CRT filters, unlocked frame rate, V-Sync, FPS counter, anisotropic filtering |
| 👆 **Touch Controls** | A full Xbox pad on screen, layout presets, a layout editor to move and resize each control, PS2 button icons |
| 🎮 **Other Input** | SDL3 controllers with rumble, keyboard with rebindable controls |
| ⚙️ **General** | Language (with Indonesian), audio output, update check, pause while in the background |

The settings panel opens from the gear at the top of the screen. Android's back button is the controller's Start.

## 📱 Requirements

- **Android** 10 or newer, arm64
- **GPU** with a Vulkan 1.1 driver (Adreno 6xx+, Mali-G7x+, Xclipse)
- **4 GB** RAM, **4 GB** free storage

## 🙏 Credits

Built on [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) by **TGP482** and the Android host of [Black-Android](https://github.com/RanggaCasper/Black-Android), with [SDL](https://github.com/libsdl-org/SDL), [glslang](https://github.com/KhronosGroup/glslang), [toml++](https://github.com/marzer/tomlplusplus) and [Dear ImGui](https://github.com/ocornut/imgui).

*Need for Speed* is a trademark of Electronic Arts Inc. This project is not affiliated with or endorsed by them.
