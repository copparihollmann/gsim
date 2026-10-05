#include "ExternalClockOutput.h"

void ClockProducer(uint8_t enable, uint8_t& tick) {
  static uint8_t level = 0;
  level ^= 1;
  tick = level & enable;
}

void ClockConsumer(uint8_t value, uint8_t& result) {
  result = value;
}

int main() {
  SExternalClockOutput dut;
  dut.set_reset(0);
  for (int step = 0; step < 8; ++step) {
    dut.step();
    if (dut.get_out() != ((step + 1) & 1)) return 1;
  }
  return 0;
}
