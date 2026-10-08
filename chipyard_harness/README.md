# Chipyard native harness

This harness executes the unchanged `TestHarness` FIRRTL circuit with
`--dynamic-clocks`. It does not re-root the circuit, change reset behavior,
replace the accelerator, or patch generated C++.

```sh
python chipyard_harness/build.py \
  --firrtl /selected/chipyard/TestHarness.fir \
  --emitter build/gsim/gsim --compiler /selected/bin/clang++ \
  --chipyard /selected/chipyard --out /generated/gsim-build \
  --comb-extmod EICG_wrapper --jobs 4 --merlin-receipt
```

`--merlin-receipt` requires an installed `merlin-experiments` package. Without
it the native build and exact command transcript remain usable independently.
Builds require fresh output directories and preserve failed logs. The receipt
pins adopted FIRRTL, all emitted model files, the emitter, compiler, harness,
upstream C++ support and FESVR library. It explicitly does **not** claim that this
build elaborated the RTL. Keep these pinned source/model files with the binary.

Clock periods come from the original FIRRTL real parameters. The sample cadence
is the greatest common divisor of declared clock half-periods; reported harness
cycles use the fastest declared free-running clock. Synchronous registers,
memory writers and side effects advance only on their own rising edge. Clocked
external outputs publish separately, preserving same-edge sampling.
`--comb-extmod` explicitly identifies clock-valued combinational models such as
the upstream low-phase enable latch, rather than treating every clock port as
a sequential model.

The current boundary supports one 32-bit-address/64-bit-data SimDRAM using
upstream `mm_magic_t`, one native TestChipIP TSI and one UART. The selected
GemminiRocketConfig uses these interfaces. Interactive JTAG is rejected instead
of starting a network listener. Other parameterizations and four-state/tristate
behavior need separate adapters and qualification; this is not a universal
blackbox library. Do not request DRAMSim memory timing with this harness.

```sh
/generated/gsim-build/native/emulator kernel.elf +max-cycles=20000000
/generated/gsim-build/native/emulator kernel.elf \
  +loadmem=kernel.elf +max-cycles=20000000
```

Both paths use native TSI/HTIF and the RTL's boot sequence and completion. The
second uses a faster backdoor ELF load, **not a cache-state-equivalent load**:
serial loading can warm caches through the interconnect. Compare kernel cycles
only with matched loading/warmup policies. Functional agreement and matched
timing agreement are separate observations, not consequences of successful
compilation or byte-bound build provenance.

An opt-in terminal dump can read declared DRAM regions through the live
TSI/TileLink host port after a normal guest exit. Pass absolute paths as
`+dump-regions=/path/regions.txt +dump-out=/path/dump.bin`, optionally with
`+dump-mode=coherent`. The manifest contains bounded `address bytes` rows.
The emulator writes a complete `GSIMDMP1` coherent-source frame and
`GSIMEND1` trailer to a private partial file, then publishes the final file
atomically. Missing, overlapping, out-of-DRAM, interrupted, or failed reads
do not yield a passing dump. This does not read the potentially stale DRAM
backing store, and a consumer must still bind the manifest, ELF, engine
receipt, console, and every requested output byte before claiming a result.
