# Test Inputs

- Any `*.fir` file in this directory is auto-discovered by `make fir-tests` and by the GitHub CI `fir-regression` job.
- repro-usefulreset.fir: Minimized FIR reproducer for GSIM issue #106, used to guard against ConstantAnalysis hangs and OOM regressions.
- ext-clock-output.fir: Clock-valued blackbox outputs must stay in the call and
  dependency graph. `make ext-clock-output-check` compiles and executes the
  generated model to check that an output cast to data changes every step.
  It also checks that an opaque generated clock driving a register is rejected
  explicitly, rather than silently converted to a constant clock.
