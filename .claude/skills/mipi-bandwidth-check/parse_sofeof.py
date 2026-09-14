#!/usr/bin/env python3
# Parse camera_common:tegra_channel_capture_frame ftrace -> per-channel SOF/EOF spans and
# cross-channel offsets, and (optionally) MB/s + overlap peak.
#
#   python parse_sofeof.py <trace_file> [depthWxH] [rawWxH]
#   e.g. python parse_sofeof.py trace.txt 1280x720 1612x808
#
# Spans are reported both over the whole run and restricted to the ALL-N overlap window
# (between the last-started channel's first and last SOF) -- the latter is the steady
# coexistence state, unaffected by any per-stream HTS change that triggers only when the
# other streams join.
#
# GOTCHA: the sof:/eof: payload is "<secs>.<nanoseconds>" with ns NOT zero-padded
# (1939.5533632 = 1939 s + 5,533,632 ns = 1939.005533632 s). Parse secs and ns as separate
# integers (secs + ns/1e9); a naive float() gives a ~25x wrong span.
import re, sys, statistics as st

path = sys.argv[1]
depth_bytes = raw_bytes = None
def wh_bytes(s, bpp):
    w, h = s.lower().split('x'); return int(round(int(w)*int(h)*bpp))
if len(sys.argv) > 2: depth_bytes = wh_bytes(sys.argv[2], 2)    # Z16
if len(sys.argv) > 3: raw_bytes  = wh_bytes(sys.argv[3], 1)    # BA81 (1 byte/px, = RAW10-of-1288 wire bytes)

pat = re.compile(r'-(\d+)\s+\[\d+\].*?(sof|eof):(\d+)\.(\d+)')
events = {}
for line in open(path):
    m = pat.search(line)
    if not m: continue
    pid = int(m.group(1)); ts = int(m.group(3)) + int(m.group(4)) / 1e9
    events.setdefault(pid, []).append((m.group(2), ts))

frames = {}
for pid, evs in events.items():
    fl = []; i = 0
    while i < len(evs) - 1:
        if evs[i][0] == 'sof' and evs[i+1][0] == 'eof':
            fl.append((evs[i][1], evs[i+1][1], evs[i+1][1] - evs[i][1])); i += 2
        else: i += 1
    frames[pid] = [f for f in fl if f]
frames = {p: fl for p, fl in frames.items() if fl}

if len(frames) < 2:
    pid, fl = next(iter(frames.items()))
    sp = sorted(f[2]*1000 for f in fl)
    print(f"single channel PID {pid}: frames={len(fl)} span_median={st.median(sp):.3f}ms")
    sys.exit(0)

anchor = max(frames, key=lambda p: frames[p][0][0])    # started last
w0, w1 = frames[anchor][0][0], frames[anchor][-1][0]    # all-N window
depth = min(frames, key=lambda p: frames[p][0][0])      # started first

def span_in(fl, lo, hi):
    s = sorted(f[2]*1000 for f in fl if lo <= f[0] <= hi)
    return st.median(s) if s else float('nan')

print("=== per-channel ===  (allN = span during the all-channel overlap window)")
info = {}
for pid, fl in sorted(frames.items()):
    sp = sorted(f[2]*1000 for f in fl)
    per = sorted((fl[i+1][0]-fl[i][0])*1000 for i in range(len(fl)-1))
    alln = span_in(fl, w0, w1)
    info[pid] = dict(span=st.median(sp), alln=alln)
    role = 'depth' if pid == depth else 'RAW  '
    print(f"  {role} PID {pid}: frames={len(fl):4d}  span_all={st.median(sp):7.3f}ms  "
          f"span_allN={alln:7.3f}ms  period={st.median(per):.3f}ms")

print(f"\n=== cross-channel offsets vs anchor PID {anchor} (all-N window) ===")
print("    +dSOF = starts later ;  -dEOF = ends earlier (nested)")
def nearest(fl, t): return min(fl, key=lambda f: abs(f[0]-t))
for p in [x for x in frames if x != anchor]:
    ds = sorted((nearest(frames[p], af[0])[0]-af[0])*1000 for af in frames[anchor])
    de = sorted((nearest(frames[p], af[0])[1]-af[1])*1000 for af in frames[anchor])
    role = 'depth' if p == depth else 'RAW  '
    print(f"  {role} PID {p}: dSOF={st.median(ds):+7.3f}ms  dEOF={st.median(de):+7.3f}ms")

if depth_bytes and raw_bytes:
    raws = [p for p in info if p != depth]
    d_span = info[depth]['alln']; r_span = st.median([info[p]['alln'] for p in raws])
    d_rate = depth_bytes / (d_span/1000) / 1e6
    r_rate = raw_bytes / (r_span/1000) / 1e6
    peak = d_rate + len(raws)*r_rate
    print(f"\n=== bandwidth (all-N window) ===")
    print(f"  depth = {d_rate:6.1f} MB/s ({depth_bytes} B / {d_span:.3f}ms)")
    print(f"  RAW   = {r_rate:6.1f} MB/s each x{len(raws)} = {len(raws)*r_rate:.1f} MB/s ({raw_bytes} B / {r_span:.3f}ms)")
    print(f"  OVERLAP PEAK = {peak:6.1f} MB/s   (clean ref 720p = 196 MB/s)")
