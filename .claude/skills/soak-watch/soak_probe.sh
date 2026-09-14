#!/usr/bin/env bash
# soak_probe.sh — one cheap liveness snapshot of a running soak, for soak_watch.sh.
# Stage on the RIG next to soak_loop.sh; the watcher calls it once per poll over
# SSH so all the fragile quoting lives here, not in the watcher's ssh string.
#
# Usage:  bash soak_probe.sh <LOG> [PIDFILE] [USB_CHECK]
# Prints three fenced sections the watcher parses:
#   <tail of LOG>
#   ---PID---
#   ALIVE | DEAD            (is the loop's recorded PID still running?)
#   ---USB---
#   <count of matching USB devices>   (0 => device dropped off the bus)
set -u

LOG=${1:?usage: soak_probe.sh <LOG> [PIDFILE] [USB_CHECK]}
PIDFILE=${2:-$LOG.pid}
USB_CHECK=${3:-lsusb -d 8086:}

tail -n 40 "$LOG" 2>/dev/null

echo "---PID---"
if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; then
  echo ALIVE
else
  echo DEAD
fi

echo "---USB---"
# count non-empty matching lines; empty (0) means the camera is gone from USB
$USB_CHECK 2>/dev/null | grep -c . || echo 0
