#!/usr/bin/env bash
# Runs the recompiled game on a virtual screen for N seconds, then saves a
# screenshot and the log (OFP_GAME: another game folder). usage: tools/run_test.sh SECONDS OUTDIR [ofp args]
P=$(cd "$(dirname "$0")/.." && pwd)
T=${1:-30}; O=${2:-$P/work/run}; shift 2
mkdir -p "$O/fb"; rm -f "$O"/*.png
DISP=:${OFP_DISPLAY:-91}
Xvfb $DISP -screen 0 ${OFP_XVFB_SCREEN:-1600x1200x24} -fbdir "$O/fb" >/dev/null 2>&1 & X=$!
sleep 1
export LD_LIBRARY_PATH=$P/third_party/sysroot-i386/usr/lib/i386-linux-gnu
cd "$P"
DISPLAY=$DISP setsid ${OFP_BIN:-build-linux/ofp} --run --game "${OFP_GAME:-game}" --watchdog "$T" "$@" > "$O/log.txt" 2>&1 & G=$!
# the game leaves by itself at T (--watchdog: logs closed, profile counts written); 15 s grace
for i in $(seq $((T + 15))); do sleep 1; kill -0 $G 2>/dev/null || break; done
python3 -I "$P/tools/xwd2png.py" "$O/fb/Xvfb_screen0" "$O/screen.png" >/dev/null 2>&1
kill -- -$G 2>/dev/null; sleep 1; kill -9 -- -$G 2>/dev/null; kill $X 2>/dev/null
echo "log: $O/log.txt  screen: $O/screen.png"
