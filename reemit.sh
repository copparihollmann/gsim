#!/bin/bash
# Re-run the GSIM compiler over one SoC FIRRTL file, emitting C++ into a work dir.
#
# Usage: reemit.sh <design.fir> [outdir]
# Env:   GSIM_BIN (default: this checkout's build/gsim/gsim), GSIM_WORK (default: ./gsim-work),
#        GSIM_SUPERNODE_MAX_SIZE (65), GSIM_CPP_MAX_SIZE_KB (8192).
#
# The .fir comes from a Chipyard elaboration, e.g.
#   $MERLIN_CHIPYARD/sims/verilator/generated-src/<config>/<config>.fir
set -euo pipefail

FIR="${1:?usage: reemit.sh <design.fir> [outdir]}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GSIM_BIN="${GSIM_BIN:-$HERE/build/gsim/gsim}"
WORK="${2:-${GSIM_WORK:-$PWD/gsim-work}}/$(basename "${FIR%.fir}")"

mkdir -p "$WORK"
LOG="$WORK/reemit.log"
echo "=== reemit start $(date -u +%Y-%m-%dT%H:%M:%SZ) ===" > "$LOG"
"$GSIM_BIN" \
  --supernode-max-size="${GSIM_SUPERNODE_MAX_SIZE:-65}" \
  --cpp-max-size-KB="${GSIM_CPP_MAX_SIZE_KB:-8192}" \
  --dir "$WORK/obj" "$FIR" >> "$LOG" 2>&1
rc=$?
echo "=== reemit exit rc=$rc $(date -u +%Y-%m-%dT%H:%M:%SZ) ===" >> "$LOG"
echo "$WORK/obj"
exit $rc
