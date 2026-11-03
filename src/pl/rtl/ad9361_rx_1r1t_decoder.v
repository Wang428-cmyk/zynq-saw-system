`timescale 1ns / 1ps

// AD9361 LVDS DDR 1R1T receive ordering for the selected RX channel:
//   I_MSB/Q_MSB while RX_FRAME is high,
//   I_LSB/Q_LSB while RX_FRAME is low.
module Ad9361_Rx_1R1T_Decoder(
    input                   clk,
    input                   rst,
    input                   enable,
    input                   frame_rise,
    input                   frame_fall,
    input       [5:0]       data_rise,
    input       [5:0]       data_fall,
    output reg signed [11:0] rx_i,
    output reg signed [11:0] rx_q,
    output reg              rx_valid,
    output reg              frame_error,
    output                  have_msb_debug
    );

    reg have_msb;
    assign have_msb_debug = have_msb;
    reg [5:0] i_msb;
    reg [5:0] q_msb;

    always @(posedge clk) begin
        if (rst || !enable) begin
            have_msb <= 1'b0;
            i_msb <= 6'd0;
            q_msb <= 6'd0;
            rx_i <= 12'sd0;
            rx_q <= 12'sd0;
            rx_valid <= 1'b0;
            frame_error <= 1'b0;
        end
        else begin
            rx_valid <= 1'b0;
            frame_error <= 1'b0;

            if (frame_rise != frame_fall) begin
                frame_error <= 1'b1;
                have_msb <= 1'b0;
            end
            else if (frame_rise) begin
                i_msb <= data_rise;
                q_msb <= data_fall;
                have_msb <= 1'b1;
            end
            else if (have_msb) begin
                rx_i <= $signed({i_msb, data_rise});
                rx_q <= $signed({q_msb, data_fall});
                rx_valid <= 1'b1;
                have_msb <= 1'b0;
            end
        end
    end

endmodule
