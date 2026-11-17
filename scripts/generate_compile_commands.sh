#!/bin/bash
# generate_compile_commands.sh
#
# Generates .vscode/compile_commands.json for the d4xx driver sources
# by extracting the real compiler commands from the cached .<obj>.o.cmd files
# that the kernel build writes after every module build.
#
# d4xx.ko is linked from two translation units, so both get an entry.
#
# Usage (run from repo root after a module build):
#   ./scripts/generate_compile_commands.sh
#
# Optional: pass a different .cmd file path as $1.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$REPO/.vscode/compile_commands.json"
D4XX_UNITS="d4xx_main d4xx_desc"

# ── locate the .d4xx_main.o.cmd file ─────────────────────────────────────────
CMD_FILE="${1:-}"
if [ -z "$CMD_FILE" ]; then
  CMD_FILE="$(find "$REPO/sources_"* -name '.d4xx_main.o.cmd' 2>/dev/null | head -1 || true)"
fi

if [ -z "$CMD_FILE" ] || [ ! -f "$CMD_FILE" ]; then
  echo "Error: could not find .d4xx_main.o.cmd under sources_*/." >&2
  echo "  Run the module build first (build_all.sh), then re-run this script." >&2
  exit 1
fi
echo "Using cmd file: $CMD_FILE"

# ── extract the compiler command ──────────────────────────────────────────────
# The file contains:  cmd_<target> := <full gcc invocation> \
#                       <continuation lines>
# Grab everything after ' := ' on the first cmd_ line and join continuations.
read_cmd() {
  sed -n '/^cmd_.*:=/{s/^cmd_[^ ]* := //; p}' "$1" | tr -d '\\\n' | sed 's/  */ /g'
}
RAW_CMD="$(read_cmd "$CMD_FILE")"

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

# ── rewrite the commands for IDE use ──────────────────────────────────────────
# 1. Strip -Wp,-MMD,... (generates .d dependency files, confuses clangd/cpptools)
# 2. Replace the original source path with our repo's copy
# 3. Remove the -o <object> output flag (not needed for IndexDB)
ARGS=()
for unit in $D4XX_UNITS; do
  SOURCE_FILE="$REPO/kernel/realsense/${unit}.c"
  UNIT_CMD_FILE="$(dirname "$CMD_FILE")/.${unit}.o.cmd"
  [ -f "$UNIT_CMD_FILE" ] || continue
  CLEAN_CMD="$(read_cmd "$UNIT_CMD_FILE" | \
    sed -E 's|-Wp,-MMD,[^ ]+||g' | \
    sed -E "s|[^ ]+/d4xx_[a-z]+\.c|$SOURCE_FILE|g" | \
    sed -E 's|-o [^ ]+\.o +||g' | \
    sed 's/  */ /g; s/^ //; s/ $//')"

  # Append the source file at the end if not already present
  if ! echo "$CLEAN_CMD" | grep -q "$SOURCE_FILE"; then
    CLEAN_CMD="$CLEAN_CMD $SOURCE_FILE"
  fi

  # If the kernel config indicates OF support, make CONFIG_OF visible to the indexer
  if find "$REPO/sources_"* -path '*/include/config/OF' -print -quit | grep -q .; then
    if ! echo "$CLEAN_CMD" | grep -q -- '-DCONFIG_OF'; then
      CLEAN_CMD="$CLEAN_CMD -DCONFIG_OF"
    fi
  fi
  ARGS+=("$SOURCE_FILE" "$CLEAN_CMD")
done

# ── write compile_commands.json ───────────────────────────────────────────────
mkdir -p "$REPO/.vscode"
python3 - "$DEST" "$DIRECTORY" "${ARGS[@]}" <<'PY'
import sys, json
dest, directory = sys.argv[1:3]
rest = sys.argv[3:]
db = [{"directory": directory, "command": cmd, "file": src}
      for src, cmd in zip(rest[::2], rest[1::2])]
with open(dest, "w") as f:
  json.dump(db, f, indent=2)
print(f"Written {dest} ({len(db)} entries, directory={directory})")
PY
