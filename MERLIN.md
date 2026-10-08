# Merlin integration

This fork's `merlin` branch publishes the simulator and harness changes used by
Merlin. `master` remains the upstream branch. Pin a full commit for an experiment;
do not assume that two builds from a moving branch are identical.

## Install and check the emitter

Use Linux with Clang 19 or newer, GNU Make, Flex (including `FlexLexer.h`), Bison,
GMP development headers/libraries, and a compatible C++ standard library. The
native Gemmini build has been exercised with Clang 23 on x86-64 Linux. No GPU or
FPGA is needed. An x86-64 binary does not run natively on a Graviton/ARM worker.

```sh
git clone --branch merlin https://github.com/copparihollmann/gsim.git
cd gsim
export GSIM_CLANGXX=/absolute/toolchain/bin/clang++
make -j2 CXX=./cxxwrap_portable.sh build-gsim
make CXX=./cxxwrap_portable.sh ext-clock-output-check dynamic-clock-check
"$GSIM_CLANGXX" -std=c++17 chipyard_harness/terminal_dump_selftest.cpp \
  -o build/terminal-dump-selftest
build/terminal-dump-selftest
```

The portable wrapper also accepts `MERLIN_CLANG`, including a versioned `clang`
executable, and explicitly selects the C++ driver. Override
`GSIM_GCC_INSTALL_DIR` when using a nonstandard GCC/libstdc++ installation.
`cxxwrap.sh` is a historical, host-specific receipt input: do not use it for a
new worker or edit it to make an old receipt appear valid.

## Build the selected Chipyard model

The emitter is not an accelerator simulator by itself. Supply the complete
selected `TestHarness` FIRRTL and its matching Chipyard support checkout. The
native harness currently supports one 32-bit-address/64-bit-data SimDRAM,
TestChipIP TSI and UART; other interfaces require separate qualification.

Chipyard must contain initialized TestChipIP and the selected FESVR installation:

```text
generators/testchipip/src/main/resources/testchipip/csrc/
.conda-env/riscv-tools/include/fesvr/
.conda-env/riscv-tools/lib/libfesvr.a
```

To write a Merlin receipt, install Merlin core and `packages/merlin-experiments`
in the Python environment used below. Keep the selected source/tool versions and
all receipt-referenced inputs with the resulting binary.

```sh
python chipyard_harness/build.py \
  --firrtl /absolute/selected/TestHarness.fir \
  --emitter "$PWD/build/gsim/gsim" \
  --compiler "$GSIM_CLANGXX" \
  --chipyard /absolute/selected/chipyard \
  --out /absolute/fresh/out/build/rtl_engines/native-gsim \
  --comb-extmod EICG_wrapper --optimization 2 --jobs 2 --merlin-receipt
```

This command adopts existing FIRRTL; it does **not** prove which RTL revision
elaborated it. Retain the elaboration recipe, hardware revisions and generated
ABI header separately. The selected Phase 0 facts must have the same FIRRTL
digest as the simulator receipt. A new build produces a new receipt; do not
rewrite historical receipt paths or hashes.

See [the native harness contract](chipyard_harness/README.md) for loading,
completion and coherent readback. `+loadmem` can produce different cache warming
than serial ELF loading; match loading/warmup policies when comparing timing.

## Connect Merlin

Select the exact binary and facts through Merlin's process configuration:

```sh
export MERLIN_EXT_GSIM=/absolute/gsim
export MERLIN_CHIPYARD=/absolute/selected/chipyard
export MERLIN_EXT_CHIPYARD="$MERLIN_CHIPYARD"
export MERLIN_GSIM_EMU_GEMMINI=/absolute/fresh/out/build/rtl_engines/native-gsim/native/emulator
export MERLIN_REQUIRED_RTL_ENGINE=gsim
export MERLIN_GSIM_REQUIRE_RECEIPT=1
export MERLIN_RTL_FACTS=/absolute/selected/facts.json
```

Use the matching target support provider, guest RISC-V toolchain and LLVM/MLIR
installation. For EL4, also install `merlin-experiments`, configure the author
driver's authentication, and qualify Bubblewrap user/network namespace isolation.
Chia workers additionally need the managed-worker cleanup guarantees required by
Merlin. Authentication and private validation answers never belong in Git.

## What cloning does not supply

Generated FIRRTL, model C++, simulator binaries, toolchain installations, model
weights/captures, Phase 0 releases and experiment runs are not source files.
Transfer their complete, byte-verified input closures or regenerate and review
them. Relocation requires fresh host admission/isolation checks; do not assume
an existing frozen run can resume merely because its files were copied.

The small checks above cover clock and transport regressions, not full ISA or
hardware equivalence. Before certification, run a nonzero numerical smoke and
check every output, accelerator activity, normal completion and source identity.
Older Radiance helpers are configuration-specific diagnostics, not the native
Gemmini certification path. Their cache-overlay readback and fixed emitted-file
assumptions must not be generalized into hardware-correctness claims.
