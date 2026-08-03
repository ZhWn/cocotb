// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause
// Minimal design exercising the cocotb IPC layer end to end.
`timescale 1ns/1ps
module ipc_smoke (
    input  wire        clk,
    input  wire        rst,
    input  wire [7:0]  din,
    output reg  [7:0]  dout
);
    always @(posedge clk or posedge rst) begin
        if (rst) begin
            dout <= 8'h00;
        end else begin
            dout <= din;
        end
    end
endmodule