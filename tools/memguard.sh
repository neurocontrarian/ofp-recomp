#!/bin/sh
# Keeps a build from running out of memory: below 2 GB free, pauses the
# newest compiler; above 3 GB, or when every compiler is paused (else nothing
# would ever free memory again: paused ones keep theirs), resumes the oldest
# paused one. One build at a time: it watches every cc1 on the machine.
# usage: tools/memguard.sh STOPFILE   (exits, resuming all, once STOPFILE exists)
F=$1; stopped=""
while :; do
  a=$(awk '/MemAvailable/ {print $2}' /proc/meminfo)
  running=$(ps -eo pid=,stat=,comm= | awk '$3=="cc1" && $2!~/T/ {print $1}')
  if [ "$a" -lt 2000000 ] && [ "$(echo "$running" | wc -w)" -gt 1 ]; then
    p=$(ps -eo pid=,stat=,comm= --sort=-start_time | awk '$3=="cc1" && $2!~/T/ {print $1; exit}')
    kill -STOP "$p" && stopped="$stopped $p"
  elif [ -n "$stopped" ] && { [ "$a" -gt 3000000 ] || [ -z "$running" ]; }; then
    set -- $stopped; kill -CONT "$1" 2>/dev/null; shift; stopped="$*"
  fi
  [ -f "$F" ] && break
  sleep 2
done
for p in $stopped; do kill -CONT "$p" 2>/dev/null; done
