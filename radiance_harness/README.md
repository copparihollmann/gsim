# Radiance / Chipyard boot harness for GSIM-generated C++

GSIM emits a `TestHarness.h` + C++ sources for an elaborated Chipyard SoC, but no top-level driver
and no models for the clocked blackboxes the SoC instantiates. This directory supplies both, so a
GSIM-compiled `RadianceGsimConfig` (or an Atlas/Gemmini SoC) can boot an ELF and run to completion.

| file | what it is |
|---|---|
| `main.cpp` | top-level loop: drives clock/reset (one `step()` == one clock), holds reset, runs until fesvr reports HTIF `done()` or the `+max-cycles` cap is hit. Takes `<soc.elf> [+plusargs]`. |
| `blackboxes.cpp` | C++ models for the clocked extmodules (SimDRAM backing store + `dram_peek`, SimTSI/fesvr, SimUART, SimJTAG). These are the blackboxes whose false combinational loops the compiler patch severs. |
| `cvfpu_model.cpp` | CVFPU model. Pipeline latency is set by the `CVFPU_LAT` environment variable, which is what `../latsweep.sh` sweeps. |

## Building

1. Build the patched GSIM (`../cxxwrap.sh` is the compiler wrapper; see its header for the env vars
   that point it at a clang++).
2. Emit C++ for the design: `../reemit.sh <config>.fir <workdir>` — the `.fir` comes from a Chipyard
   elaboration under `$MERLIN_CHIPYARD/sims/verilator/generated-src/<config>/`.
3. Compile the emitted sources together with the three files here into `emu`.
4. Run: `./emu <kernel.soc.elf> +loadmem=<kernel.soc.elf> +max-cycles=<N>`.

## Note on `.flexinc/`

The build also wants a `.flexinc/` directory holding `FlexLexer.h` and `gmp.h`. It is deliberately
not tracked: on this kind of box it is two symlinks into `/usr/include`, which is a property of the
machine, not of the source. Recreate it with:

```sh
mkdir -p .flexinc
ln -sf /usr/include/FlexLexer.h .flexinc/FlexLexer.h
ln -sf "$(printf '%s' /usr/include/*-linux-gnu/gmp.h)" .flexinc/gmp.h
```
