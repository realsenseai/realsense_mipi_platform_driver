#!/bin/bash
# generate_compile_commands.sh
#
# Generates .vscode/compile_commands.json for kernel/realsense/d4xx.c
# by extracting the real compiler command from the cached .d4xx.o.cmd
# that the kernel build writes after every module build.
#
# Usage (run from repo root after a module build):
#   ./scripts/generate_compile_commands.sh
#
# Optional: pass a different .cmd file path as $1.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$REPO/.vscode/compile_commands.json"
SOURCE_FILE="$REPO/kernel/realsense/d4xx.c"

# ── locate the .d4xx.o.cmd file ───────────────────────────────────────────────
CMD_FILE="${1:-}"
if [ -z "$CMD_FILE" ]; then
  CMD_FILE="$(find "$REPO/sources_"* -name '.d4xx.o.cmd' 2>/dev/null | head -1 || true)"
fi

if [ -z "$CMD_FILE" ] || [ ! -f "$CMD_FILE" ]; then
  echo "Error: could not find .d4xx.o.cmd under sources_*/." >&2
  echo "  Run the module build first (build_all.sh), then re-run this script." >&2
  exit 1
fi
echo "Using cmd file: $CMD_FILE"

# ── extract the compiler command ──────────────────────────────────────────────
# The file contains:  cmd_<target> := <full gcc invocation> \
#                       <continuation lines>
# Grab everything after ' := ' on the first cmd_ line and join continuations.
RAW_CMD="$(sed -n '/^cmd_.*:=/{s/^cmd_[^ ]* := //; p}' "$CMD_FILE" | \
           tr -d '\\\n' | sed 's/  */ /g')"

if [ -z "$RAW_CMD" ]; then
  echo "Error: could not parse cmd_ line from $CMD_FILE" >&2
  exit 1
fi

# Try to locate the kernel source top under the same sources_* tree
# 1) extract the sources_<ver> root folder from the .cmd path
SOURCES_ROOT="$(echo "$CMD_FILE" | sed -n 's@\(.*sources_[^/]*\)/.*@\1@p' || true)"
KERNEL_TOP=""
if [ -n "$SOURCES_ROOT" ] && [ -d "$SOURCES_ROOT" ]; then
  # First check for an immediate child like sources_*/kernel*/ that looks like the kernel tree
  for d in "$SOURCES_ROOT"/*; do
    if [ -d "$d" ] && [[ "$(basename "$d")" == kernel* ]] && [ -f "$d/include/linux/kconfig.h" ]; then
      KERNEL_TOP="$d"
      break
    fi
  done
  # If not found, fall back to searching for any include/linux/kconfig.h
  if [ -z "$KERNEL_TOP" ]; then
    while IFS= read -r kp; do
      maybe_top="$(dirname "$(dirname "$kp")")"  # parent of 'include'
      if [ -f "$maybe_top/Makefile" ]; then
        KERNEL_TOP="$maybe_top"
        break
      fi
    done < <(find "$SOURCES_ROOT" -maxdepth 12 -type f -path '*/include/linux/kconfig.h' -print || true)
  fi
fi

if [ -n "$KERNEL_TOP" ]; then
  DIRECTORY="$KERNEL_TOP"
else
  # Fallback: use the object path recorded in the cmd_ key if available
  OBJ_PATH="$(sed -n 's/^cmd_\([^ ]*\) := .*$/\1/p' "$CMD_FILE" | head -1 || true)"
  if [ -n "$OBJ_PATH" ]; then
    DIRECTORY="$(dirname "$OBJ_PATH")"
  else
    DIRECTORY="$REPO"
  fi
fi

# ── rewrite the command for IDE use ───────────────────────────────────────────
# 1. Strip -Wp,-MMD,... (generates .d dependency files, confuses clangd/cpptools)
# 2. Replace the original source path with our repo's d4xx.c
# 3. Remove the -o <object> output flag (not needed for IndexDB)
CLEAN_CMD="$(echo "$RAW_CMD" | \
  sed -E 's|-Wp,-MMD,[^ ]+||g' | \
  sed -E "s|[^ ]+/d4xx\.c|$SOURCE_FILE|g" | \
  sed -E 's|-o [^ ]+\.o +||g' | \
  sed 's/  */ /g; s/^ //; s/ $//')"

# Append the source file at the end if not already present
if ! echo "$CLEAN_CMD" | grep -q "$SOURCE_FILE"; then
  CLEAN_CMD="$CLEAN_CMD $SOURCE_FILE"
fi

# If the kernel config indicates OF support, ensure CONFIG_OF is visible to the indexer
if find "$REPO/sources_"* -path '*/include/config/OF' -print -quit | grep -q .; then
  if ! echo "$CLEAN_CMD" | grep -q -- '-DCONFIG_OF'; then
    CLEAN_CMD="$CLEAN_CMD -DCONFIG_OF"
  fi
fi

# ── write compile_commands.json ───────────────────────────────────────────────
mkdir -p "$REPO/.vscode"
python3 - "$DEST" "$DIRECTORY" "$CLEAN_CMD" "$SOURCE_FILE" <<'PY'
import sys, json
dest, directory, command, src = sys.argv[1:]
db = [{"directory": directory, "command": command, "file": src}]
with open(dest, "w") as f:
  json.dump(db, f, indent=2)
print(f"Written {dest} (directory={directory})")
PY
