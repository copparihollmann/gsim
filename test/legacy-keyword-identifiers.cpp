#include <cstdio>
#include "LegacyKeywordIdentifiers.h"

int main() {
  SLegacyKeywordIdentifiers dut;
  dut.set_reset(0);
  dut.set_seed$$data(37);
  dut.step();
  dut.step();
  if (dut.get_result() != 37) return 1;
  dut.set_reset(1);
  dut.step();
  dut.step();
  if (dut.get_result() != 0) return 1;
  std::puts("legacy keyword identifiers: PASS");
  return 0;
}
