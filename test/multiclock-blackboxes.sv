`timescale 1ns/1ps
// Independent event-driven definitions for the regression's external models.
module Oscillator #(parameter real PERIOD = 2.0)(input power, output reg clk);
  initial clk = 0;
  always #(PERIOD / 2.0) clk = ~clk & power;
endmodule
module ClockCell(input pad, output value);
  assign value = pad;
endmodule
module StateModel(input clock, input reset, output reg [15:0] count);
  always @(posedge clock) count <= reset ? 0 : count + 1;
endmodule
