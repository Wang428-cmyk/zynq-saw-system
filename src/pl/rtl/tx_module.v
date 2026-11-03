`timescale 1ns / 1ps

module Tx_Module(
    input           clk_122_88M,
    input           clk_122_88M_invert,
    input           clk_61_44M,
    input           rst,
    input           saw_tx_active,
    input [6:0]     saw_freq_index,
    input signed [31:0] saw_start_phase_inc,
    input signed [31:0] saw_step_phase_inc,
    input [20:0]    pcm_rate,
    input [23:0]    pcm_period,
    input [15:0]    modu_coff,
    output          tx_clk_p,
    output          tx_clk_n,
    output          tx_frame_p,
    output          tx_frame_n,
    output  [5:0]   tx_data_p,
    output  [5:0]   tx_data_n
    );

    wire [11:0] data_I;
    wire [11:0] data_Q;
    reg serializer_phase;
    reg tx_frame;
    reg [5:0] tx_word_i;
    reg [5:0] tx_word_q;
    wire [5:0] tx_DDR_out;

    OBUFDS #(
        .IOSTANDARD("LVDS"),
        .SLEW("SLOW")
    ) OBUFDS_CLK (
        .O(tx_clk_p),
        .OB(tx_clk_n),
        .I(clk_122_88M_invert)
    );

    saw_chirp_baseband saw_chirp_baseband_inst0(
        .clk_fs(clk_61_44M),
        .rst(rst),
        .active(saw_tx_active),
        .freq_index(saw_freq_index),
        .start_phase_inc(saw_start_phase_inc),
        .step_phase_inc(saw_step_phase_inc),
        .sin(data_Q),
        .cos(data_I)
    );

    // 1R1T LVDS framing for the AD9361-selected TX2 channel.
    // Rising/falling DDR edges carry I/Q; two clock cycles carry one sample.
    always @(posedge clk_122_88M) begin
        if (rst) begin
            serializer_phase <= 1'b0;
            tx_frame <= 1'b1;
            tx_word_i <= 6'd0;
            tx_word_q <= 6'd0;
        end
        else begin
            serializer_phase <= ~serializer_phase;
            if (!serializer_phase) begin
                tx_frame <= 1'b1;
                tx_word_i <= saw_tx_active ? data_I[11:6] : 6'd0;
                tx_word_q <= saw_tx_active ? data_Q[11:6] : 6'd0;
            end
            else begin
                tx_frame <= 1'b0;
                tx_word_i <= saw_tx_active ? data_I[5:0] : 6'd0;
                tx_word_q <= saw_tx_active ? data_Q[5:0] : 6'd0;
            end
        end
    end

    OBUFDS #(
        .IOSTANDARD("LVDS"),
        .SLEW("SLOW")
    ) OBUFDS_Frame (
        .O(tx_frame_p),
        .OB(tx_frame_n),
        .I(tx_frame)
    );

    genvar i;
    generate for(i = 0; i < 6; i = i + 1) begin: ODDR_DATA
        ODDR #(
            .DDR_CLK_EDGE("SAME_EDGE"),
            .INIT(1'b0),
            .SRTYPE("SYNC")
        ) ODDR_inst (
            .Q(tx_DDR_out[i]),
            .C(clk_122_88M),
            .CE(1'b1),
            .D1(tx_word_i[i]),
            .D2(tx_word_q[i]),
            .R(rst),
            .S(1'b0)
        );

        OBUFDS #(
            .IOSTANDARD("LVDS"),
            .SLEW("SLOW")
        ) OBUFDS_Data (
            .O(tx_data_p[i]),
            .OB(tx_data_n[i]),
            .I(tx_DDR_out[i])
        );
    end
    endgenerate

endmodule
