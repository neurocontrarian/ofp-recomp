#!/usr/bin/env bash
# Runs the game on a virtual screen, waits for the main menu, plays input
# steps (tools/xinput.py) and keeps a frame every OFP_SHOT_EVERY frames.
# usage: tools/ui_test.sh SECONDS OUTDIR "xinput steps..." [-- game args]
P=$(cd "$(dirname "$0")/.." && pwd)
T=$1; O=$(mkdir -p "$2" && cd "$2" && pwd); STEPS=$3; shift 3
mkdir -p "$O/shots"
export OFP_SHOT="$O/shots" OFP_SHOT_EVERY=${OFP_SHOT_EVERY:-10}
DISP=${OFP_DISPLAY:-98}
OFP_DISPLAY=$DISP "$P/tools/run_test.sh" "$T" "$O" "$@" > /dev/null &
R=$!
# the menu is up once the game has drawn UI_WAIT_SHOTS frames kept (12)
until [ "$(ls "$O/shots" 2>/dev/null | wc -l)" -ge ${UI_WAIT_SHOTS:-12} ] || ! kill -0 $R 2>/dev/null; do sleep 1; done
python3 -I "$P/tools/xinput.py" ":$DISP" $STEPS
wait $R
echo "frames: $(ls "$O/shots" | wc -l)"
