FIRRTL version 3.3.0
circuit LegacyKeywordIdentifiers :
  module LegacyKeywordIdentifiers :
    input clock : Clock
    input reset : UInt<1>
    input seed : { data : UInt<8> }
    output result : UInt<8>
    wire with : { data : UInt<8> }
    reg held : UInt<8>, clock
    with <- seed
    connect held, mux(reset, UInt<8>(0), with.data)
    connect result, held
