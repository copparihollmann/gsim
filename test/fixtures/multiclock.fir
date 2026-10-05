FIRRTL version 3.3.0
circuit MultiClock :
  extmodule Oscillator :
    input power : UInt<1>
    output clk : Clock
    defname = Oscillator
    parameter PERIOD = 2.0
  extmodule OscillatorSlow :
    input power : UInt<1>
    output clk : Clock
    defname = Oscillator
    parameter PERIOD = 10.0
  extmodule ClockCell :
    input pad : UInt<1>
    output value : UInt<1>
    defname = ClockCell
  extmodule StateModel :
    input clock : Clock
    input reset : UInt<1>
    output count : UInt<16>
    defname = StateModel

  module MultiClock :
    input clock : Clock
    input reset : UInt<1>
    output fast : UInt<16>
    output slow : UInt<16>
    output external : UInt<16>
    inst source of Oscillator
    connect source.power, UInt<1>(0h1)
    inst source_slow of OscillatorSlow
    connect source_slow.power, UInt<1>(0h1)
    inst cell of ClockCell
    connect cell.pad, asUInt(source_slow.clk)
    node slow_clock = asClock(cell.value)
    regreset fast_count : UInt<16>, source.clk, reset, UInt<16>(0h0)
    regreset slow_count : UInt<16>, slow_clock, reset, UInt<16>(0h0)
    connect fast_count, tail(add(fast_count, UInt<1>(0h1)), 1)
    connect slow_count, tail(add(slow_count, UInt<1>(0h1)), 1)
    inst external_state of StateModel
    connect external_state.clock, slow_clock
    connect external_state.reset, reset
    connect fast, fast_count
    connect slow, slow_count
    connect external, external_state.count
    assert(source.clk, UInt<1>(0h1), UInt<1>(0h1), "always satisfied")
    assert(source.clk, UInt<1>(0h0), UInt<1>(0h0), "disabled assertion")
    printf(source.clk, UInt<1>(0h0), "disabled print\n")
