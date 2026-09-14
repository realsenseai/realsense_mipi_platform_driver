#!/bin/bash
# Capture per-VI-channel hardware SOF/EOF for an N-stream RS400 config on a Jetson,
# to measure stream interleaving + bandwidth. Tracepoint camera_common:tegra_channel_capture_frame
# emits hardware sof:/eof: timestamps ("<secs>.<nanoseconds>", ns NOT zero-padded) per frame.
# Streams are started staggered so each VI channel ends up as a distinct PID in the trace.
#
# Env overrides (defaults = D401 dual-RGB):
#   DEPTH_DEV=/dev/video0  DEPTH_W=1280 DEPTH_H=720
#   RAW_DEVS="/dev/video2 /dev/video4"  RAW_W=1612 RAW_H=808 RAW_FMT=BA81
#   FPS=30   OUT=/tmp/sofeof_phased.txt
set -u
DEPTH_DEV=${DEPTH_DEV:-/dev/video0}; DEPTH_W=${DEPTH_W:-1280}; DEPTH_H=${DEPTH_H:-720}
RAW_DEVS=${RAW_DEVS:-"/dev/video2 /dev/video4"}; RAW_W=${RAW_W:-1612}; RAW_H=${RAW_H:-808}
RAW_FMT=${RAW_FMT:-BA81}; FPS=${FPS:-30}; OUT=${OUT:-/tmp/sofeof_phased.txt}

TR=/sys/kernel/debug/tracing
[ -d /sys/kernel/tracing/events ] && TR=/sys/kernel/tracing
EV=$TR/events/camera_common/tegra_channel_capture_frame/enable
SU=""; [ -w "$TR/trace" ] || SU="sudo -n"
tw(){ $SU bash -c "echo $1 > $2"; }
if ! $SU bash -c "test -e $EV"; then echo "FATAL: $EV not found"; exit 2; fi

# formats
v4l2-ctl -d "$DEPTH_DEV" --set-fmt-video=width=$DEPTH_W,height=$DEPTH_H >/dev/null 2>&1
v4l2-ctl -d "$DEPTH_DEV" --set-parm=$FPS >/dev/null 2>&1
for d in $RAW_DEVS; do
  v4l2-ctl -d "$d" --set-fmt-video=width=$RAW_W,height=$RAW_H,pixelformat=$RAW_FMT >/dev/null 2>&1
  v4l2-ctl -d "$d" --set-parm=$FPS >/dev/null 2>&1
done

BASE=$($SU dmesg 2>/dev/null | grep -c 'discarding frame')

tw 0 "$TR/tracing_on"; $SU bash -c "echo > $TR/trace"; tw 1 "$EV"; tw 1 "$TR/tracing_on"

# total run = 3s per stream of head start, +5s all-together
N=$(echo $RAW_DEVS | wc -w); TOT=$(( (N+1)*3 + 5 ))
echo "PHASE depth-alone   $(date +%T.%N)"
timeout $TOT v4l2-ctl -d "$DEPTH_DEV" --stream-mmap --stream-count=100000 >/tmp/se_depth.log 2>&1 &
i=0
for d in $RAW_DEVS; do
  sleep 3; i=$((i+1)); rem=$(( TOT - i*3 ))
  echo "PHASE +RAW $d        $(date +%T.%N)"
  timeout $rem v4l2-ctl -d "$d" --stream-mmap --stream-count=100000 >/tmp/se_raw$i.log 2>&1 &
done
wait
echo "STREAMS_DONE        $(date +%T.%N)"

tw 0 "$TR/tracing_on"; $SU cp "$TR/trace" "$OUT"; $SU chown $(id -un):$(id -gn) "$OUT" 2>/dev/null; tw 0 "$EV"
POST=$($SU dmesg 2>/dev/null | grep -c 'discarding frame')

echo "=== summary ==="
echo "trace_lines=$(wc -l < $OUT)"
echo "distinct_PIDs (one per channel expected):"; sed -nE 's/^.* -([0-9]+) +\[.*/\1/p' "$OUT" | sort | uniq -c
echo -n "delivered: depth=$(tr -cd '<' </tmp/se_depth.log|wc -c)"; for j in $(seq 1 $N); do echo -n " raw$j=$(tr -cd '<' </tmp/se_raw$j.log|wc -c)"; done; echo
echo "short_frame_discards_during_run=$((POST-BASE))"
echo "OUT=$OUT"
