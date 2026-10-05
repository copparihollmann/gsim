FIRRTL version 3.3.0
circuit ExternalClockOutput :
  extmodule ClockProducer :
    input enable : UInt<1>
    output tick : Clock
    defname = ClockProducer

  extmodule ClockConsumer :
    input value : UInt<1>
    output result : UInt<1>
    defname = ClockConsumer

  module ExternalClockOutput :
    input clock : Clock
    input reset : UInt<1>
    output out : UInt<1>

    inst producer of ClockProducer
    connect producer.enable, UInt<1>(0h1)
    inst consumer of ClockConsumer
    connect consumer.value, asUInt(producer.tick)
    connect out, consumer.result
