#!/bin/sh
# Writes src/linux/ofp_region_*.c from ColdWarAssault.exe (tools/region_c.py),
# with the size of each stack object whose address the region passes on.
# Then tools/patch_gen_regions.py adds their hooks to src/recomp/gen if needed.
cd "$(dirname "$0")/.."
PY=${PY:-.venv/bin/python}
# sub_00671950, a terrain segment: the loop over its cells. Objects passed by
# address: the "found" byte of sub_00618959, the index of sub_0067A7E0, the
# positions and normals of sub_00618794 (12 bytes), the face of sub_005B6637.
REGION_TAKEN=0x13:1,0x1C:4,0x28:4,0x90:4,0xB4:12,0x14C:12,0x158:12,0x164:12,0x170:12,0x17C:12,0x188:12,0x194:12,0x1A0:0x20 \
  $PY tools/region_c.py 672996 6734DD 6758C ofp_region_00672996 > src/linux/ofp_region_00672996.c
# the same: its normals (12-byte vectors to sub_006CEA20, cross product, and
# sub_006CE940, normalisation)
REGION_TAKEN=0x90:12,0x128:12,0x14C:12,0x164:12 \
  $PY tools/region_c.py 672277 672507 6758C ofp_region_00672277 > src/linux/ofp_region_00672277.c
# the same: its faces by section (the "found" byte of sub_00618959, the face
# copied into the list)
REGION_TAKEN=0x13:1,0x1A0:0x20 \
  $PY tools/region_c.py 673741 673C09 6758C ofp_region_00673741 > src/linux/ofp_region_00673741.c
# Whole functions can be translated too (FRAME "func", END 0: its extent is
# found), for instance
#   $PY tools/region_c.py 6777C0 0 func ofp_func_006777C0
# but on small ones the lifted code was as fast (plan-acceleration.md).
