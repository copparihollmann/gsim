FIRRTL version 3.3.0
circuit LegacyRegReset :
  module LegacyRegReset :
    input clock : Clock
    input reset : UInt<1>
    input with : UInt<8>
    input print_enable : UInt<1>
    output legacy_value : UInt<8>
    output modern_value : UInt<8>
    output inline_value : UInt<8>
    output no_reset_value : UInt<8>
    output plain_value : UInt<8>
    output legacy_literal : UInt<8>
    output binary_literal : UInt<8>
    output octal_literal : UInt<8>
    output decimal_literal : UInt<8>
    output signed_literal : SInt<8>

    reg legacy : UInt<8>, clock with :
      reset => (reset, UInt<8>("h2a"))
    regreset modern : UInt<8>, clock, reset, UInt<8>(0h2a)
    reg inline_reset : UInt<8>, clock with : (reset => (reset, UInt<8>("d42")))
    reg no_reset : UInt<8>, clock with :
      reset => (UInt<1>("h0"), no_reset)
    reg plain : UInt<8>, clock
    legacy <= with
    connect modern, with
    connect inline_reset, with
    connect no_reset, with
    connect plain, with
    legacy_value <= legacy
    connect modern_value, modern
    connect inline_value, inline_reset
    connect no_reset_value, no_reset
    connect plain_value, plain
    connect legacy_literal, UInt<8>("hFF")
    connect binary_literal, UInt<8>("b101010")
    connect octal_literal, UInt<8>("o52")
    connect decimal_literal, UInt<8>("d42")
    connect signed_literal, SInt<8>("h-2")
    printf(clock, print_enable, "h2a")
