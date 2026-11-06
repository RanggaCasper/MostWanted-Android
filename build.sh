#!/usr/bin/env bash
# Linux build: build.sh [debug]  (debug: the release build's optimisation with the info lines in the log)
# The same steps as build.bat, all on Linux: the recompiler, default.xbe (from an ISO if need be), the symbols, the
# recompiled code in generated/game, then the game as out/build/linux/mostwanted.
set -e
GAME_TITLE="Need for Speed Most Wanted"
GAME_XBE=default.xbe
cd "$(dirname "$0")"
OUT=out/build/linux
TOOLS=out/build/linux-tools
VERBOSE=OFF
[ "$1" = debug ] && VERBOSE=ON

echo
echo "  $GAME_TITLE for Linux"
echo "  ======================================"
echo

fail() {
    echo
    echo "  BUILD FAILED. $1"
    echo
    exit 1
}

# ---------------------------------------------------------------- tools
echo "[1/5] Tools"
missing=
for tool in clang clang++ cmake ninja git pkg-config python3; do
    command -v $tool >/dev/null || missing="$missing $tool"
done
pkg-config --exists vulkan 2>/dev/null || missing="$missing vulkan"
if [ -n "$missing" ]; then
    echo "      missing:$missing"
    echo "      Arch:          sudo pacman -S clang lld cmake ninja git pkgconf python vulkan-headers vulkan-icd-loader sdl3"
    echo "      Debian/Ubuntu: sudo apt install clang lld cmake ninja-build git pkg-config python3 libvulkan-dev"
    echo "                     and libsdl3-dev, or SDL3's build dependencies (libx11-dev libxext-dev libwayland-dev"
    echo "                     libxkbcommon-dev libpipewire-0.3-dev libpulse-dev libasound2-dev libudev-dev libdecor-0-dev)"
    echo "      Fedora:        sudo dnf install clang lld cmake ninja-build git pkgconf python3 vulkan-loader-devel SDL3-devel"
    fail "Install the missing tools, then run this again."
fi
echo "      $(clang++ --version | head -1)"
pkg-config --exists sdl3 2>/dev/null && echo "      SDL3 $(pkg-config --modversion sdl3) from the system" ||
    echo "      no system SDL3: it is downloaded and built in"

# ---------------------------------------------------------------- the xbe
echo "[2/5] $GAME_XBE"
if [ ! -f "assets/$GAME_XBE" ]; then
    ISO=$(ls assets/*.iso iso/*.iso 2>/dev/null | head -1 || true)
    if [ -z "$ISO" ]; then
        echo "  Put the game in the assets folder first: $GAME_XBE from the USA release, or its disc image (.iso)"
        echo "    $PWD/assets"
        fail
    fi
    mkdir -p assets
    echo "      taking it from $ISO"
    python3 -I tools/xdvdfs.py "$ISO" $GAME_XBE "assets/$GAME_XBE" || fail "Read the last message above."
fi

# ---------------------------------------------------------------- recompiler
echo "[3/5] Recompiler"
cmake -S tools -B "$TOOLS" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    >/dev/null || fail "Read the last message above."
cmake --build "$TOOLS" >/dev/null || fail "Read the last message above."
SDK=$(head -1 "$TOOLS/sdk_dir.txt")
if ! grep -q '^\[symbols\]' config/game.toml 2>/dev/null; then
    "$TOOLS/xbe_symbols" "assets/$GAME_XBE" "$SDK/xdk" --out config/game.toml || fail "Read the last message above."
fi

# ---------------------------------------------------------------- recompile
echo "[4/5] Recompiling $GAME_XBE into generated/game"
"$TOOLS/xbe_recomp" config/game.toml --xdk "$SDK/xdk" >/dev/null || fail "Read the last message above."

# ---------------------------------------------------------------- the game
echo "[5/5] Game: the first build compiles the recompiled code, 15 to 60 minutes"
cmake -S . -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 -DXBE_VERBOSE_LOG=$VERBOSE \
    >/dev/null || fail "Read the last message above."
cmake --build "$OUT" --target main || fail "Read the last message above."

echo
echo "  $GAME_TITLE for Linux built."
echo "  $OUT/mostwanted"
echo
echo "  The game is found in assets (its files or the .iso) beside the executable or in any folder above it."
echo "  Settings, saves (hdd), logs and the shader cache are written beside the executable."
echo
