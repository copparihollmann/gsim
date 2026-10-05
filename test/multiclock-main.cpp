#include "MultiClock.h"
#include <cstdio>
#ifdef GSIM_VERILATOR_REFERENCE
#include "VMultiClock.h"
#include "verilated.h"
#endif

static uint64_t sample_time = 0;
void Oscillator(double period, uint8_t power, uint8_t& clk) {
  clk = power && (static_cast<uint64_t>(sample_time / (period / 2)) & 1);
}
void ClockCell(uint8_t pad, uint8_t& value) { value = pad; }
void StateModel(uint8_t reset, uint16_t& count) {
  count = reset ? 0 : count + 1;
}

int main() {
  SMultiClock dut;
#ifdef GSIM_VERILATOR_REFERENCE
  VerilatedContext context;
  VMultiClock reference{&context};
  auto checkReference = [&](uint8_t reset) {
    reference.reset = reset;
    reference.clock = sample_time & 1;
    reference.eval();
    bool same = dut.fast_count$NEXT == reference.fast &&
                dut.slow_count$NEXT == reference.slow &&
                dut.external_state$count$__gsim_next_sample == reference.external;
    if (!same) fprintf(stderr, "Verilator disagreement at sample %llu: fast=%u/%u slow=%u/%u external=%u/%u\n",
                       static_cast<unsigned long long>(sample_time),
                       dut.fast_count$NEXT, reference.fast,
                       dut.slow_count$NEXT, reference.slow,
                       dut.external_state$count$__gsim_next_sample, reference.external);
    context.timeInc(1000); // reference timeprecision is picoseconds
    return same;
  };
#endif
  // Hold synchronous reset long enough to include an edge in both domains.
  dut.set_reset(1);
  for (sample_time = 0; sample_time < 20; ++sample_time) {
    dut.step();
#ifdef GSIM_VERILATOR_REFERENCE
    if (!checkReference(1)) return 1;
#endif
  }
  dut.set_reset(0);
  uint16_t fast = 0, slow = 0;
  for (; sample_time < 120; ++sample_time) {
    uint16_t published_external = slow;
    if (sample_time % 2 == 1) ++fast;
    if (sample_time % 10 == 5) ++slow;
    dut.step();
#ifdef GSIM_VERILATOR_REFERENCE
    if (!checkReference(0)) return 1;
#endif
    // GSIM computes next-state in step(); the source register is published at
    // the beginning of the following sample. Inspect that actual next-state.
    if (dut.fast_count$NEXT != fast || dut.slow_count$NEXT != slow ||
        dut.external_state$count != published_external) {
      fprintf(stderr, "sample=%llu fast=%u/%u slow=%u/%u ext=%u\n",
              static_cast<unsigned long long>(sample_time), dut.fast_count$NEXT,
              fast, dut.slow_count$NEXT, slow, dut.external_state$count);
      return 1;
    }
  }
  return 0;
}
