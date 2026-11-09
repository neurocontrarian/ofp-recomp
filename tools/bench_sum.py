#!/usr/bin/env python3
"""The bench figure from a performance log: the game's own time per frame
(main thread outside imports, the log's "game X ms/frame (Y ms)"), the game's
own CPU cycles a frame (independent of the CPU clock) and
the frames over 50 ms, over the
last WINDOW seconds before the final 5, plus fps and draws.
usage: bench_sum.py LOG [WINDOW]"""
import re, sys
log, win = sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 60
rows = []
for l in open(log, errors="replace"):
    m = re.match(r"\[ *([0-9.]+)\] fps +([0-9.]+) .*?draws/frame +([0-9.]+) .*?\| game [0-9.]+ ms/frame \(([0-9]+) ms\)(?:, (?:main thread )?([0-9.]+) Mcycles/frame)?(?:, all ([0-9.]+) Mcycles/frame)? (?:\| slow frames ([0-9]+) )?\| main thread: game code ([0-9]+)%", l)
    if m: rows.append(tuple(float(g or 0) for g in m.groups()))
if not rows: sys.exit("no per-second lines with the game time in " + log)
end = rows[-1][0] - 5
sel = [r for r in rows if end - win <= r[0] <= end]
fr = sum(r[1] for r in sel); gms = sum(r[3] for r in sel)
dr = sum(r[1] * r[2] for r in sel)
cyc = sum(r[1] * r[4] for r in sel)
acy = sum(r[1] * r[5] for r in sel)
print(f"{len(sel)} s: game {gms / fr if fr else 0:.2f} ms/frame, game {cyc / fr if fr else 0:.2f} Mcycles/frame, all {acy / fr if fr else 0:.2f} Mcycles/frame, fps {fr / len(sel):.1f}, draws/frame {dr / fr if fr else 0:.0f}, game code {sum(r[7] for r in sel) / len(sel):.0f}%, slow frames (>50 ms) {sum(r[6] for r in sel):.0f}")
