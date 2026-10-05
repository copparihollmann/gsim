# Test Inputs

- Any `*.fir` file in this directory is auto-discovered by `make fir-tests` and by the GitHub CI `fir-regression` job.
- repro-usefulreset.fir: Minimized FIR reproducer for GSIM issue #106, used to guard against ConstantAnalysis hangs and OOM regressions.
- ext-clock-output.fir: Clock-valued blackbox outputs must stay in the call and
  dependency graph. `make ext-clock-output-check` compiles and executes the
  generated model to check that an output cast to data changes every step.
  It also checks that an opaque generated clock driving a register is rejected
  explicitly, rather than silently converted to a constant clock.
- `make dynamic-clock-check` compiles and executes the opt-in level-clock mode:
  independent 2 ns/10 ns domains, a clock passing through a data-valued I/O cell,
  synchronous resets, registered external-model output publication, and disabled
  side effects. The default legacy mode still rejects opaque generated clocks.
- `multiclock-blackboxes.sv` supplies independent event-driven models for a
  CIRCT/Verilator differential check. Generate Verilog from
  `fixtures/multiclock.fir`, then compile `multiclock-main.cpp` with
  `GSIM_VERILATOR_REFERENCE`, the generated GSIM model and Verilator's timed
  model. The check compares post-edge state over 120 samples. Verilator timing
  support requires C++20; both its compile and link must use the selected Clang
  toolchain. This finite check is not a proof for all clock networks.
