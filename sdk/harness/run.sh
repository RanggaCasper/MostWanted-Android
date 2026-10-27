#!/bin/bash
# run.sh: headless instances of a harness exe side by side, as system link peers when more than one
#   -x exe   -g game folder   -o out folder (instance k runs in <out>/<k>)   -t seconds (60)
#   -s pad script, once per instance in order (instance k gets the k-th, the last repeats)   -n instances (default: scripts, or 1)
#   -d seconds between starts (2)
# env is passed through (REF, CAP, PROBES ...); with more than one instance each gets INSTANCE=k
set -e
T=60; D=2; SCRIPTS=()
while getopts x:g:o:t:s:n:d: opt; do
    case $opt in
        x) EXE=$(realpath "$OPTARG") ;; g) GAME=$(realpath "$OPTARG") ;; o) OUT=$OPTARG ;; t) T=$OPTARG ;; d) D=$OPTARG ;;
        s) SCRIPTS+=("$(realpath "$OPTARG")") ;; n) N=$OPTARG ;; *) sed -n 2,6p "$0"; exit 2 ;;
    esac
done
[ -n "$EXE" ] && [ -n "$GAME" ] && [ -n "$OUT" ] || { sed -n 2,6p "$0"; exit 2; }
N=${N:-${#SCRIPTS[@]}}; [ "$N" -gt 0 ] || N=1
pids=()
for k in $(seq 1 "$N"); do
    dir=$OUT/$k; mkdir -p "$dir"
    s=; [ ${#SCRIPTS[@]} -gt 0 ] && s=${SCRIPTS[$((k - 1))]:-${SCRIPTS[-1]}}
    (
        cd "$dir"
        [ "$N" -gt 1 ] && export INSTANCE=$k
        [ -n "$s" ] && export SCRIPT=$s
        exec "$EXE" "$GAME"
    ) > "$dir/run.out" 2>&1 &
    pids+=($!)
    [ "$k" -lt "$N" ] && sleep "$D"
done
sleep "$T"
kill "${pids[@]}" 2>/dev/null || true
wait 2>/dev/null || true
for k in $(seq 1 "$N"); do
    echo "instance $k: $(grep -h 'renderer: frame' "$OUT/$k"/logs/*.log 2>/dev/null | tail -1 | grep -o 'frame [0-9]*, [0-9]* draws')"
done
