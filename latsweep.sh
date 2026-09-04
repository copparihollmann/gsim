#!/bin/bash
# Sweep the CVFPU pipeline latency of a built GSIM emulator and report, per latency, whether the
# kernel drained (all warps retired) or hit the cycle cap.
#
# Usage: latsweep.sh <emu-dir> <kernel.elf> <lat> [lat...]
# Env:   GSIM_MAX_CYCLES (default 80000), GSIM_TIMEOUT_S (default 60).
#
# The kernel ELF is a built radiance kernel, e.g.
#   $MERLIN_RADIANCE_KERNELS/kernels/<name>/kernel.soc.elf
set -euo pipefail

EMU_DIR="${1:?usage: latsweep.sh <emu-dir> <kernel.elf> <lat> [lat...]}"; shift
ELF="${1:?usage: latsweep.sh <emu-dir> <kernel.elf> <lat> [lat...]}"; shift
[ $# -gt 0 ] || { echo "no latencies given" >&2; exit 2; }

cd "$EMU_DIR"
ulimit -s unlimited 2>/dev/null || true
for LAT in "$@"; do
  OUT=$(CVFPU_LAT="$LAT" timeout "${GSIM_TIMEOUT_S:-60}" ./emu "$ELF" \
          +loadmem="$ELF" +max-cycles="${GSIM_MAX_CYCLES:-80000}" 2>&1 || true)
  PASS=$(printf '%s' "$OUT" | grep -cE 'no more active warps' || true)
  TMO=$(printf '%s' "$OUT" | grep -cE 'Timeout exceeded' || true)
  echo "CVFPU_LAT=$LAT  pass(no_more_warps)=$PASS  timeout=$TMO"
done
