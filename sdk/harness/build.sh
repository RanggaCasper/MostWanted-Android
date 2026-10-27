#!/bin/bash
# build.sh: the headless harness exe of one image, on Linux with clang
#   -r repo (holding sdk/ and src/)   -g generated folder of the image   -o out folder
#   -n exe name (default: the generated folder's name)
#   -x regex of sources to leave out (repeatable)   -e extra source file (repeatable)
#   -G folder for the generated objects (default <out>/gen), shared between builds of the same generated code
# env: CXX (clang++), JOBS (nproc)
# Leaves out the window, SDL input, XAudio2 and D3D12 parts of the runtime and the sources of the repo's other images
# (src/<name> for each other folder beside the generated one); the harness supplies their stand ins.
set -e
EXCL=(); EXTRA=()
while getopts r:g:o:n:x:e:G: opt; do
    case $opt in
        r) REPO=$OPTARG ;; g) GEN=$OPTARG ;; o) OUT=$OPTARG ;; n) EXE=$OPTARG ;;
        x) EXCL+=("$OPTARG") ;; e) EXTRA+=("$(realpath "$OPTARG")") ;; G) GEN_OBJ=$OPTARG ;;
        *) sed -n 2,8p "$0"; exit 2 ;;
    esac
done
[ -n "$REPO" ] && [ -n "$GEN" ] && [ -n "$OUT" ] || { sed -n 2,8p "$0"; exit 2; }
REPO=$(realpath "$REPO"); GEN=$(realpath "$GEN"); mkdir -p "$OUT"; OUT=$(realpath "$OUT")
EXE=${EXE:-$(basename "$GEN")}; GEN_OBJ=${GEN_OBJ:-$OUT/gen}; mkdir -p "$GEN_OBJ" "$OUT/host"
H=$(dirname "$(realpath "$0")"); RT=$REPO/sdk/runtime
CXX=${CXX:-clang++}; JOBS=${JOBS:-$(nproc)}
BASE="-std=c++20 -march=x86-64-v2 -fno-strict-aliasing"
HOST="$BASE -O1 -g -I$REPO/src -I$RT -I$GEN -I$H -I$H/compat -DXBE_RAW_VERTICES=0 -DXBE_RENDER_THREAD=0"

# generated code, an object rebuilt only when its partition is newer
export CXX BASE RT GEN GEN_OBJ
ls "$GEN"/x86_recomp.*.cpp "$GEN"/x86_func_mapping.cpp | xargs -P "$JOBS" -I{} sh -c '
    o="$GEN_OBJ/$(basename {} .cpp).o"; [ "$o" -nt {} ] && exit 0
    $CXX $BASE -O1 -g0 -w -I"$RT" -I"$GEN" -c {} -o "$o.tmp" && mv "$o.tmp" "$o"'

others=$(ls "$GEN/.." | grep -vx "$(basename "$GEN")" | paste -sd'|' -)
SRC=$(ls "$RT"/*/*.cpp | grep -Ev "/runtime/app/|/core/input.cpp|/gpu/d3d12.cpp|/audio/xaudio2.cpp")
SRC="$SRC $(ls "$REPO"/src/*/*.cpp | grep -Ev "/src/(${others:-^$})/" || true)"
SRC="$SRC $(ls "$H"/*.cpp) ${EXTRA[*]}"
for x in "${EXCL[@]}"; do SRC=$(echo $SRC | tr ' ' '\n' | grep -Ev -- "$x"); done

# host objects, keyed by the flags; rebuilt when the source or any header of the repo, harness or image is newer
stamp=$(echo "$HOST" | md5sum | cut -c1-8)
newest=$(find "$RT" "$REPO/src" "$H" "$GEN/image_symbols.h" -name '*.h' -printf '%T@ %p\n' | sort -n | tail -1 | cut -d' ' -f2)
objs=()
for f in $SRC; do
    o=$OUT/host/$(echo "${f#/}" | tr / _ | sed "s/\.cpp$/.$stamp.o/"); objs+=("$o")
    [ "$o" -nt "$f" ] && [ "$o" -nt "$newest" ] && continue
    echo "compile $f"
    rm -f "$o"
    $CXX $HOST -c "$f" -o "$o" &
    [ "$(jobs -r | wc -l)" -ge "$JOBS" ] && wait -n
done
wait
$CXX -o "$OUT/$EXE" "${objs[@]}" "$GEN_OBJ"/*.o -lpthread
echo "built $OUT/$EXE"
