#!/usr/bin/env bash
# A new build to try beside the one already installed (OFP-Recomp from
# make_package.sh): OUTDIR/ofp-NAME and OUTDIR/play-NAME.sh, to copy into the
# OFP-Recomp folder. Both versions then share the game, settings and logs;
# play.sh still starts the old one. Also a send-logs.sh (LOG_DEST: see
# make_package.sh). With NAME "-": the installed version's own files instead
# (ofp, play.sh), to replace it.
# usage: tools/make_update.sh OUTDIR PROGRAM NAME
set -e
P=$(cd "$(dirname "$0")/.." && pwd)
OUT=$1; BIN=$2; NAME=$3
if [ "$NAME" = - ]; then S= ; else S=-$NAME; fi
mkdir -p "$OUT"
cp "$BIN" "$OUT/ofp$S"
python3 -I - "$OUT/ofp$S" /lib64/ld-linux-x86-64.so.2 lib/ld-linux-x86-64.so.2 <<'PY'
import sys
p, old, new = sys.argv[1], sys.argv[2].encode() + b'\0', sys.argv[3].encode()
b = bytearray(open(p, 'rb').read()); i = b.index(old)
b[i:i + len(old)] = new + b'\0' * (len(old) - len(new))
open(p, 'wb').write(b)
PY
cat > "$OUT/play$S.sh" <<EOF2
#!/usr/bin/env bash
# Operation Flashpoint, recompiled for Linux${S:+: the test version "$NAME"}.
${S:+# Same game, settings and logs as play.sh (which starts the older version).}
P=\$(cd "\$(dirname "\$0")" && pwd)
cd "\$P" || exit 1
# the folder and the date first: once LD_LIBRARY_PATH names the bundled
# libraries, the system's own commands (mkdir, date) may crash on them
mkdir -p "\$P/logs"
J="\$P/logs/console$S-\$(date +%Y%m%d-%H%M%S).txt"
export LD_LIBRARY_PATH="\$P/lib" LIBGL_DRIVERS_PATH="\$P/lib/dri" GBM_BACKENDS_PATH="\$P/lib"
export OFP_LOG_DIR="\$P/logs"
export OFP_OPTIONS="\$P/Options.txt"
export VK_DRIVER_FILES="\$P/lib/vulkan/radeon_icd.json:\$P/lib/vulkan/intel_icd.json:\$P/lib/vulkan/lvp_icd.json"
exec ./ofp$S --run --game game "\$@" 2> "\$J"
EOF2
"$P/tools/write_send_logs.sh" "$OUT/send-logs.sh"
chmod +x "$OUT/ofp$S" "$OUT/play$S.sh"
ls -la "$OUT"
