#!/usr/bin/env bash
# Run a fail-closed Radiance emulator smoke on a self-checking kernel ELF.
#
# Usage: smoke_radiance_emu.sh <emulator> <kernel.soc.elf> <model-dir> <output-dir>
# Env:   MAX_CYCLES (default 2000000), TIMEOUT_SECONDS (default 120)
set -euo pipefail

if [[ $# -ne 4 ]]; then
  echo "usage: $0 <emulator> <kernel.soc.elf> <model-dir> <output-dir>" >&2
  exit 2
fi

emu="$(realpath "$1")"
elf="$(realpath "$2")"
model_dir="$(realpath "$3")"
out_dir="$(realpath -m "$4")"
max_cycles="${MAX_CYCLES:-2000000}"
timeout_seconds="${TIMEOUT_SECONDS:-120}"
[[ -x "$emu" ]] || { echo "emulator is absent or not executable: $emu" >&2; exit 2; }
[[ -f "$elf" ]] || { echo "kernel ELF is absent: $elf" >&2; exit 2; }
[[ "$max_cycles" =~ ^[1-9][0-9]*$ ]] || { echo "MAX_CYCLES must be positive" >&2; exit 2; }
[[ "$timeout_seconds" =~ ^[1-9][0-9]*$ ]] || { echo "TIMEOUT_SECONDS must be positive" >&2; exit 2; }

mapfile -t model_sources < <(find "$model_dir" -maxdepth 1 -type f \
  -name 'TestHarness*.cpp' -printf '%p\n' | sort -V)
[[ ${#model_sources[@]} -eq 224 ]] || {
  echo "expected 224 emitted model sources, found ${#model_sources[@]}" >&2
  exit 2
}
success_exit_sites="$(grep -h -oF 'exit(0);' "${model_sources[@]}" | wc -l)"
if [[ "$success_exit_sites" -ne 1 ]]; then
  echo "expected exactly one emitted hardware success exit, found $success_exit_sites" >&2
  exit 2
fi

mkdir -p "$out_dir"
stdout="$out_dir/smoke.stdout"
stderr="$out_dir/smoke.stderr"
metrics="$out_dir/smoke.time"
command=(timeout "$timeout_seconds" "$emu" "$elf" "+loadmem=$elf" \
  +max_core_cycles=0 "+max-cycles=$max_cycles")
printf '%q ' "${command[@]}" > "$out_dir/smoke.command"
printf '\n' >> "$out_dir/smoke.command"

set +e
/usr/bin/time -f '%e\t%M\t%x' -o "$metrics" "${command[@]}" > "$stdout" 2> "$stderr"
rc=$?
set -e
read -r wall_seconds max_rss_kib timed_status < "$metrics"

failure_pattern='Timeout exceeded|DEADBEEF|Assertion failed|\*\*\* FAILED \*\*\*|FINISHED:.*done=0'
failure_matches="$({ grep -h -E "$failure_pattern" "$stdout" "$stderr" 2>/dev/null || true; } | wc -l)"
last_observed_cycle="$({ grep -h -oE '^C[0-9]+: [0-9]+' "$stdout" "$stderr" || true; } \
  | awk '{if ($2 > max) max=$2} END {print max+0}')"

status=pass
reason="rc0 reached the model's sole hardware success exit before the cycle/time cap"
if [[ "$rc" -ne 0 || "$timed_status" -ne 0 ]]; then
  status=fail
  reason="emulator command returned non-zero"
elif [[ "$failure_matches" -ne 0 ]]; then
  status=fail
  reason="console contains a timeout, mismatch, assertion, or failure witness"
elif grep -q 'FINISHED:' "$stdout" "$stderr"; then
  status=fail
  reason="control returned through the harness loop rather than the emitted hardware success exit"
elif [[ "$last_observed_cycle" -le 0 || "$last_observed_cycle" -ge "$max_cycles" ]]; then
  status=fail
  reason="no positive sub-cap hardware execution cycle was observed"
fi

python3 - "$status" "$reason" "$emu" "$elf" "$model_dir" "$max_cycles" \
  "$timeout_seconds" "$success_exit_sites" "$last_observed_cycle" "$wall_seconds" \
  "$max_rss_kib" "$rc" "$stdout" "$stderr" "$out_dir/smoke.command" \
  > "$out_dir/smoke_record.json" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

(status, reason, emu, elf, model_dir, max_cycles, timeout_seconds, success_exit_sites,
 last_cycle, wall_seconds, max_rss_kib, rc, stdout, stderr, command) = sys.argv[1:]

def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            h.update(chunk)
    return h.hexdigest()

record = {
    "schema": "gsim.radiance-smoke.v1",
    "status": status,
    "reason": reason,
    "emulator": {"path": emu, "sha256": digest(emu)},
    "elf": {"path": elf, "sha256": digest(elf)},
    "model_dir": model_dir,
    "command": Path(command).read_text().strip(),
    "max_cycles": int(max_cycles),
    "timeout_seconds": int(timeout_seconds),
    "emitted_hardware_success_exit_sites": int(success_exit_sites),
    "last_observed_cycle": int(last_cycle),
    "wall_seconds": float(wall_seconds),
    "max_rss_kib": int(max_rss_kib),
    "return_code": int(rc),
    "stdout": {"path": stdout, "sha256": digest(stdout)},
    "stderr": {"path": stderr, "sha256": digest(stderr)},
}
json.dump(record, sys.stdout, indent=2, sort_keys=True)
sys.stdout.write("\n")
PY

if [[ "$status" != pass ]]; then
  echo "Radiance smoke failed: $reason (see $out_dir/smoke_record.json)" >&2
  exit 1
fi
echo "$out_dir/smoke_record.json"
