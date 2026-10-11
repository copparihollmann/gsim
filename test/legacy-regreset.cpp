#include <cstdio>
#include "LegacyRegReset.h"

int main() {
  SLegacyRegReset dut;
  dut.set_reset(1);
  dut.set_with(0);
  dut.set_print_enable(0);
  dut.step();
  // Both syntax forms must preserve identical sequential semantics when
  // reset is asserted, deasserted, or toggled among nonzero data values.
  for (unsigned cycle = 0; cycle < 32; ++cycle) {
    dut.set_reset(cycle % 5 == 0);
    dut.set_with((cycle * 37 + 11) & 255);
    dut.set_print_enable(cycle == 31);
    dut.step();
    if (dut.get_legacy_value() != dut.get_modern_value() ||
        dut.get_inline_value() != dut.get_modern_value() ||
        dut.get_no_reset_value() != dut.get_plain_value() ||
        dut.get_legacy_literal() != 255 ||
        dut.get_binary_literal() != 42 || dut.get_octal_literal() != 42 ||
        dut.get_decimal_literal() != 42 ||
        static_cast<int>(static_cast<signed _BitInt(8)>(dut.get_signed_literal())) != -2) {
      std::fprintf(stderr, "legacy reset/literal mismatch at cycle %u\n", cycle);
      return 1;
    }
  }
  std::puts("legacy reset and quoted literal: PASS");
  return 0;
}
