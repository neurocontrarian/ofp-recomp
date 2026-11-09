#!/usr/bin/env bash
# Writes send-logs.sh into a package (make_package.sh, make_update.sh): it packs
# the logs and the game's .rpt into one archive to attach to a bug report or,
# when LOG_DEST (user@host:folder/) was set at packaging time, copies them
# there with scp (.log as .log.txt: some folder synchronisers skip .log).
# usage: tools/write_send_logs.sh OUTFILE
set -e
cat > "$1" <<'EOF2'
#!/usr/bin/env bash
# Collects the logs (performance, freezes, settings) and the game's report.
P=$(cd "$(dirname "$0")" && pwd)
DEST="__LOG_DEST__"
T=$(mktemp -d)
for f in "$P"/logs/* "$P"/game/ColdWarAssault.rpt; do
  [ -f "$f" ] || continue
  case $f in *.log) cp "$f" "$T/$(basename "$f").txt" ;; *) cp "$f" "$T/" ;; esac
done
if [ -n "$DEST" ]; then
  scp "$T"/* "$DEST" 2>/dev/null && echo "Logs sent." || echo "Sending failed."
else
  A="$P/logs-$(date +%Y%m%d-%H%M%S).tar.gz"
  tar -C "$T" -czf "$A" . && echo "Logs packed into $A: attach it to your bug report."
fi
rm -rf "$T"
read -r -p "Press Enter to close" _
EOF2
sed -i "s|__LOG_DEST__|${LOG_DEST:-}|" "$1"
chmod +x "$1"
