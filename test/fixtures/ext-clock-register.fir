FIRRTL version 3.3.0
circuit ExternalClockRegister :
  extmodule ClockProducer :
    input enable : UInt<1>
    output tick : Clock
    defname = ClockProducer

  module ExternalClockRegister :
    input clock : Clock
    input reset : UInt<1>
    output out : UInt<8>

    inst producer of ClockProducer
    connect producer.enable, UInt<1>(0h1)
    regreset counter : UInt<8>, producer.tick, reset, UInt<8>(0h0)
    connect counter, tail(add(counter, UInt<1>(0h1)), 1)
    connect out, counter
