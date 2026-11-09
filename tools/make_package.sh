#!/usr/bin/env bash
# Builds a self-contained copy of the recompiled game for another Linux PC
# (e.g. Linux Mint): the game, its data, and every library it loads, OpenGL
# drivers (Mesa) included, so nothing needs installing there.
# usage: tools/make_package.sh [OUTDIR] [PROGRAM]   -> OUTDIR/OFP-Recomp.tar.gz
#   LOG_DEST=user@host:folder/ makes send-logs.sh copy the logs there (scp)
#   instead of packing them into an archive.
#   PROGRAM: build-linux64/ofp (64-bit, the default) or build-linux/ofp (32-bit)
set -e
P=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$P/work/pkg}
D=$OUT/OFP-Recomp
BIN=${2:-$P/build-linux64/ofp}
if file -L "$BIN" | grep -q "ELF 64-bit"; then
    SYS=/usr/lib/x86_64-linux-gnu LOADER=/lib64/ld-linux-x86-64.so.2 LOCAL=lib/ld-linux-x86-64.so.2
    export LD_LIBRARY_PATH=$P/third_party/sysroot-x86_64/usr/lib/x86_64-linux-gnu
else
    SYS=/usr/lib/i386-linux-gnu LOADER=/lib/ld-linux.so.2 LOCAL=lib/ld-linux.so.2
    export LD_LIBRARY_PATH=$P/third_party/sysroot-i386/usr/lib/i386-linux-gnu
fi
mkdir -p "$D/lib/dri"
[ -e "$D/game" ] && { echo "$D already exists: remove it first"; exit 1; }

# the program; its loader becomes the bundled one, a path relative to the
# folder the launcher runs it from (no longer than the original)
cp "$BIN" "$D/ofp"
python3 -I - "$D/ofp" "$LOADER" "$LOCAL" <<'EOF'
import sys
p, old, new = sys.argv[1], sys.argv[2].encode() + b'\0', sys.argv[3].encode()
b = bytearray(open(p, 'rb').read()); i = b.index(old)
b[i:i + len(old)] = new + b'\0' * (len(old) - len(new))
open(p, 'wb').write(b)
EOF

# libraries: the program's, and Mesa's OpenGL (GLX vendor + gallium + DRI)
# and Vulkan (the loader, Mesa's drivers: AMD, Intel, and the software one)
VKDRV=("$SYS/libvulkan_radeon.so" "$SYS/libvulkan_intel.so" "$SYS/libvulkan_lvp.so")
roots=("$BIN" "$SYS/libGLX_mesa.so.0" "$SYS"/libgallium-*.so "$SYS/libGLX_indirect.so.0" "$SYS/dri/libdril_dri.so"
       "$SYS/libvulkan.so.1" "${VKDRV[@]}")
ldd "${roots[@]}" 2>/dev/null | awk '/=> \//{print $3} /^\t\/lib/{print $1}' | sort -u | while read -r l; do
    cp -L "$l" "$D/lib/$(basename "$l")"
done
for r in "${roots[@]:1}"; do cp -L "$r" "$D/lib/"; done
cp -L "$LOADER" "$D/$LOCAL"
cp -a "$SYS"/dri/*.so "$D/lib/dri/"
cp -L "$SYS/dri/libdril_dri.so" "$D/lib/dri/libdril_dri.so"
mkdir -p "$D/lib/vulkan"
for v in "${VKDRV[@]}"; do                       # the drivers' manifests, next to them (a relative path is the manifest's)
    n=$(basename "$v" .so); n=${n#libvulkan_}
    printf '{ "file_format_version": "1.0.1", "ICD": { "library_path": "../%s", "api_version": "1.4.0" } }\n' "$(basename "$v")" > "$D/lib/vulkan/${n}_icd.json"
done

# the game folder, links resolved (the data lives in OperationFlashpoint/)
cp -rL "$P/game" "$D/game"
# the game's menus with the recomp's options button (read before bin\resource.bin)
python3 -I "$P/tools/make_resource.py" "$P/game/BIN/RESOURCE.BIN" "$D/game/RES/bin/resource.bin"

cat > "$D/play.sh" <<'EOF'
#!/usr/bin/env bash
# Operation Flashpoint, recompiled for Linux. --fullscreen: full screen (F11 too).
P=$(cd "$(dirname "$0")" && pwd)
cd "$P" || exit 1
# the folder and the date first: once LD_LIBRARY_PATH names the bundled
# libraries, the system's own commands (mkdir, date) may crash on them
mkdir -p "$P/logs"
J="$P/logs/console-$(date +%Y%m%d-%H%M%S).txt"
export LD_LIBRARY_PATH="$P/lib" LIBGL_DRIVERS_PATH="$P/lib/dri" GBM_BACKENDS_PATH="$P/lib"
export OFP_LOG_DIR="$P/logs"
# Options.txt (NAME=value, one per line): read by the game itself
export OFP_OPTIONS="$P/Options.txt"
# Vulkan: the bundled drivers (Mesa's, as for OpenGL)
export VK_DRIVER_FILES="$P/lib/vulkan/radeon_icd.json:$P/lib/vulkan/intel_icd.json:$P/lib/vulkan/lvp_icd.json"
exec ./ofp --run --game game "$@" 2> "$J"
EOF
"$P/tools/write_send_logs.sh" "$D/send-logs.sh"
cat > "$D/Options.txt" <<'EOF2'
# Options of the recompiled Operation Flashpoint.
# Change a value, save, then start the game again.

# These options can also be changed in the game: Pause key (or Ctrl+O).

# Antialiasing (smoother edges): 0 = off, 2, 4 or 8 (higher = nicer, slower)
ANTIALIASING=4

# Vertical sync: 1 = on (frames per second capped at the screen's refresh rate), 0 = off (more frames per second)
VSYNC=1

# Frames per second counter, top left: 1 = shown, 0 = hidden
SHOW_FPS=0

# Renderer: 0 = OpenGL, 1 = Vulkan (if the graphics card supports it; OpenGL otherwise)
VULKAN=0

# Server of the Internet game list (GameSpy closed in 2014):
# the community's, or another one
MASTER_SERVER=master.ofpisnotdead.com

# Internet servers in the Multiplayer menu's list (with ping and players): 1 = yes, 0 = no
INTERNET_SERVERS=1

# The game's working memory, in MB (1024 to 2000). The original game stops at 1024:
# "Out of reserved memory" with a long view distance. 1536 by default.
GAME_MEMORY_MB=1536
EOF2
chmod +x "$D/play.sh" "$D/ofp"
cat > "$D/README.txt" <<'EOF'
Operation Flashpoint (recompiled for Linux)

Start: double-click "play.sh", then "Run" (or in a terminal: ./play.sh).
Full screen: F11.
Options (antialiasing...): the file Options.txt, or the Pause key in the game.
Logs (performance, freezes, settings): the logs/ folder. To collect them for a
bug report: double-click "send-logs.sh".
EOF
tar -C "$OUT" -cf - OFP-Recomp | pigz -1 > "$OUT/OFP-Recomp.tar.gz"
du -sh "$D" "$OUT/OFP-Recomp.tar.gz"
