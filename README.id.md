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

Save disimpan di `/storage/emulated/0/MostWanted/hdd` dan log di `/storage/emulated/0/MostWanted/logs`, jadi tetap ada walaupun aplikasi di-uninstall. Setiap kali dibuka, save yang berubah disalin ke `/storage/emulated/0/MostWanted/backups/saves` (sepuluh salinan terakhir disimpan), dan save yang penulisannya terpotong (aplikasi tertutup saat game sedang menyimpan) dikembalikan dari salinan bagus terakhirnya. Saat update dari versi yang menyimpan save di `Android/data/com.racas.mostwanted/files/hdd`, save itu disalin ke sana saat pertama kali dibuka; salinan lamanya dibiarkan apa adanya. Pengaturan (`mostwanted.toml`, `android.toml`, `layout.toml`) dan cache shader tetap di `Android/data/com.racas.mostwanted/files`.

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

### Mobil Tambahan (Mods Loader)
**Most Wanted Mods Loader** punya ikon sendiri di samping ikon game. Aplikasi ini memasukkan mobil buatan untuk versi PC ke dalam game, dan mengubahnya ke format game Xbox langsung di HP:
1. Ketuk **Cari mobil di NFSAddons**: situsnya terbuka di browser. Unduh mobil Most Wanted (ZIP, 7z, atau RAR).
2. Ketuk **Pasang dari file** lalu pilih file-nya, atau buka atau bagikan file yang sudah diunduh ke Mods Loader. Paket berisi beberapa mobil memasang semua mobil di dalamnya.
3. Ketuk **Main**: mobilnya sudah ada di pemilihan mobil dan garasimu.

Mobil yang terpasang ditampilkan di Mods Loader, masing-masing dengan tombol **Hapus**. File game harus sudah terpasang lebih dulu, karena mobilnya dibangun dari file itu; aplikasi tidak mengunduh apa pun sendiri. Save tetap bisa dipakai saat mobil ditambah atau dihapus.

### My Music
Taruh lagu sendiri (MP3, M4A/AAC, OGG, Opus, FLAC, atau WAV) di `/storage/emulated/0/MostWanted/music`; beri nama `Artis - Judul`, dan subfolder menjadi albumnya. Tab **MY MUSIC** di panel roda gigi memilih apa yang diputar (My Music atau EA Trax milik game), berurutan atau acak, dan untuk tiap lagu apakah diputar di menu, saat balapan, atau keduanya, serta urutannya di daftar. Lagu mengikuti slider volume musik game dan tampil di banner lagu game.

### Cheat
Di panel roda gigi: nitro tak terbatas, speedbreaker tak terbatas, uang maksimal, tanpa polisi, buka semua, tanpa lalu lintas, dan ganti gigi otomatis di drag race.

## Status
Diuji di Xiaomi Pad 7 (Snapdragon 7+ Gen 3): boot, menu, intro karier, dan balapan bisa dimainkan, sekitar 60 FPS saat balapan.

- `xbe_symbols` menemukan 165 dari 238 fungsi XDK yang dikenal host dan semua 12 simbol data; sisanya tidak di-link ke image ini. Sebelas lagi diselesaikan secara manual di `config/game.toml`.
- Most Wanted menjalankan banyak vertex program-nya dari memori program GPU (`LoadVertexShaderProgram` dan `SelectVertexShaderDirect`), mengirim konstanta vertex secara inline, mengatur pixel combiner lewat render state, dan memakai frame buffer serta depth buffer miliknya sendiri; host menangani semuanya.
- Visibility test (silau matahari) belum dihitung: efek silau tetap mati.
- Pekerjaan khusus game sejauh ini: pengaturan ada di menu OPTIONS game sendiri (ANDROID OPTIONS, di menu utama dan menu pause); pengaturan kualitas untuk pantulan mobil dan jalan, bayangan, partikel, hujan, tetesan hujan di layar, warna kuning, dan motion blur; layar lebar (Hor+) untuk bentuk layar apa pun; SKIP MOVIES; menu bisa disentuh; editor tata letak HUD; tombol RESET yang mengembalikan mobil ke jalan; cheat; mobil tambahan; dan My Music.
- Diperbaiki di v1.3.0: game crash saat movie mulai atau berhenti (saat boot, setelah balapan) di HP dengan banyak core. Pembaca stream EA membaca data yang masih ditulis oleh thread stream.
- Diperbaiki di rilis berikutnya: force close saat loading, saat free roam, saat mulai karier, dan setelah balapan Blacklist. Xbox hanya punya satu core, sedangkan di HP thread-thread game membaca arsip yang sama dan mengambil slot dari pool yang sama secara bersamaan; bagian-bagian itu sekarang dikunci. Juga crash di musik setelah balapan boss.
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

SDK disertakan di `sdk/`, pada commit upstream yang tercatat di `sdk/UPSTREAM_COMMIT`, dengan tiga perubahan: memori guest 128 MB (`sdk/runtime/kernel/memory.h`), bahasa konsol diambil dari `app::ConsoleLanguage()` (`sdk/runtime/kernel/xboxkrnl.cpp`), dan file disc dibaca dari `MostWanted/mods` kalau ada di sana (`sdk/runtime/kernel/files.cpp`).

`tools/car_mod.py <iso> <folder mod> <keluaran>` mengubah satu mobil PC di PC, seperti yang dilakukan Mods Loader di HP.

## Kredit
- [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) oleh TGP482: recompiler statis dan runtime portabel (kernel Xbox, library XDK, dan Direct3D 8) yang menjadi dasar port ini.
- [Black-Android](https://github.com/RanggaCasper/Black-Android) dan [ShaolinMonks-Android](https://github.com/RanggaCasper/ShaolinMonks-Android) oleh RanggaCasper: host Android yang menjadi dasar port ini.
- Zukiru: screen filter HDR.
- [bagasromadon](https://github.com/bagasromadon): desain tombol.
- [SDL](https://github.com/libsdl-org/SDL), [glslang](https://github.com/KhronosGroup/glslang), [toml++](https://github.com/marzer/tomlplusplus), dan [Dear ImGui](https://github.com/ocornut/imgui).
- [xemu](https://github.com/xemu-project/xemu) dan [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded), yang dipakai XboxXBE-Recomp sebagai referensi perilaku hardware dan API Xbox.

- [Apache Commons Compress](https://commons.apache.org/proper/commons-compress/), [XZ for Java](https://tukaani.org/xz/java.html), dan [libarchive for Android](https://github.com/zhanghai/libarchive-android), yang membuka arsip mobil.
- Para pembuat mobil tambahan di [NFSAddons](https://nfsaddons.com/).

## Lisensi
Proyek ini berlisensi [GNU General Public License v3.0](LICENSE). SDK di `sdk/` tetap memakai lisensi MIT-nya sendiri (`sdk/LICENSE`), begitu juga library di atas dengan lisensinya masing-masing.

*Need for Speed* adalah merek dagang Electronic Arts Inc. Proyek ini tidak berafiliasi dengan maupun didukung oleh mereka.
