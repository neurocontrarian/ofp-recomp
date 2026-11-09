#!/bin/sh
# usage: tools/build_variant.sh DIR "GEN_OPT" [-jN] [more cmake args] — builds DIR/ofp with the memory guard on
cd "$(dirname "$0")/.."
D=$1; G=$2; J=${3:--j4}; shift 3 2>/dev/null
rm -f "$D.done"
sh tools/memguard.sh "$D.done" &
start=$(date +%s)
{ cmake -S . -B "$D" -G Ninja "-DGEN_OPT=${G:--O2}" "$@" && nice cmake --build "$D" $J; } > "$D.log" 2>&1; rc=$?
echo "rc=$rc $(( $(date +%s) - start )) s" > "$D.done"
cat "$D.done"
