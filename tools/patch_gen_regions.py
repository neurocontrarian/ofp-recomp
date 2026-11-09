#!/usr/bin/env python3
"""Adds run_lift.py's PATCHES to the generated C already on disk, in every
function that lifts the patched instruction (the same text run_lift.py adds
when it generates them), so a new patch needs no full regeneration. A line
already patched is left alone; an older version of a region's hook (the
line calling ofp_region_on with the same start) is taken out first.
usage: tools/patch_gen_regions.py"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)
os.chdir(HERE)
import run_lift  # noqa: E402

for path in sorted(glob.glob('src/recomp/gen/recomp_*.c')):
    src = open(path).read()
    if not any('/* 0x%08X:' % va in src for va in run_lift.PATCHES):
        continue
    out, changed = [], 0
    funcs = src.split('\nvoid ')
    for k, f in enumerate(funcs):
        for va, c in run_lift.PATCHES.items():
            m = re.search(r'ofp_region_on\(0x[0-9A-F]{8}u\)', c)
            if m and c not in f:
                f = '\n'.join(l for l in f.split('\n') if m.group(0) not in l)
            if '/* 0x%08X:' % va in f and c not in f:
                f = run_lift.apply_patches(f, {va: c})
                changed += 1
        out.append(f)
    if changed:
        open(path, 'w', newline='\n').write('\nvoid '.join(out))
        print('%s: %d functions patched' % (path, changed))
