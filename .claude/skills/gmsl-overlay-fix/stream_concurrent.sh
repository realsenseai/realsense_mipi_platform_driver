#!/bin/bash
# stream_concurrent.sh <frames> <node:WxH:FOURCC:fps>...   (run on the Jetson)
# Starts one v4l2-ctl per spec (STAGGER=<s> between starts), waits for all, prints frames per
# stream and the dmesg lines logged since the start (by kernel timestamp, so a wrapped ring is fine).
# Example: STAGGER=1 ./stream_concurrent.sh 300 "0:848x480:Z16 :30" "2:1280x720:YUYV:30"
N=${1:?frames}; shift
D=$(mktemp -d /tmp/stream_concurrent.XXXX)
if pgrep -x v4l2-ctl >/dev/null; then echo "ABORT: v4l2-ctl already running"; pgrep -a v4l2-ctl; exit 2; fi
t0=$(awk '{print $1}' /proc/uptime)
pids=()
for s in "$@"; do
    IFS=: read -r node wh fcc fps <<<"$s"
    timeout $((N / fps + 25)) v4l2-ctl -d "/dev/video$node" \
        --set-fmt-video=width=${wh%x*},height=${wh#*x},pixelformat="$fcc" --set-parm="$fps" \
        --stream-mmap --stream-count="$N" --stream-to=/dev/null > "$D/v$node.log" 2>&1 &
    pids+=($!); sleep "${STAGGER:-0}"
done
rc=0; i=0
for s in "$@"; do
    wait "${pids[$i]}"; r=$?; i=$((i + 1))
    node=${s%%:*}
    got=$(tr -cd '<' < "$D/v$node.log" | wc -c)
    echo "video$node $s: rc=$r frames=$got/$N last=$(grep -o '[0-9.]* fps' "$D/v$node.log" | tail -1)"
    [ "$r" = 0 ] && [ "$got" -ge "$N" ] || rc=1
done
sudo dmesg | awk -v t0="$t0" 'match($0, /^\[ *[0-9.]+\]/) {
    ts = substr($0, 2, RLENGTH - 2); if (ts + 0 >= t0) print }' > "$D/dmesg"
echo "--- dmesg since start: $(wc -l < "$D/dmesg") lines, $(grep -c -i -E 'err|fail|timed out' "$D/dmesg") error-like"
grep -i -E 'err|fail|timed out|warn|max96712|max9295' "$D/dmesg" | head -40
echo "RESULT rc=$rc (logs: $D)"
exit $rc
