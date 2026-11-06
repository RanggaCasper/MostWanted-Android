# Need for Speed: Most Wanted untuk Android
[English](README.md) | **Bahasa Indonesia**

Port Android native tidak resmi dari *Need for Speed: Most Wanted* (Black Edition) versi Xbox orisinal, dengan kontrol sentuh, dukungan controller dan keyboard, serta opsi grafis tambahan.

Kode x86 game ini direkompilasi secara statis ke C++ sebelumnya dengan [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp), lalu dibangun untuk arm64. Gambar ditampilkan lewat Vulkan, sedangkan jendela, input, dan audio lewat SDL3. Tidak ada emulator yang dipakai. Host Android-nya (renderer Vulkan, DirectSound, kontrol sentuh, panel pengaturan) berasal dari [Black-Android](https://github.com/RanggaCasper/Black-Android), sebagaimana dibawa oleh [ShaolinMonks-Android](https://github.com/RanggaCasper/ShaolinMonks-Android).

> [!NOTE]
> Proyek ini tidak menyertakan aset game apa pun. Untuk memasang atau membangunnya, kamu harus menyediakan file dari salinan game milikmu sendiri yang didapat secara legal.

> [!WARNING]
> Masih dalam pengembangan. Lihat [Status](#status).

## Gameplay

<p align="center">
  <a href="https://youtu.be/x5bczwTuyhw">
    <img src="https://img.youtube.com/vi/0ILO1_gulrg/maxresdefault.jpg" alt="Gameplay Most Wanted Android" width="720">
  </a>
  <br>
  <sub>▶ Gameplay di Xiaomi Pad 7</sub>
</p>

## Instalasi
1. Pasang APK-nya.
2. Buka **Most Wanted** dan izinkan **akses semua file** saat diminta (di Android 10, izin **penyimpanan**). Izin ini wajib: game tidak bisa dimulai tanpanya.
3. Salin game ke `/storage/emulated/0/MostWanted`, atau ke folder `MostWanted` di bagian paling atas kartu SD: bisa berupa image disc (`.iso`), atau `default.xbe` dengan folder `NFS` dari disc di sebelahnya. Aplikasi membuat folder itu di penyimpanan HP setelah akses diizinkan, dan langsung memulai game begitu file-nya ditemukan.

Save disimpan di `/storage/emulated/0/MostWanted/hdd` dan log di `/storage/emulated/0/MostWanted/logs`, jadi tetap ada walaupun aplikasi di-uninstall. Saat update dari versi yang menyimpan save di `Android/data/com.racas.mostwanted/files/hdd`, save itu disalin ke sana saat pertama kali dibuka; salinan lamanya dibiarkan apa adanya. Pengaturan (`mostwanted.toml`, `android.toml`, `layout.toml`) dan cache shader tetap di `Android/data/com.racas.mostwanted/files`.

Disc yang didukung adalah USA Black Edition (`default.xbe` SHA-1 `f636a16af04171089f879a4bed646e185c734233`).

## Fitur
Panel pengaturan dibuka dari ikon roda gigi di kiri atas layar (atau F10 di keyboard). Tombol kembali Android berfungsi sebagai tombol Start pada controller, yang menjeda game.

### Grafis
- **Render Resolution**: 20% sampai 200% dari tinggi layar, sebagai ganti 640x480 aslinya.
- **V-Sync**, **Max Frame Rate** (tanpa batas, sesuai refresh rate layar, atau batas sendiri), dan **FPS Counter**.
- **Anisotropic Filtering**, **Anti-Aliasing** (FXAA, SSAA 2x), dan **Screen Filters**.

### Kontrol Sentuh
Controller Xbox lengkap di layar:
- Sisi kiri layar adalah **stick kiri**, di mana pun jempolmu menyentuh. Geseran di sisi kanan layar adalah **stick kanan**.
- **A**, **B**, **X**, **Y** dengan susunan dan warna seperti di controller, **Black** dan **White**, **trigger** (RT gas, LT rem); tombol-tombolnya menampilkan kontrol game itu sendiri (A rem tangan, B nitro, X speedbreaker, Y lihat ke belakang), **Start**, **Back**, dan **D-pad**.
- **Controls Layout**: *Default* (trigger di sudut atas) atau *Low Triggers*, beserta transparansi dan ukuran keseluruhan kontrol.
- **Edit Controls Position**: geser setiap kontrol ke tempat yang kamu mau. Untuk mengubah ukurannya, pilih kontrolnya lalu ketuk **−** atau **+**, atau cubit dengan jari kedua.

### Input Lain
- **Controller**: controller apa pun yang didukung SDL3, dengan getaran. Kontrol sentuh disembunyikan selama controller dipakai.
- **Keyboard**, dengan semua tombol controller dan kedua stick bisa diatur ulang.

### Umum
- **Language**: Inggris, Prancis, Jerman, Italia, atau Spanyol. Game mengambil bahasa dari pengaturan bahasa konsol, yang diatur oleh opsi ini; berlaku saat game dimulai berikutnya. Bahasa Indonesia menerjemahkan panel pengaturan dan memainkan game dalam bahasa Inggris.
- **Audio Output**: otomatis, mono, stereo, atau surround 5.1.
- **Background Pause**: game dan suaranya dijeda selama aplikasi berada di background.

## Status
Diuji di Xiaomi Pad 7 (Snapdragon 7+ Gen 3): boot, menu, intro karier, dan balapan bisa dimainkan, sekitar 60 FPS saat balapan.

- `xbe_symbols` menemukan 165 dari 238 fungsi XDK yang dikenal host dan semua 12 simbol data; sisanya tidak di-link ke image ini. Sebelas lagi diselesaikan secara manual di `config/game.toml`.
- Most Wanted menjalankan banyak vertex program-nya dari memori program GPU (`LoadVertexShaderProgram` dan `SelectVertexShaderDirect`), mengirim konstanta vertex secara inline, mengatur pixel combiner lewat render state, dan memakai frame buffer serta depth buffer miliknya sendiri; host menangani semuanya.
- Visibility test (silau matahari) belum dihitung: efek silau tetap mati.
- Pekerjaan khusus game sejauh ini: pengaturan ada di menu OPTIONS game sendiri (ANDROID OPTIONS, di menu utama dan menu pause); pengaturan kualitas untuk pantulan mobil dan jalan, bayangan, partikel, hujan, tetesan hujan di layar, warna kuning, dan motion blur; layar lebar (Hor+) untuk bentuk layar apa pun; SKIP MOVIES; menu bisa disentuh; editor tata letak HUD; tombol RESET yang mengembalikan mobil ke jalan; dan cheat (nitro dan speedbreaker tak terbatas, uang maksimal, tanpa polisi, buka semua) di panel roda gigi.
- Diperbaiki di rilis berikutnya: game crash saat movie mulai atau berhenti (saat boot, setelah balapan) di HP dengan banyak core. Pembaca stream EA membaca data yang masih ditulis oleh thread stream.
- Diketahui: sebuah film cerita (setelah balapan pertama) pernah terlihat berhenti di gambar pertamanya. Cutscene dari engine game (intro rival, pembuka karier) belum bisa dilewati.

Log ada di `/storage/emulated/0/MostWanted/logs`. Di Android 11 ke atas, crash ditulis di sana sebagai `crash_<date>.txt` saat game dibuka lagi. Sertakan `mostwanted.log`, dan file crash kalau ada, saat melaporkan masalah.

## Kebutuhan Sistem
- **Android** 10 atau lebih baru, arm64.
- **GPU** dengan driver Vulkan 1.1, misalnya Adreno 6xx atau lebih baru, Mali-G7x atau lebih baru, atau Xclipse.
- **Memori**: 4 GB.
- **Penyimpanan**: 4 GB untuk file game dan save.

## Membangun
Membangun proyek ini membutuhkan PC Windows.

### 1. Clone repositori ini

### 2. Tambahkan file game
Taruh `default.xbe` di `assets/`, atau taruh image disc (`.iso`) di `assets/` atau `iso/`. Proses build akan mengekstrak `default.xbe` dari image tersebut.

### 3. Pasang dependensi
- [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) dengan *Desktop development with C++*. Recompiler berjalan di PC.
- [Android Studio](https://developer.android.com/studio). Di SDK Manager-nya, pasang Android SDK Platform 34, NDK `28.2.13676358`, dan CMake `3.31.6`. Gradle membutuhkan Java 17 sampai 21 (`JAVA_HOME`).
- [Python](https://www.python.org/), hanya dibutuhkan untuk mengekstrak `default.xbe` dari image disc.

### 4. Build
Jalankan `build.bat`. Skrip ini membangun recompiler, merekompilasi xbe ke `generated/game`, lalu membangun `app/build/outputs/apk/release/app-release.apk` dengan Gradle. Build pertama mengompilasi seluruh game untuk arm64 dan memakan waktu 30 sampai 90 menit.

Pasang APK-nya dengan:
```
adb install -r app/build/outputs/apk/release/app-release.apk
```

SDK disertakan di `sdk/`, pada commit upstream yang tercatat di `sdk/UPSTREAM_COMMIT`, dengan satu perubahan: bahasa konsol diambil dari `app::ConsoleLanguage()` (`sdk/runtime/kernel/xboxkrnl.cpp`).

## Linux
Game ini juga bisa dibangun dan dijalankan secara native di Linux (x86-64) dari sumber yang sama: renderer Vulkan, suara, controller, dan pengaturannya sama dengan versi Android, dengan jendela desktop sebagai pengganti layar HP. Versi Linux dibangun dari sumber.

- Kebutuhan: distribusi 64-bit dengan driver Vulkan 1.1 (Mesa atau NVIDIA), serta Clang, CMake, Ninja, Git, pkg-config, Python 3, dan loader serta header Vulkan. SDL3 diambil dari sistem bila versinya 3.2 atau lebih baru, jika tidak akan diunduh dan dibangun sekalian. `build.sh` menampilkan daftar paket untuk Arch, Debian/Ubuntu, dan Fedora bila ada yang kurang.
- Taruh `default.xbe` beserta file game, atau image disc (`.iso`), di `assets/`, lalu jalankan `./build.sh` (`./build.sh debug` untuk baris info di log). Skrip ini membangun recompiler, merekompilasi xbe ke `generated/game`, lalu membangun `out/build/linux/mostwanted`. Build pertama mengompilasi seluruh game dan memakan waktu 15 sampai 60 menit.
- Jalankan `out/build/linux/mostwanted`. Game dicari di folder `assets` atau `iso` di samping executable atau di folder mana pun di atasnya, jadi folder `assets` milik repo langsung bisa dipakai.
- Pengaturan (`mostwanted.toml`, `android.toml`, `layout.toml`), save (`hdd`), log, dan cache shader ditulis di samping executable.
- Keyboard, mouse (di panel pengaturan), dan controller lewat SDL3, di Wayland atau X11. F10 membuka panel pengaturan, F11 atau Alt+Enter beralih antara jendela dan layar penuh. `display_mode` (`fullscreen`, `borderless`, `windowed`) beserta `width` dan `height` di `mostwanted.toml` mengatur jendela.
- Menu game menampilkan LINUX OPTIONS sebagai pengganti ANDROID OPTIONS. Kontrol sentuh hanya muncul di layar sentuh, setelah layarnya disentuh.

## Kredit
- [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) oleh TGP482: recompiler statis dan runtime portabel (kernel Xbox, library XDK, dan Direct3D 8) yang menjadi dasar port ini.
- [Black-Android](https://github.com/RanggaCasper/Black-Android) dan [ShaolinMonks-Android](https://github.com/RanggaCasper/ShaolinMonks-Android) oleh RanggaCasper: host Android yang menjadi dasar port ini.
- Zukiru: screen filter HDR.
- [bagasromadon](https://github.com/bagasromadon): desain tombol.
- [SDL](https://github.com/libsdl-org/SDL), [glslang](https://github.com/KhronosGroup/glslang), [toml++](https://github.com/marzer/tomlplusplus), dan [Dear ImGui](https://github.com/ocornut/imgui).
- [xemu](https://github.com/xemu-project/xemu) dan [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded), yang dipakai XboxXBE-Recomp sebagai referensi perilaku hardware dan API Xbox.

*Need for Speed* adalah merek dagang Electronic Arts Inc. Proyek ini tidak berafiliasi dengan maupun didukung oleh mereka.
