#!/usr/bin/env python3
"""Makes the bench's game folder: the game's own files (links) plus a fixed
mission, "00 Bench" (first in the Single Missions list): mission 01 (Malden)
with no random start or placement, no triggers, the player 800 m away and
captive, no other soldier (BENCH_AI=1 keeps them), and a 60 s camera loop over the village: 24 s high orbit looking in,
24 s low looking out (450 to 2300 draws a frame), then 12 s at eye height in
the street turning on the spot, a turn in 4 s, three times (the player turning:
where micro-stutters were seen). A 60 s bench window then holds exactly one
loop, whatever its phase.
BENCH_PATH=discover: instead, one flight town to town over the island (new
objects all along: the game building their models).
usage: bench_game.py GAME_DIR OUT_DIR   (then tools/bench.sh with BENCH_GAME=OUT_DIR)"""
import math, os, re, shutil, subprocess, sys

game, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
tools = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(out, "_src01")
shutil.rmtree(out, ignore_errors=True)
os.makedirs(os.path.join(out, "Missions"))
subprocess.run([sys.executable, "-I", os.path.join(tools, "unpbo.py"),
                os.path.join(game, "Missions", "01TakeTheCar.ABEL.pbo"), src], check=True, stdout=subprocess.DEVNULL)
m = os.path.join(out, "Missions", "00Bench.Abel")
shutil.copytree(src, m)
for f in ("mission.sqm", "vssver.scc"):
    if os.path.exists(os.path.join(m, f)): os.remove(os.path.join(m, f))
s = subprocess.run([sys.executable, "-I", os.path.join(tools, "derap.py"), os.path.join(src, "mission.sqm")],
                   check=True, capture_output=True, text=True).stdout
s = re.sub(r'\n\t*placement=[0-9.]+;', '', s)                                  # no random placement
s = re.sub(r'markers\[\]=\{[^}]*\};', 'markers[]={};', s)                     # no random start
s = re.sub(r'synchronizations\[\]=\{[^}]*\};', 'synchronizations[]={};', s)   # links to the triggers
i = s.index('\tclass Sensors')                                                 # the triggers (they end the mission)
j, depth = s.index('{', i), 0
for k in range(j, len(s)):
    depth += {'{': 1, '}': -1}.get(s[k], 0)
    if depth == 0: break
s = s[:i] + s[s.index('\n', k) + 1:]
s = s.replace('position[]={7186.188,167.0442,7909.548};', 'position[]={7041.875,77.93999,7128.558};', 1)
s = s.replace('init="group this setgroupID [""Alpha"",""Six""]";', 'init="this setCaptive true";', 1)
s = s.replace('briefingName="@STRN_S01";', 'briefingName="00 Bench";', 1)
if os.environ.get("BENCH_AI") != "1":                                       # no soldiers but the player:
    i = s.index('\tclass Groups')                                              # their AI decides at random, and
    j = s.index('\n\t\tclass Item1\n', i) + 1  # the work a frame with it
    depth, k = 0, s.index('{', i)                                               # (18 % from run to run)
    for k in range(k, len(s)):
        depth += {'{': 1, '}': -1}.get(s[k], 0)
        if depth == 0: break
    s = s[:j] + s[k:]
    s = re.sub(r'(\tclass Groups\s*\{\s*items=)[0-9.]+;', r'\g<1>1;', s, count=1)
open(os.path.join(m, "mission.sqm"), "w").write(s)
cx, cy = 7270, 7970
if os.environ.get("BENCH_PATH") == "discover":
    # one flight over Malden, town to town (the centres of the campaign's
    # units, by km square), 15 m up at 120 m/s (160 s), looking ahead, with a full turn
    # every third town: objects keep coming into view for the first time, so
    # the game keeps building their models (the stutters seen on the Mint).
    # Then the camera stays still. Nothing is seen twice: one pass, no loop.
    pts = [(7147, 7464), (7215, 6260), (7323, 5504), (7655, 4719), (8265, 3296), (7113, 3074), (6810, 3504),
           (5596, 3515), (5456, 2774), (4038, 4439), (3428, 5172), (3150, 6601), (5610, 6767), (6731, 5551)]
    L = ['; bench: one flight town to town, the same every run', 'showCinemaBorder false',
         'titleCut ["","BLACK IN", 1]', '_c = "camera" camCreate [%d, %d, 15]' % (cx, cy),
         '_c camSetTarget [%d, %d, 0]' % pts[0], '_c cameraEffect ["internal","back"]', '_c camCommit 0', '@camCommitted _c']
    x, y = cx, cy
    for n, (px, py) in enumerate(pts):
        d = math.hypot(px - x, py - y)
        L += ['_c camSetTarget [%d, %d, 0]' % (px + (px - x) * 3, py + (py - y) * 3),
              '_c camSetPos [%d, %d, 15]' % (px, py), '_c camCommit %.1f' % (d / 120), '@camCommitted _c']
        if n % 3 == 2:
            for k in range(4):
                a = math.atan2(py - y, px - x) + math.pi / 2 * (k + 1)
                L += ['_c camSetTarget [%.0f, %.0f, 0]' % (px + 500 * math.cos(a), py + 500 * math.sin(a)), '_c camCommit 1', '@camCommitted _c']
        x, y = px, py
    L += ['~1000']
    open(os.path.join(m, "init.sqs"), "w", newline="\r\n").write("\n".join(L) + "\n")
else:
  L = ['; bench: a 60 s camera loop over the village, the same every run', 'showCinemaBorder false',
       'titleCut ["","BLACK IN", 1]', '_c = "camera" camCreate [%d, %d, 40]' % (cx - 150, cy),
       '_c cameraEffect ["internal","back"]', '#loop']
  for n in range(8):
      a = 2 * math.pi * n / 8
      L += ['_c camSetTarget [%d, %d, 0]' % (cx, cy),
            '_c camSetPos [%.0f, %.0f, 40]' % (cx - 150 * math.cos(a), cy - 150 * math.sin(a)), '_c camCommit 3', '@camCommitted _c']
  for n in range(8):
      a = 2 * math.pi * n / 8
      L += ['_c camSetTarget [%.0f, %.0f, 0]' % (cx + 1500 * math.cos(a), cy + 1500 * math.sin(a)),
            '_c camSetPos [%.0f, %.0f, 12]' % (cx + 60 * math.cos(a), cy + 60 * math.sin(a)), '_c camCommit 3', '@camCommitted _c']
  L += ['_c camSetPos [%d, %d, 1.8]' % (cx, cy), '_c camSetTarget [%d, %d, 1.8]' % (cx + 200, cy), '_c camCommit 0', '@camCommitted _c']
  for n in range(12):      # eye height, turning on the spot: 12 s, a quarter turn a second
      a = 2 * math.pi * (n + 1) / 4
      L += ['_c camSetTarget [%.0f, %.0f, 1.8]' % (cx + 200 * math.cos(a), cy + 200 * math.sin(a)), '_c camCommit 1', '@camCommitted _c']
  L.append('goto "loop"')
  open(os.path.join(m, "init.sqs"), "w", newline="\r\n").write("\n".join(L) + "\n")
shutil.rmtree(src)
for e in os.listdir(game):                       # the rest: links to the game's own files
    if e in ("Missions", "Users", "ColdWarAssault.cfg"): continue
    os.symlink(os.path.realpath(os.path.join(game, e)), os.path.join(out, e))
for e in os.listdir(os.path.join(game, "Missions")):
    os.symlink(os.path.realpath(os.path.join(game, "Missions", e)), os.path.join(out, "Missions", e))
os.makedirs(os.path.join(out, "Users", "player"))
print("bench game folder:", out)
