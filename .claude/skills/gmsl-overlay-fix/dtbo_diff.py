#!/usr/bin/env python3
"""dtbo_diff.py <a.dtbo|a.dts> <b.dtbo|b.dts>: property-level diff of two overlays.

Flattens each overlay to sorted 'node/path: prop = value' lines and drops phandle
plumbing (phandle numbers, remote-endpoint, __symbols__/__fixups__), so only real
topology differences remain: mux channel, port-index, lanes, link-mask, VC maps.
"""
import difflib
import re
import subprocess
import sys

SKIP_NODES = {"__symbols__", "__fixups__", "__local_fixups__"}
SKIP_PROPS = re.compile(r"(phandle|remote-endpoint|maxim,gmsl-dser-device|"
                        r"nvidia,gmsl-(ser|dser)-device|i2c-parent|mux-gpios)\b")


def flatten(path_arg):
    if path_arg.endswith(".dtbo") or path_arg.endswith(".dtb"):
        text = subprocess.run(["dtc", "-q", "-I", "dtb", "-O", "dts", path_arg],
                              check=True, capture_output=True, text=True).stdout
    else:
        text = open(path_arg).read()
    path, out = [], []
    for line in text.splitlines():
        s = line.strip()
        if s.endswith("{"):
            path.append(s[:-1].strip())
            continue
        if s == "};":
            path.pop()
            continue
        if not s or SKIP_NODES & set(path) or SKIP_PROPS.match(s):
            continue
        node = "/".join(p for p in path[1:] if p not in ("fragment@0", "__overlay__"))
        out.append(node + ": " + s)
    return sorted(out)


a, b = sys.argv[1], sys.argv[2]
sys.stdout.writelines(l + "\n" for l in difflib.unified_diff(
    flatten(a), flatten(b), a, b, n=0, lineterm=""))
