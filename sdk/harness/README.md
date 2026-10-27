# Headless harness

Builds and runs one image of a game on Linux without a window, GPU, audio device or pad: the recompiled code and
the SDK runtime as they are, with these stand ins.

- `app_headless.cpp` the app side: default settings, the run folder as the exe folder, no splash or handover
- `pad_script.cpp` port 0 from a script, then `GameInfo::pad` as on Windows
- `null_audio.cpp` the mixer pulled every 10 ms, output dropped
- `null_backend.cpp` counts frames, lists draws, draws chosen frames with `reference_renderer.cpp` (CPU, into guest memory)
- `main_test.cpp` runs `GameInfo::init` and `window` (with no window), then boots the image
- `compat/` the few Win32, Winsock and DXGI names portable sources use

Needs clang and the game's generated code. The game repo supplies stand ins for its own Windows only sources.

## Build

```
sdk/harness/build.sh -r <repo> -g <generated/image> -o <out> [-n exe] [-x regex]... [-e source]... [-G gen objects]
```

`-x` leaves sources out, `-e` adds sources, `-G` shares the generated objects between builds.

## Run

```
sdk/harness/run.sh -x <exe> -g <game folder> -o <out> [-t seconds] [-s script]... [-n instances] [-d seconds]
```

Instance k runs in `<out>/k` with its own `logs`, `hdd` and `cap`. With more than one instance each gets
`INSTANCE=k` and they find each other over system link on 127.0.0.k. The exe alone: `<exe> <game folder> [--instance N]`.

Environment:

- `SCRIPT` pad script: lines of `frame buttons [frames held]` (default 8), `#` comments, buttons joined with `+`:
  `UP DOWN LEFT RIGHT START BACK A B X Y LT RT LS LSD LSL LSR`. Frames are presented frames.
- `cmd` file in the run folder: `buttons [frames held]`, pressed once when read
- `REF=N` every Nth frame drawn by the reference renderer into `cap/ref_<frame>.tga`
- `snap` file in the run folder: the next two frames drawn into `snap_1.tga` and `snap_2.tga`
- `CAP=N` every Nth frame's draws listed in `cap/draws_<frame>.txt`
- `PROBES="x,y x,y"` log every colour the reference renderer writes at those pixels of linear `PROBE_WIDTH` (640) wide
  targets; `TRACE="x,y"` logs the combiner registers there
- `XBE_PERF` as on Windows

A frame drawn by the reference renderer takes long; a button held across one can auto repeat in the game's menus.
