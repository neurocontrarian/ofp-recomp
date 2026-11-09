#!/bin/sh
# One-time preparation after cloning (INSTALL.md, steps 2 to 4):
#  - pcrecomp at the commit in patches/pcrecomp-base.txt, with
#    patches/pcrecomp-ofp.patch, in third_party/pcrecomp;
#  - a Python environment (.venv) with capstone and pefile;
#  - your ColdWarAssault.exe copied to work/ and analysed (RTTI, then the
#    function catalog: 5 to 25 minutes, 2 GB of memory);
#  - the lifted C in src/recomp/gen (about 15 minutes).
# usage: tools/prepare.sh /path/to/ColdWarAssault.exe
set -e
EXE_IN=$1
[ -f "$EXE_IN" ] || { echo "usage: tools/prepare.sh /path/to/ColdWarAssault.exe" >&2; exit 1; }
EXE_IN=$(readlink -f "$EXE_IN")
cd "$(dirname "$0")/.."
KNOWN_SHA256=2db665ee6edb115636e2a8dc642d5862b7d91c250f56045785e60562ff0f7ed3

# 1. pcrecomp
if [ ! -d third_party/pcrecomp ]; then
  mkdir -p third_party
  git clone https://github.com/sp00nznet/pcrecomp third_party/pcrecomp
  git -C third_party/pcrecomp checkout -q "$(cat patches/pcrecomp-base.txt)"
  git -C third_party/pcrecomp apply "$PWD/patches/pcrecomp-ofp.patch"
  echo "[*] pcrecomp ready"
fi

# 2. Python
if [ ! -x .venv/bin/python ]; then
  python3 -m venv .venv
  .venv/bin/python -m pip install -q capstone==5.0.9 pefile==2024.8.26
  echo "[*] .venv ready"
fi
PY=.venv/bin/python

# 3. the game's program, analysed
mkdir -p work/analysis
if [ "$(sha256sum "$EXE_IN" | cut -d' ' -f1)" != $KNOWN_SHA256 ]; then
  echo "[!] This ColdWarAssault.exe is not the copy the project was made with" >&2
  echo "    (sha256 $KNOWN_SHA256). run_lift.py stops if it is not 1.99." >&2
fi
[ "$EXE_IN" = "$(readlink -f work/ColdWarAssault.exe 2>/dev/null)" ] || cp "$EXE_IN" work/ColdWarAssault.exe
if [ ! -f work/analysis/functions.json ]; then
  $PY -I third_party/pcrecomp/tools/cpp/rtti.py work/ColdWarAssault.exe \
      -o work/analysis/rtti.json --seeds work/analysis/rtti_seeds.json
  echo "[*] finding the functions (5 to 25 minutes)..."
  $PY -I third_party/pcrecomp/tools/disasm/disasm32.py work/ColdWarAssault.exe \
      --seed-functions work/analysis/rtti_seeds.json -o work/analysis/functions.json \
      > work/analysis/disasm32.log
fi

# 4. the lifted C
$PY -I run_lift.py --all
echo "[*] done: now ./build-linux64.sh -j4 (INSTALL.md, step 4)"
