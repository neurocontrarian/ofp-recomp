#!/usr/bin/env bash
# The bench: the first single mission on a virtual screen, the same settings
# every run (work/bench-cfg: UserInfo.cfg and ColdWarAssault.cfg copied in
# first), no vsync, the NVIDIA card when there is one (else lavapipe); prints the game's own time per frame over the last
# BENCH_WINDOW seconds of play (tools/bench_sum.py).
# usage: tools/bench.sh OUTDIR [BIN]   (BIN: default build-linux64/ofp)
P=$(cd "$(dirname "$0")/.." && pwd)
O=$(mkdir -p "$1" && cd "$1" && pwd); B=${2:-$P/build-linux64/ofp}
C=${BENCH_CFG:-$P/work/bench-cfg}
# BENCH_GAME: a game folder of its own (a fixed mission added: tools/bench_game.py)
G=${BENCH_GAME:-$P/game}; export OFP_GAME=$G
mkdir -p "$G/Users/player"
cp "$C/UserInfo.cfg" "$G/Users/player/UserInfo.cfg"
cp "$C/ColdWarAssault.cfg" "$G/ColdWarAssault.cfg"
: > "$O/options.txt"
# menu → Single Mission → first mission → briefing → play
STEPS=${BENCH_STEPS:-"sleep:5 key:Return sleep:10 key:Down sleep:4 key:Return sleep:40 key:Return"}
# a fixed frame rate (OFP_FPS_CAP, below what every build reaches): the same frames every run
export OFP_FPS_CAP=${OFP_FPS_CAP:-25}
export OFP_BIN=$B OFP_VSYNC=0 OFP_LOG_DIR=$O OFP_OPTIONS=$O/options.txt
export OFP_RENDERER=${OFP_RENDERER:-vulkan} VK_DRIVER_FILES=${VK_DRIVER_FILES:-$(ls /usr/share/vulkan/icd.d/nvidia_icd.json 2>/dev/null || echo /usr/share/vulkan/icd.d/lvp_icd.json)}
export OFP_XVFB_SCREEN=${OFP_XVFB_SCREEN:-1280x1024x24} OFP_SHOT_EVERY=${OFP_SHOT_EVERY:-10}
# the CPU clock while it runs (a power-saving governor moves it): mean MHz of the
# cores the bench runs on, every 2 s
( while :; do awk -v c="${BENCH_PIN:-0-19}" '/^processor/ {p=$3} /^cpu MHz/ {print p, $4}' /proc/cpuinfo; sleep 2; done ) > "$O/mhz.txt" & MHZ=$!
${BENCH_PIN:+taskset -c $BENCH_PIN} "$P/tools/ui_test.sh" "${BENCH_SECONDS:-180}" "$O" "$STEPS" > "$O/ui.txt" 2>&1
# the frames kept to see the menu come up: only the last 3 stay (1.6 GB a run otherwise)
ls "$O/shots" | head -n -3 | sed "s|^|$O/shots/|" | xargs -r rm -f
rm -f "$O"/fb/*
kill $MHZ 2>/dev/null
echo "$(python3 -I "$P/tools/bench_sum.py" "$O"/perf-*.log ${BENCH_WINDOW:-60}), clock $(awk -v pin="${BENCH_PIN:-0-19}" 'BEGIN {n = split(pin, r, ","); for (i = 1; i <= n; i++) { if (split(r[i], ab, "-") == 2) for (c = ab[1]; c <= ab[2]; c++) ok[c] = 1; else ok[r[i]] = 1 } } ok[$1] {s += $2; k++} END {printf "%.0f", k ? s / k : 0}' "$O/mhz.txt") MHz"
free=$(df -BG --output=avail "$O" | tail -1 | tr -dc 0-9)
[ "$free" -lt 30 ] && echo "WARNING: only $free GB left on the disk" >&2
