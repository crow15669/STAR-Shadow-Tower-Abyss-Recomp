#!/bin/bash
# ab.sh <name> <state> [VAR=value ...]: loads build/bin/logs/state_test/<state>.state at GS field 300 and writes fields
# FROM..TO (default 360..420, every field) into build/bin/logs/ab_<name>.
name="$1"; state="$2"; shift 2
cd "$(dirname "$0")/../build/bin" || exit 1
ISO="$(cat ../../game/iso_path.txt)"
mkdir -p "logs/ab_$name"
env PS2X_STATE_LOAD_AT="300:logs/state_test/$state.state" PS2X_PCSX2_SNAPSHOT_DIR="logs/ab_$name" \
    PS2X_PCSX2_SNAPSHOT_EVERY="${EVERY:-1}" PS2X_PCSX2_SNAPSHOT_FROM="${FROM:-360}" PS2X_PCSX2_SNAPSHOT_TO="${TO:-420}" "$@" \
    ./STAR.exe --iso "$ISO" --hidden --seconds "${SECONDS_LIMIT:-13}" > "logs/ab_$name.log" 2>&1
echo "$name: exit $? files $(ls "logs/ab_$name" | grep -c bmp)"
