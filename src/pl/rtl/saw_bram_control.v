`timescale 1ns / 1ps

module Saw_Bram_Control #(
    parameter CAPTURE_DEPTH = 1024
)(
    input               bram_clk,
    input       [12:0]  bram_addr,
    input       [31:0]  bram_wrdata,
    input       [3:0]   bram_we,
    input               bram_en,
    input               rst,

    input       [31:0]  capture_rddata,
    input               capture_busy,
    input               capture_done,
    input       [15:0]  capture_count,
    input       [2:0]   capture_state,
    input       [15:0]  coherent_count,
    input       [15:0]  noncoherent_count,
    input       [6:0]   current_freq_index,
    input               force_rx_on,
    input               force_tx_on,
    input       [31:0]  rx_samples_last,
    input               rx_overflow,

    output reg  [31:0]  bram_rddata,
    output reg          saw_start_toggle,
    output reg          saw_stop_toggle,
    output reg  [31:0]  tx_sample_count,
    output reg  [31:0]  guard1_sample_count,
    output reg  [31:0]  rx_sample_count,
    output reg  [31:0]  guard2_sample_count,
    output reg  [15:0]  coherent_repetitions,
    output reg  [15:0]  noncoherent_segments,
    output reg  [7:0]   point_count,
    output reg signed [31:0] start_phase_inc,
    output reg signed [31:0] step_phase_inc,
    output      [9:0]   capture_rdaddr
    );

    localparam CMD_START      = 32'hAA550201;
    localparam CMD_CLEAR_DONE = 32'hAA550202;
    localparam CMD_STOP       = 32'hAA550203;

    localparam REG_ID         = 11'h000;
    localparam REG_CONTROL    = 11'h001;
    localparam REG_STATUS     = 11'h002;
    localparam REG_TX_SAMPLES = 11'h003;
    localparam REG_GUARD      = 11'h004;
    localparam REG_RX_SAMPLES = 11'h005;
    localparam REG_RX_COUNT   = 11'h006;
    localparam REG_POINT_COUNT = 11'h007;
    localparam REG_START_PHASE = 11'h008;
    localparam REG_STEP_PHASE  = 11'h009;
    localparam REG_GUARD2      = 11'h00A;
    localparam REG_RX_CAPTURED = 11'h00B;
    localparam REG_N_COH       = 11'h00C;
    localparam REG_M_NC        = 11'h00D;
    localparam REG_DEBUG       = 11'h00E;
    localparam REG_CURRENT     = 11'h00F;
    localparam REG_DATA_BASE  = 11'h100;
    localparam [7:0] MAX_POINT_COUNT = (CAPTURE_DEPTH > 127) ? 8'd127 : CAPTURE_DEPTH;

    wire [10:0] word_addr = bram_addr[12:2];
    wire        wr_active = bram_en && (bram_we == 4'hF);

    reg capture_done_latched;
    reg capture_done_meta;
    reg capture_done_sync;
    reg capture_done_seen;

    assign capture_rdaddr = word_addr[9:0] - REG_DATA_BASE[9:0];

    always @(posedge bram_clk) begin
        if (rst) begin
            tx_sample_count <= 32'd1230;     // 10 us, aligned to two 122.88 MHz cycles
            guard1_sample_count <= 32'd3072; // 25 us TX-to-RX guard
            rx_sample_count <= 32'd2458;     // 20 us, 1229 RX2 I/Q samples
            guard2_sample_count <= 32'd616;  // 5 us, aligned to a 1R1T frame
            coherent_repetitions <= 16'd64;
            noncoherent_segments <= 16'd16;
            point_count <= 8'd101;         // 912 MHz to 918 MHz, 60 kHz step
            start_phase_inc <= -32'sd209715200; // -3 MHz at 61.44 MSPS
            step_phase_inc <= 32'sd4194304;     // 60 kHz at 61.44 MSPS
            saw_start_toggle <= 1'b0;
            saw_stop_toggle <= 1'b0;
            capture_done_latched <= 1'b0;
            capture_done_meta <= 1'b0;
            capture_done_sync <= 1'b0;
            capture_done_seen <= 1'b0;
        end
        else begin
            capture_done_meta <= capture_done;
            capture_done_sync <= capture_done_meta;
            capture_done_seen <= capture_done_sync;
            if (capture_done_sync && !capture_done_seen) begin
                capture_done_latched <= 1'b1;
            end

            if (wr_active) begin
                case (word_addr)
                    REG_CONTROL: begin
                        if (bram_wrdata == CMD_START) begin
                            saw_start_toggle <= ~saw_start_toggle;
                            capture_done_latched <= 1'b0;
                        end
                        else if (bram_wrdata == CMD_CLEAR_DONE) begin
                            capture_done_latched <= 1'b0;
                        end
                        else if (bram_wrdata == CMD_STOP) begin
                            saw_stop_toggle <= ~saw_stop_toggle;
                            capture_done_latched <= 1'b0;
                        end
                    end
                    REG_TX_SAMPLES: begin
                        tx_sample_count <= bram_wrdata;
                    end
                    REG_GUARD: begin
                        guard1_sample_count <= bram_wrdata;
                    end
                    REG_RX_SAMPLES: begin
                        rx_sample_count <= bram_wrdata;
                    end
                    REG_POINT_COUNT: begin
                        if (bram_wrdata[7:0] == 8'd0) begin
                            point_count <= 8'd1;
                        end
                        else if (bram_wrdata[7:0] > MAX_POINT_COUNT) begin
                            point_count <= MAX_POINT_COUNT;
                        end
                        else begin
                            point_count <= bram_wrdata[7:0];
                        end
                    end
                    REG_START_PHASE: begin
                        start_phase_inc <= bram_wrdata;
                    end
                    REG_STEP_PHASE: begin
                        step_phase_inc <= bram_wrdata;
                    end
                    REG_GUARD2: begin
                        guard2_sample_count <= bram_wrdata;
                    end
                    REG_N_COH: begin
                        coherent_repetitions <= (bram_wrdata[15:0] == 16'd0) ?
                                                16'd1 : bram_wrdata[15:0];
                    end
                    REG_M_NC: begin
                        noncoherent_segments <= (bram_wrdata[15:0] == 16'd0) ?
                                                16'd1 : bram_wrdata[15:0];
                    end
                    default: begin
                    end
                endcase
            end
        end
    end

    always @(posedge bram_clk) begin
        if (rst) begin
            bram_rddata <= 32'd0;
        end
        else begin
            case (word_addr)
                REG_ID:         bram_rddata <= 32'h53415731; // "SAW1"
                REG_CONTROL:    bram_rddata <= 32'd0;
                REG_STATUS:     bram_rddata <= {27'd0, force_tx_on, force_rx_on,
                                                capture_done_latched, capture_busy,
                                                capture_done_sync};
                REG_TX_SAMPLES: bram_rddata <= tx_sample_count;
                REG_GUARD:      bram_rddata <= guard1_sample_count;
                REG_RX_SAMPLES: bram_rddata <= rx_sample_count;
                REG_RX_COUNT:   bram_rddata <= {16'd0, capture_count};
                REG_POINT_COUNT: bram_rddata <= {24'd0, point_count};
                REG_START_PHASE: bram_rddata <= start_phase_inc;
                REG_STEP_PHASE:  bram_rddata <= step_phase_inc;
                REG_GUARD2:      bram_rddata <= guard2_sample_count;
                REG_RX_CAPTURED: bram_rddata <= rx_samples_last;
                REG_N_COH:       bram_rddata <= {16'd0, coherent_repetitions};
                REG_M_NC:        bram_rddata <= {16'd0, noncoherent_segments};
                REG_DEBUG:       bram_rddata <= {capture_state, rx_overflow, 4'd0,
                                                 noncoherent_count[7:0],
                                                 coherent_count[7:0],
                                                 capture_count[7:0]};
                REG_CURRENT:     bram_rddata <= {25'd0, current_freq_index};
                default: begin
                    if ((word_addr >= REG_DATA_BASE) &&
                        (word_addr < (REG_DATA_BASE + CAPTURE_DEPTH))) begin
                        bram_rddata <= capture_rddata;
                    end
                    else begin
                        bram_rddata <= 32'd0;
                    end
                end
            endcase
        end
    end

endmodule
