#!/usr/bin/env bash
# Build a RadianceGsimConfig emulator from GSIM-emitted TestHarness C++.
#
# This starts at an existing FIRRTL/GSIM emission boundary.  It does not run, or
# claim to run, Chipyard elaboration.  If the optional FIRRTL path is supplied,
# its digest is recorded as an adopted pre-existing input.
#
# Usage:
#   build_radiance_emu.sh <emitted-model-dir> <output-dir> [source.fir]
#
# Environment:
#   JOBS                 parallel C++ jobs (default: 4)
#   CXX                  clang++ or wrapper (default: ./cxxwrap.sh)
#   CHIPYARD             Chipyard checkout (default below)
#   FESVR_PREFIX         include/lib prefix (default: $CHIPYARD/.conda-env/riscv-tools)
#   SOFTFLOAT_ROOT       directory containing softfloat.h
#   SOFTFLOAT_LIB        libsoftfloat.a path
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "usage: $0 <emitted-model-dir> <output-dir> [source.fir]" >&2
  exit 2
fi

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
invocation_cwd="$(pwd -P)"
model_dir="$(realpath "$1")"
out_dir="$(realpath -m "$2")"
firrtl="${3:-}"
jobs="${JOBS:-4}"
cxx="${CXX:-$here/cxxwrap.sh}"
chipyard="${CHIPYARD:-/scratch/agustin/projects/chipyard}"
fesvr_prefix="${FESVR_PREFIX:-$chipyard/.conda-env/riscv-tools}"
softfloat_root="${SOFTFLOAT_ROOT:-$chipyard/toolchains/riscv-tools/riscv-isa-sim/softfloat}"
softfloat_lib="${SOFTFLOAT_LIB:-$chipyard/toolchains/riscv-tools/riscv-isa-sim/build/libsoftfloat.a}"
testchip_csrc="$chipyard/generators/testchipip/src/main/resources/testchipip/csrc"
fesvr_lib="$fesvr_prefix/lib/libfesvr.a"
harness_dir="$here/radiance_harness"

[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "JOBS must be a positive integer" >&2; exit 2; }
for required in \
  "$model_dir/TestHarness.h" "$model_dir/TestHarness0.cpp" "$model_dir/TestHarness1.cpp" \
  "$cxx" "$harness_dir/main.cpp" "$harness_dir/blackboxes.cpp" \
  "$harness_dir/cvfpu_model.cpp" "$testchip_csrc/mm.cc" \
  "$testchip_csrc/testchip_tsi.cc" "$testchip_csrc/testchip_htif.cc" \
  "$fesvr_lib" "$softfloat_root/softfloat.h" "$softfloat_lib"; do
  [[ -f "$required" ]] || { echo "required input is missing: $required" >&2; exit 2; }
done
if [[ -n "$firrtl" ]]; then
  firrtl="$(realpath "$firrtl")"
  [[ -f "$firrtl" ]] || { echo "FIRRTL input is missing: $firrtl" >&2; exit 2; }
fi

mapfile -t model_sources < <(find "$model_dir" -maxdepth 1 -type f \
  -name 'TestHarness*.cpp' -printf '%p\n' | sort -V)
if [[ ${#model_sources[@]} -ne 224 ]]; then
  echo "refusing Radiance build: expected 224 TestHarness*.cpp files, found ${#model_sources[@]}" >&2
  exit 2
fi

mkdir -p "$out_dir/obj" "$out_dir/logs"
if find "$out_dir/obj" -mindepth 1 -maxdepth 1 -type f | grep -q .; then
  echo "refusing non-fresh output directory: $out_dir/obj is not empty" >&2
  exit 2
fi
helper_snapshot="$out_dir/build_radiance_emu.invoked.sh"
cp "$(realpath "$0")" "$helper_snapshot"

commands_log="$out_dir/commands.log"
timings="$out_dir/timings.tsv"
metadata="$out_dir/build_metadata.txt"
: > "$commands_log"
printf 'stage\titem\twall_seconds\tmax_rss_kib\tstatus\n' > "$timings"

quote_command() {
  printf '%q ' "$@"
  printf '\n'
}

record_command() {
  local stage="$1"
  shift
  {
    printf '[%s] ' "$stage"
    quote_command "$@"
  } >> "$commands_log"
}

elapsed_seconds() {
  local start_ns="$1"
  local end_ns="$2"
  awk -v start="$start_ns" -v end="$end_ns" 'BEGIN { printf "%.3f", (end-start)/1000000000 }'
}

# GSIM leaks exactly these two uint16 im2col temporaries for each of the two
# Radiance Gemmini tiles across C++ partitions.  TestHarness1.cpp consumes each
# twice without a declaration; their producer-local definitions live in other
# partitions.  A translation-unit-only include supplies their reset value.  The
# census is deliberately strict: any emitter change must be reviewed, not
# silently papered over.
mapfile -t leaked_symbols < <(
  grep -oE '[[:alnum:]_$]+_modulo_block_(done|save)_T' "$model_dir/TestHarness1.cpp" | sort -u
)
if [[ ${#leaked_symbols[@]} -ne 4 ]]; then
  echo "refusing workaround: expected exactly 4 im2col temp symbols in TestHarness1.cpp, found ${#leaked_symbols[@]}" >&2
  printf '  %s\n' "${leaked_symbols[@]}" >&2
  exit 2
fi
for tile in 3 6; do
  for suffix in done save; do
    matches=0
    for symbol in "${leaked_symbols[@]}"; do
      if [[ "$symbol" == *"radiance_gemmini_tile_${tile}"*"im2col\$_modulo_block_${suffix}_T" ]]; then
        matches=$((matches + 1))
        count="$(grep -oF "$symbol" "$model_dir/TestHarness1.cpp" | wc -l)"
        if [[ "$count" -ne 2 ]]; then
          echo "refusing workaround: $symbol has $count uses in TestHarness1.cpp, expected 2" >&2
          exit 2
        fi
      fi
    done
    if [[ "$matches" -ne 1 ]]; then
      echo "refusing workaround: expected one tile_${tile} modulo_block_${suffix} symbol, found $matches" >&2
      exit 2
    fi
  done
done

workaround="$out_dir/radiance_im2col_partition_shim.h"
{
  echo '#pragma once'
  echo '#include <cstdint>'
  echo '// GSIM cross-partition reset temporaries; validated by build_radiance_emu.sh.'
  for symbol in "${leaked_symbols[@]}"; do
    printf 'static uint16_t %s = 0;\n' "$symbol"
  done
} > "$workaround"

common_flags=(
  -O1 -std=c++2b -DNDEBUG -fbracket-depth=4096 -fdollars-in-identifiers
  -I"$model_dir" -I"$harness_dir" -I"$testchip_csrc"
  -I"$fesvr_prefix/include" -I"$softfloat_root"
)

compile_one() {
  local src="$1"
  local obj="$2"
  local name="$3"
  shift 3
  local -a cmd=("$cxx" "${common_flags[@]}" "$@" -c "$src" -o "$obj")
  local metrics="$out_dir/logs/$name.time"
  local wall_seconds max_rss_kib rc
  record_command compile "${cmd[@]}"
  set +e
  /usr/bin/time -f '%e\t%M' -o "$metrics" \
    "${cmd[@]}" > "$out_dir/logs/$name.stdout" 2> "$out_dir/logs/$name.stderr"
  rc=$?
  set -e
  read -r wall_seconds max_rss_kib < "$metrics"
  printf 'compile\t%s\t%s\t%s\t%s\n' "$name" "$wall_seconds" "$max_rss_kib" "$rc" >> "$timings"
  return "$rc"
}

pids=()
names=()
wait_one() {
  local done_pid rc=0 index=-1 name=unknown
  wait -n -p done_pid "${pids[@]}" || rc=$?
  for i in "${!pids[@]}"; do
    if [[ "${pids[$i]}" == "$done_pid" ]]; then
      index="$i"
      name="${names[$i]}"
      break
    fi
  done
  if [[ "$index" -lt 0 ]]; then
    echo "internal error: completed compiler pid $done_pid was not tracked" >&2
    exit 2
  fi
  unset 'pids[index]' 'names[index]'
  pids=("${pids[@]}")
  names=("${names[@]}")
  if [[ "$rc" -ne 0 ]]; then
    echo "compile failed: $name (see $out_dir/logs/$name.stderr)" >&2
    for live_pid in "${pids[@]}"; do kill "$live_pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
    exit 1
  fi
}

build_start_ns="$(date +%s%N)"
for src in "${model_sources[@]}"; do
  base="$(basename "$src" .cpp)"
  extra=()
  if [[ "$base" == TestHarness1 ]]; then
    extra=(-include "$workaround")
  fi
  compile_one "$src" "$out_dir/obj/$base.o" "$base" "${extra[@]}" &
  pids+=("$!")
  names+=("$base")
  if [[ ${#pids[@]} -ge $jobs ]]; then wait_one; fi
done

support_sources=(
  "$harness_dir/main.cpp"
  "$harness_dir/blackboxes.cpp"
  "$harness_dir/cvfpu_model.cpp"
  "$testchip_csrc/mm.cc"
  "$testchip_csrc/testchip_tsi.cc"
  "$testchip_csrc/testchip_htif.cc"
)
for src in "${support_sources[@]}"; do
  base="$(basename "$src")"
  base="${base%.*}"
  compile_one "$src" "$out_dir/obj/$base.o" "$base" &
  pids+=("$!")
  names+=("$base")
  if [[ ${#pids[@]} -ge $jobs ]]; then wait_one; fi
done
while [[ ${#pids[@]} -gt 0 ]]; do wait_one; done

mapfile -t objects < <(find "$out_dir/obj" -maxdepth 1 -type f -name '*.o' -printf '%p\n' | sort -V)
if [[ ${#objects[@]} -ne 230 ]]; then
  echo "refusing link: expected 230 objects (224 model + 6 support), found ${#objects[@]}" >&2
  exit 2
fi

emu="$out_dir/emu_radiance_gsimconfig"
link_cmd=("$cxx" -o "$emu" "${objects[@]}" "$fesvr_lib" "$softfloat_lib" -lpthread -ldl -lgmp)
record_command link "${link_cmd[@]}"
set +e
/usr/bin/time -f '%e\t%M' -o "$out_dir/logs/link.time" \
  "${link_cmd[@]}" > "$out_dir/logs/link.stdout" 2> "$out_dir/logs/link.stderr"
link_rc=$?
set -e
read -r link_wall_seconds link_max_rss_kib < "$out_dir/logs/link.time"
printf 'link\temu_radiance_gsimconfig\t%s\t%s\t%s\n' \
  "$link_wall_seconds" "$link_max_rss_kib" "$link_rc" >> "$timings"
if [[ "$link_rc" -ne 0 ]]; then
  echo "link failed (see $out_dir/logs/link.stderr)" >&2
  exit "$link_rc"
fi

build_end_ns="$(date +%s%N)"
inputs="$out_dir/build_inputs.sha256"
{
  sha256sum "$model_dir/TestHarness.h" "${model_sources[@]}"
  sha256sum "$harness_dir/main.cpp" "$harness_dir/blackboxes.cpp" "$harness_dir/cvfpu_model.cpp"
  sha256sum "$testchip_csrc/mm.cc" "$testchip_csrc/mm.h" \
    "$testchip_csrc/testchip_tsi.cc" "$testchip_csrc/testchip_tsi.h" \
    "$testchip_csrc/testchip_htif.cc" "$testchip_csrc/testchip_htif.h"
  sha256sum "$fesvr_lib" "$softfloat_root/softfloat.h" "$softfloat_lib" "$cxx" "$workaround" \
    "$helper_snapshot"
  if [[ -n "$firrtl" ]]; then sha256sum "$firrtl"; fi
} > "$inputs"

{
  echo 'schema=gsim.radiance-emulator-build.v1'
  echo 'status=complete'
  echo 'boundary=adopted_preexisting_firrtl_and_emitted_cpp'
  echo 'elaboration_performed=false'
  echo "started_unix_ns=$build_start_ns"
  echo "finished_unix_ns=$build_end_ns"
  echo "wall_seconds=$(elapsed_seconds "$build_start_ns" "$build_end_ns")"
  echo "model_dir=$model_dir"
  echo "invocation_cwd=$invocation_cwd"
  echo "model_cpp_count=${#model_sources[@]}"
  echo "output_binary=$emu"
  echo "output_binary_sha256=$(sha256sum "$emu" | cut -d' ' -f1)"
  echo "compiler=$cxx"
  echo "compiler_sha256=$(sha256sum "$cxx" | cut -d' ' -f1)"
  echo "compiler_version=$($cxx --version | head -n 1)"
  echo "build_helper_snapshot=$helper_snapshot"
  echo "build_helper_sha256=$(sha256sum "$helper_snapshot" | cut -d' ' -f1)"
  echo "gsim_checkout_commit=$(git -C "$here" rev-parse HEAD)"
  if git -C "$here" diff --quiet && git -C "$here" diff --cached --quiet; then
    echo 'gsim_checkout_dirty=false'
  else
    echo 'gsim_checkout_dirty=true'
  fi
  if [[ -n "$firrtl" ]]; then
    echo "adopted_firrtl=$firrtl"
    echo "adopted_firrtl_sha256=$(sha256sum "$firrtl" | cut -d' ' -f1)"
  else
    echo 'adopted_firrtl=unrecorded'
  fi
  echo "commands_sha256=$(sha256sum "$commands_log" | cut -d' ' -f1)"
  echo "timings_sha256=$(sha256sum "$timings" | cut -d' ' -f1)"
  echo "inputs_sha256=$(sha256sum "$inputs" | cut -d' ' -f1)"
  echo "max_compile_rss_kib=$(awk -F '\t' '$1 == "compile" && $4 > max {max=$4} END {print max+0}' "$timings")"
  echo "link_max_rss_kib=$link_max_rss_kib"
  echo "workaround_sha256=$(sha256sum "$workaround" | cut -d' ' -f1)"
  printf 'workaround_symbols='
  printf '%s;' "${leaked_symbols[@]}"
  printf '\n'
} > "$metadata"

python3 - "$metadata" "$timings" "$commands_log" "$inputs" > "$out_dir/build_record.json" <<'PY'
import csv
import json
import shlex
import sys
from pathlib import Path

metadata_path, timings_path, commands_path, inputs_path = map(Path, sys.argv[1:])
metadata = {}
for line in metadata_path.read_text().splitlines():
    key, value = line.split("=", 1)
    metadata[key] = value

with timings_path.open(newline="") as handle:
    timings = list(csv.DictReader(handle, delimiter="\t"))
for row in timings:
    row["wall_seconds"] = float(row["wall_seconds"])
    row["max_rss_kib"] = int(row["max_rss_kib"])
    row["status"] = int(row["status"])

inputs = []
for line in inputs_path.read_text().splitlines():
    digest, path = line.split("  ", 1)
    inputs.append({"sha256": digest, "path": path})

command_records = []
for line in commands_path.read_text().splitlines():
    stage_token, command = line.split(" ", 1)
    command_records.append({
        "stage": stage_token.removeprefix("[").removesuffix("]"),
        "cwd": metadata["invocation_cwd"],
        "argv": shlex.split(command),
    })

record = {
    "schema": metadata["schema"],
    "status": metadata["status"],
    "provenance": {
        "boundary": metadata["boundary"],
        "elaboration_performed": False,
        "adopted_firrtl": metadata["adopted_firrtl"],
        "adopted_firrtl_sha256": metadata.get("adopted_firrtl_sha256"),
        "warning": "This build starts from pre-existing FIRRTL and emitted C++; it does not establish RTL-revision provenance.",
    },
    "model": {
        "directory": metadata["model_dir"],
        "cpp_count": int(metadata["model_cpp_count"]),
    },
    "output": {
        "binary": metadata["output_binary"],
        "sha256": metadata["output_binary_sha256"],
    },
    "toolchain": {
        "compiler": metadata["compiler"],
        "compiler_sha256": metadata["compiler_sha256"],
        "compiler_version": metadata["compiler_version"],
        "gsim_checkout_commit": metadata["gsim_checkout_commit"],
        "gsim_checkout_dirty": metadata["gsim_checkout_dirty"] == "true",
        "build_helper_snapshot": metadata["build_helper_snapshot"],
        "build_helper_sha256": metadata["build_helper_sha256"],
    },
    "wall_seconds": float(metadata["wall_seconds"]),
    "max_compile_rss_kib": int(metadata["max_compile_rss_kib"]),
    "link_max_rss_kib": int(metadata["link_max_rss_kib"]),
    "timings": timings,
    "commands": command_records,
    "inputs": inputs,
    "digests": {
        "commands_sha256": metadata["commands_sha256"],
        "timings_sha256": metadata["timings_sha256"],
        "inputs_sha256": metadata["inputs_sha256"],
        "workaround_sha256": metadata["workaround_sha256"],
    },
    "workaround_symbols": [s for s in metadata["workaround_symbols"].split(";") if s],
}
json.dump(record, sys.stdout, indent=2, sort_keys=True)
sys.stdout.write("\n")
PY

echo "$emu"
