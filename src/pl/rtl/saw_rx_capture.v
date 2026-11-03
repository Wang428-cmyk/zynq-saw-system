`timescale 1ns / 1ps

module Saw_Rx_Capture #(
    parameter RX_BUFFER_DEPTH = 65536
)(
    input               rx_clk,
    input               rx_frame_p,
    input               rx_frame_n,
    input       [5:0]   rx_data_p,
    input       [5:0]   rx_data_n,

    input               rst,
    input               start_toggle,
    input               stop_toggle,
    input       [31:0]  tx_sample_count,
    input       [31:0]  guard1_sample_count,
    input       [31:0]  rx_sample_count,
    input       [31:0]  guard2_sample_count,
    input       [15:0]  coherent_repetitions,
    input       [15:0]  noncoherent_segments,
    input       [7:0]   point_count,
    input signed [31:0] start_phase_inc,
    input signed [31:0] step_phase_inc,

    input               bram_clk,
    input       [9:0]   bram_rdaddr,
    output      [31:0]  bram_rddata,

    output reg          tx_active,
    output              rx_capture_en,
    output              force_rx_on,
    output              force_tx_on,
    output reg          busy,
    output reg          done,
    output reg  [15:0]  capture_count,
    output reg  [15:0]  coherent_count,
    output reg  [15:0]  noncoherent_count,
    output reg  [31:0]  rx_samples_last,
    output reg          rx_overflow,
    output      [2:0]   state_debug,
    output reg  [6:0]   freq_index
    );

    localparam ST_IDLE     = 3'd0;
    localparam ST_TX       = 3'd1;
    localparam ST_GUARD1   = 3'd2;
    localparam ST_RX       = 3'd3;
    localparam ST_RX_FLUSH = 3'd4;
    localparam ST_GUARD2   = 3'd5;
    localparam ST_POWER    = 3'd6;

    localparam SAMPLE_ADDR_WIDTH = $clog2(RX_BUFFER_DEPTH);
    localparam SAMPLE_COUNT_WIDTH = $clog2(RX_BUFFER_DEPTH + 1);

    wire rx_frame;
    wire [5:0] rx_data_s;
    wire [5:0] rx_data_rise;
    wire [5:0] rx_data_fall;
    wire rx_frame_rise;
    wire rx_frame_fall;

    wire signed [11:0] rx_i;
    wire signed [11:0] rx_q;
    wire rx_valid;
    wire decoder_frame_error;

    // Continuous complex DC remover. The estimate runs for every valid RX
    // sample and is intentionally not reset between scan points.
    localparam integer RX_DC_TRACK_SHIFT = 9;
    localparam integer RX_DC_EST_WIDTH = 24;
    reg signed [RX_DC_EST_WIDTH-1:0] rx_dc_i_est;
    reg signed [RX_DC_EST_WIDTH-1:0] rx_dc_q_est;
    wire signed [RX_DC_EST_WIDTH-1:0] rx_i_dc_target =
        $signed({{(RX_DC_EST_WIDTH-12-RX_DC_TRACK_SHIFT){rx_i[11]}},
                  rx_i, {RX_DC_TRACK_SHIFT{1'b0}}});
    wire signed [RX_DC_EST_WIDTH-1:0] rx_q_dc_target =
        $signed({{(RX_DC_EST_WIDTH-12-RX_DC_TRACK_SHIFT){rx_q[11]}},
                  rx_q, {RX_DC_TRACK_SHIFT{1'b0}}});
    wire signed [RX_DC_EST_WIDTH-1:0] rx_i_dc_delta =
        rx_i_dc_target - rx_dc_i_est;
    wire signed [RX_DC_EST_WIDTH-1:0] rx_q_dc_delta =
        rx_q_dc_target - rx_dc_q_est;
    wire signed [12:0] rx_i_dc_est_code =
        $signed(rx_dc_i_est >>> RX_DC_TRACK_SHIFT);
    wire signed [12:0] rx_q_dc_est_code =
        $signed(rx_dc_q_est >>> RX_DC_TRACK_SHIFT);
    wire signed [12:0] rx_i_dc_removed =
        $signed({rx_i[11], rx_i}) - $signed(rx_i_dc_est_code);
    wire signed [12:0] rx_q_dc_removed =
        $signed({rx_q[11], rx_q}) - $signed(rx_q_dc_est_code);

    reg [2:0] state;
    reg [31:0] state_count;
    reg advance_frequency;

    reg [SAMPLE_COUNT_WIDTH-1:0] sample_index;
    reg [SAMPLE_COUNT_WIDTH-1:0] processed_sample_count;
    reg [31:0] first_i;
    reg [31:0] first_q;
    reg point_overflow;
    reg point_frame_error;
    reg [63:0] frequency_energy;

    // Per-frequency complex detector. The target offset is generated from the
    // scan start and step, so every configured frequency uses the same DDC.
    reg [31:0] nco_phase_acc;
    wire signed [38:0] indexed_phase_product =
        $signed({1'b0, freq_index}) * $signed(step_phase_inc);
    wire signed [31:0] nco_phase_inc =
        $signed(start_phase_inc) + $signed(indexed_phase_product[31:0]);
    // CORDIC "Scaled Radians" uses an 18-bit signed Q3.15 phase word:
    // +1.0 represents +pi and -1.0 represents -pi. Sign-extend the
    // upper 16 accumulator bits; using [31:14] scales phase by four and
    // creates strong 2x/4x NCO harmonics.
    wire signed [17:0] nco_phase_word = {
        {2{nco_phase_acc[31]}},
        nco_phase_acc[31:16]
    };
    wire [23:0] nco_phase_tdata = {{6{nco_phase_word[17]}}, nco_phase_word};
    wire nco_input_valid = (state == ST_RX) && rx_valid &&
                           (sample_index < RX_BUFFER_DEPTH);
    wire nco_output_valid;
    wire [31:0] nco_sin_cos;
    wire signed [11:0] nco_sin = nco_sin_cos[27:16];
    wire signed [11:0] nco_cos = nco_sin_cos[11:0];

    // The CORDIC is pipelined. This FIFO aligns I/Q with its sine/cosine word
    // without depending on a device-specific fixed pipeline latency.
    reg signed [12:0] sample_i_fifo [0:63];
    reg signed [12:0] sample_q_fifo [0:63];
    reg [5:0] sample_fifo_wrptr;
    reg [5:0] sample_fifo_rdptr;
    reg mul_valid;
    reg signed [24:0] product_i_cos;
    reg signed [24:0] product_q_sin;
    reg signed [24:0] product_q_cos;
    reg signed [24:0] product_i_sin;
    reg mix_valid;
    reg signed [25:0] mixed_i;
    reg signed [25:0] mixed_q;
    reg signed [47:0] ddc_sum_i;
    reg signed [47:0] ddc_sum_q;
    wire signed [47:0] ddc_sum_i_scaled_full = ddc_sum_i >>> 11;
    wire signed [47:0] ddc_sum_q_scaled_full = ddc_sum_q >>> 11;
    wire signed [31:0] ddc_sum_i_scaled = ddc_sum_i_scaled_full[31:0];
    wire signed [31:0] ddc_sum_q_scaled = ddc_sum_q_scaled_full[31:0];
    reg signed [63:0] energy_i_square;
    reg signed [63:0] energy_q_square;

    reg [191:0] capture_mem [0:127];
    reg [31:0] bram_rddata_r;
    reg start_meta;
    reg start_sync;
    reg start_seen;
    reg stop_meta;
    reg stop_sync;
    reg stop_seen;

    wire [64:0] block_energy_full = {1'b0, energy_i_square} +
                                    {1'b0, energy_q_square};
    wire [63:0] block_energy = block_energy_full[64] ?
                               64'hFFFFFFFFFFFFFFFF : block_energy_full[63:0];
    wire [64:0] frequency_energy_full = {1'b0, frequency_energy} +
                                        {1'b0, block_energy};
    wire [63:0] frequency_energy_next = frequency_energy_full[64] ?
                                        64'hFFFFFFFFFFFFFFFF : frequency_energy_full[63:0];
    wire coherent_block_complete =
        ((coherent_count + 16'd1) >= coherent_repetitions);
    wire point_complete = coherent_block_complete &&
        ((noncoherent_count + 16'd1) >= noncoherent_segments);

    assign bram_rddata = bram_rddata_r;
    assign rx_capture_en = (state == ST_RX);
   // assign force_rx_on = (state == ST_GUARD1) || (state == ST_RX);
    assign force_rx_on = ~rst; 
    assign force_tx_on = tx_active;
    //assign force_tx_on = 1'b0;
    assign state_debug = state;

    IBUFDS #(
        .DIFF_TERM("TRUE"),
        .IOSTANDARD("LVDS")
    ) IBUFDS_RX_FRAME (
        .I(rx_frame_p),
        .IB(rx_frame_n),
        .O(rx_frame)
    );

    genvar i;
    generate for (i = 0; i < 6; i = i + 1) begin: RX_LVDS
        IBUFDS #(
            .DIFF_TERM("TRUE"),
            .IOSTANDARD("LVDS")
        ) IBUFDS_RX_DATA (
            .I(rx_data_p[i]),
            .IB(rx_data_n[i]),
            .O(rx_data_s[i])
        );

        IDDR #(
            .DDR_CLK_EDGE("SAME_EDGE_PIPELINED"),
            .INIT_Q1(1'b0),
            .INIT_Q2(1'b0),
            .SRTYPE("SYNC")
        ) IDDR_RX_DATA (
            .Q1(rx_data_rise[i]),
            .Q2(rx_data_fall[i]),
            .C(rx_clk),
            .CE(1'b1),
            .D(rx_data_s[i]),
            .R(rst),
            .S(1'b0)
        );
    end
    endgenerate

    IDDR #(
        .DDR_CLK_EDGE("SAME_EDGE_PIPELINED"),
        .INIT_Q1(1'b0),
        .INIT_Q2(1'b0),
        .SRTYPE("SYNC")
    ) IDDR_RX_FRAME (
        .Q1(rx_frame_rise),
        .Q2(rx_frame_fall),
        .C(rx_clk),
        .CE(1'b1),
        .D(rx_frame),
        .R(rst),
        .S(1'b0)
    );
    wire decoder_have_msb;

    Ad9361_Rx_1R1T_Decoder decoder (
        .clk(rx_clk),
        .rst(rst),
        .enable(force_rx_on),
        .frame_rise(rx_frame_rise),
        .frame_fall(rx_frame_fall),
        .data_rise(rx_data_rise),
        .data_fall(rx_data_fall),
        .rx_i(rx_i),
        .rx_q(rx_q),
        .rx_valid(rx_valid),
        .frame_error(decoder_frame_error),
        .have_msb_debug(decoder_have_msb)
    );

    cos_ph rx_frequency_nco (
        .aclk(rx_clk),
        .s_axis_phase_tvalid(nco_input_valid),
        .s_axis_phase_tdata(nco_phase_tdata),
        .m_axis_dout_tvalid(nco_output_valid),
        .m_axis_dout_tdata(nco_sin_cos)
    );

    wire rx_i_clip;
    wire rx_q_clip;

    assign rx_i_clip = (rx_i == 12'sh7FF) || (rx_i == 12'sh800);
    assign rx_q_clip = (rx_q == 12'sh7FF) || (rx_q == 12'sh800);


    ila_rx_debug ila_rx_debug_inst (
        .clk(rx_clk),

        // {busy, done, tx, rx_en, tx_en, capture_en,
        //  IQ有效, 帧错, I削顶, Q削顶}
        .probe0({
            busy,
            done,
            tx_active,
            force_rx_on,
            force_tx_on,
            rx_capture_en,
            rx_valid,
            decoder_frame_error,
            rx_i_clip,
            rx_q_clip
        }),

        .probe1(state),
        .probe2(state_count),
        .probe3(sample_index),

        // 高12位为 I，低12位为 Q
        .probe4({rx_i, rx_q}),

        .probe5(freq_index),

        // 高16位：相参计数；低16位：非相参计数
        .probe6({coherent_count, noncoherent_count}),
        
        .probe7(rx_frame),
        .probe8(rx_frame_rise),
        .probe9(rx_frame_fall),
        .probe10(rx_data_s),
        .probe11(rx_data_rise),
        .probe12(rx_data_fall),
        .probe13(decoder_have_msb)
    );

    always @(posedge bram_clk) begin
        case (bram_rdaddr[2:0])
            3'd0: bram_rddata_r <= capture_mem[bram_rdaddr[9:3]][31:0];
            3'd1: bram_rddata_r <= capture_mem[bram_rdaddr[9:3]][63:32];
            3'd2: bram_rddata_r <= capture_mem[bram_rdaddr[9:3]][95:64];
            3'd3: bram_rddata_r <= capture_mem[bram_rdaddr[9:3]][127:96];
            3'd4: bram_rddata_r <= capture_mem[bram_rdaddr[9:3]][159:128];
            3'd5: bram_rddata_r <= capture_mem[bram_rdaddr[9:3]][191:160];
            default: bram_rddata_r <= 32'd0;
        endcase
    end

    /* Legacy wideband sample-energy accumulator retained here only as design
       history. It is disabled because it cannot distinguish RX frequencies.
    always @(posedge rx_clk) begin
        if (rst) begin
            start_meta <= 1'b0;
            start_sync <= 1'b0;
            start_seen <= 1'b0;
            stop_meta <= 1'b0;
            stop_sync <= 1'b0;
            stop_seen <= 1'b0;
            state <= ST_IDLE;
            state_count <= 32'd0;
            flush_count <= 2'd0;
            advance_frequency <= 1'b0;
            sample_index <= {SAMPLE_COUNT_WIDTH{1'b0}};
            first_i <= 32'd0;
            first_q <= 32'd0;
            point_overflow <= 1'b0;
            point_frame_error <= 1'b0;
            frequency_energy <= 64'd0;
            pipeline_valid <= 1'b0;
            sum_valid <= 1'b0;
            energy_valid <= 1'b0;
            coherent_count <= 16'd0;
            noncoherent_count <= 16'd0;
            freq_index <= 7'd0;
            tx_active <= 1'b0;
            busy <= 1'b0;
            done <= 1'b0;
            capture_count <= 16'd0;
            rx_samples_last <= 32'd0;
            rx_overflow <= 1'b0;
        end
        else begin
            start_meta <= start_toggle;
            start_sync <= start_meta;
            stop_meta <= stop_toggle;
            stop_sync <= stop_meta;

            if (stop_sync != stop_seen) begin
                stop_seen <= stop_sync;
                state <= ST_IDLE;
                state_count <= 32'd0;
                tx_active <= 1'b0;
                busy <= 1'b0;
                done <= 1'b0;
                coherent_count <= 16'd0;
                noncoherent_count <= 16'd0;
                advance_frequency <= 1'b0;
                pipeline_valid <= 1'b0;
                sum_valid <= 1'b0;
                energy_valid <= 1'b0;
                frequency_energy <= 64'd0;
            end
            else begin
                pipeline_valid <= 1'b0;
                sum_valid <= 1'b0;
                energy_valid <= 1'b0;

                // One-cycle synchronous RAM pipeline for the selected RX2 path.
                if (pipeline_valid) begin
                    coherent_i_mem[pipeline_addr] <= coherent_i_next;
                    coherent_q_mem[pipeline_addr] <= coherent_q_next;
                    if (pipeline_last_coherent) begin
                        sum_addr <= pipeline_addr;
                        sum_first_noncoherent <= pipeline_first_noncoherent;
                        sum_last_noncoherent <= pipeline_last_noncoherent;
                        sum_noncoherent_read <= noncoherent_energy_read;
                        sum_i <= coherent_i_next;
                        sum_q <= coherent_q_next;
                        sum_valid <= 1'b1;
                    end
                end

                // Keep the BRAM read/add and the 32-bit squaring in separate cycles.
                if (sum_valid) begin
                    energy_addr <= sum_addr;
                    energy_first_noncoherent <= sum_first_noncoherent;
                    energy_last_noncoherent <= sum_last_noncoherent;
                    energy_noncoherent_read <= sum_noncoherent_read;
                    energy_i_square <= sum_i * sum_i;
                    energy_q_square <= sum_q * sum_q;
                    energy_valid <= 1'b1;
                end

                if (energy_valid) begin
                    noncoherent_energy_mem[energy_addr] <= sample_energy;
                    if (energy_last_noncoherent) begin
                        frequency_energy <= frequency_energy_next;
                    end
                end

                if ((state == ST_RX) && decoder_frame_error) begin
                    point_frame_error <= 1'b1;
                end

                if ((state == ST_RX) && rx_valid) begin
                    if (sample_index < RX_BUFFER_DEPTH) begin
                        coherent_i_read <= coherent_i_mem[sample_index[SAMPLE_ADDR_WIDTH-1:0]];
                        coherent_q_read <= coherent_q_mem[sample_index[SAMPLE_ADDR_WIDTH-1:0]];
                        noncoherent_energy_read <=
                            noncoherent_energy_mem[sample_index[SAMPLE_ADDR_WIDTH-1:0]];
                        pipeline_addr <= sample_index[SAMPLE_ADDR_WIDTH-1:0];
                        pipeline_i <= rx_i;
                        pipeline_q <= rx_q;
                        pipeline_first_coherent <= (coherent_count == 16'd0);
                        pipeline_last_coherent <=
                            ((coherent_count + 16'd1) >= coherent_repetitions);
                        pipeline_first_noncoherent <= (noncoherent_count == 16'd0);
                        pipeline_last_noncoherent <=
                            ((noncoherent_count + 16'd1) >= noncoherent_segments);
                        pipeline_valid <= 1'b1;

                        if (sample_index == 0) begin
                            first_i <= {{20{rx_i[11]}}, rx_i};
                            first_q <= {{20{rx_q[11]}}, rx_q};
                        end
                        sample_index <= sample_index + 1'b1;
                    end
                    else begin
                        point_overflow <= 1'b1;
                        rx_overflow <= 1'b1;
                    end
                end

                case (state)
                    ST_IDLE: begin
                        tx_active <= 1'b0;
                        busy <= 1'b0;
                        state_count <= 32'd0;
                        if (start_sync != start_seen) begin
                            start_seen <= start_sync;
                            busy <= 1'b1;
                            done <= 1'b0;
                            tx_active <= 1'b1;
                            freq_index <= 7'd0;
                            capture_count <= 16'd0;
                            coherent_count <= 16'd0;
                            noncoherent_count <= 16'd0;
                            point_overflow <= 1'b0;
                            point_frame_error <= 1'b0;
                            rx_overflow <= 1'b0;
                            state <= ST_TX;
                        end
                    end

                    ST_TX: begin
                        busy <= 1'b1;
                        tx_active <= 1'b1;
                        if ((tx_sample_count == 32'd0) ||
                            (state_count >= (tx_sample_count - 32'd1))) begin
                            state_count <= 32'd0;
                            tx_active <= 1'b0;
                            state <= (guard1_sample_count == 32'd0) ?
                                     ST_RX : ST_GUARD1;
                            if (guard1_sample_count == 32'd0) begin
                                sample_index <= {SAMPLE_COUNT_WIDTH{1'b0}};
                                first_i <= 32'd0;
                                first_q <= 32'd0;
                                if ((coherent_count == 0) &&
                                    (noncoherent_count == 0)) begin
                                    frequency_energy <= 64'd0;
                                end
                            end
                        end
                        else begin
                            state_count <= state_count + 32'd1;
                        end
                    end

                    ST_GUARD1: begin
                        busy <= 1'b1;
                        tx_active <= 1'b0;
                        if (state_count >= (guard1_sample_count - 32'd1)) begin
                            state_count <= 32'd0;
                            sample_index <= {SAMPLE_COUNT_WIDTH{1'b0}};
                            first_i <= 32'd0;
                            first_q <= 32'd0;
                            if ((coherent_count == 0) &&
                                (noncoherent_count == 0)) begin
                                frequency_energy <= 64'd0;
                            end
                            state <= ST_RX;
                        end
                        else begin
                            state_count <= state_count + 32'd1;
                        end
                    end

                    ST_RX: begin
                        busy <= 1'b1;
                        tx_active <= 1'b0;
                        if ((rx_sample_count == 32'd0) ||
                            (state_count >= (rx_sample_count - 32'd1))) begin
                            state_count <= 32'd0;
                            flush_count <= 2'd0;
                            state <= ST_RX_FLUSH;
                        end
                        else begin
                            state_count <= state_count + 32'd1;
                        end
                    end

                    ST_RX_FLUSH: begin
                        busy <= 1'b1;
                        tx_active <= 1'b0;
                        if (flush_count < 2'd3) begin
                            flush_count <= flush_count + 2'd1;
                        end
                        else begin
                            flush_count <= 2'd0;
                            rx_samples_last <= sample_index;
                            advance_frequency <= 1'b0;

                            if ((coherent_count + 16'd1) >= coherent_repetitions) begin
                                coherent_count <= 16'd0;
                                if ((noncoherent_count + 16'd1) >= noncoherent_segments) begin
                                    noncoherent_count <= 16'd0;
                                    capture_mem[freq_index] <= {
                                        first_q,
                                        first_i,
                                        {{(32-SAMPLE_COUNT_WIDTH){1'b0}}, sample_index},
                                        frequency_energy[63:32],
                                        frequency_energy[31:0],
                                        {freq_index, 1'b1, point_overflow,
                                         point_frame_error, 6'd0,
                                         sample_index[15:0]}
                                    };
                                    advance_frequency <= 1'b1;
                                    if (capture_count < {8'd0, point_count}) begin
                                        capture_count <= capture_count + 16'd1;
                                    end
                                end
                                else begin
                                    noncoherent_count <= noncoherent_count + 16'd1;
                                end
                            end
                            else begin
                                coherent_count <= coherent_count + 16'd1;
                            end

                            if (guard2_sample_count == 32'd0) begin
                                if (((coherent_count + 16'd1) >= coherent_repetitions) &&
                                    ((noncoherent_count + 16'd1) >= noncoherent_segments)) begin
                                    if (({1'b0, freq_index} + 8'd1) >= point_count) begin
                                        freq_index <= 7'd0;
                                        done <= 1'b1;
                                        busy <= 1'b0;
                                        tx_active <= 1'b0;
                                        state <= ST_IDLE;
                                    end
                                    else begin
                                        freq_index <= freq_index + 7'd1;
                                        tx_active <= 1'b1;
                                        state <= ST_TX;
                                    end
                                    point_overflow <= 1'b0;
                                    point_frame_error <= 1'b0;
                                    rx_overflow <= 1'b0;
                                end
                                else begin
                                    tx_active <= 1'b1;
                                    state <= ST_TX;
                                end
                            end
                            else begin
                                state <= ST_GUARD2;
                            end
                        end
                    end

                    ST_GUARD2: begin
                        busy <= 1'b1;
                        tx_active <= 1'b0;
                        if (state_count >= (guard2_sample_count - 32'd1)) begin
                            state_count <= 32'd0;
                            if (advance_frequency) begin
                                if (({1'b0, freq_index} + 8'd1) >= point_count) begin
                                    freq_index <= 7'd0;
                                    done <= 1'b1;
                                    busy <= 1'b0;
                                    tx_active <= 1'b0;
                                    state <= ST_IDLE;
                                end
                                else begin
                                    freq_index <= freq_index + 7'd1;
                                    tx_active <= 1'b1;
                                    state <= ST_TX;
                                end
                                point_overflow <= 1'b0;
                                point_frame_error <= 1'b0;
                                rx_overflow <= 1'b0;
                            end
                            else begin
                                tx_active <= 1'b1;
                                state <= ST_TX;
                            end
                            advance_frequency <= 1'b0;
                        end
                        else begin
                            state_count <= state_count + 32'd1;
                        end
                    end

                    default: begin
                        state <= ST_IDLE;
                    end
                endcase
            end
        end
    end
    */

    // Narrowband RX detector. Each scan point mixes the selected RX2 stream
    // with its own NCO, coherently integrates one RX window, then averages the
    // resulting window energy over all configured repetitions.
    always @(posedge rx_clk) begin
        if (rst) begin
            start_meta <= 1'b0;
            start_sync <= 1'b0;
            start_seen <= 1'b0;
            stop_meta <= 1'b0;
            stop_sync <= 1'b0;
            stop_seen <= 1'b0;
            state <= ST_IDLE;
            state_count <= 32'd0;
            advance_frequency <= 1'b0;
            sample_index <= {SAMPLE_COUNT_WIDTH{1'b0}};
            processed_sample_count <= {SAMPLE_COUNT_WIDTH{1'b0}};
            first_i <= 32'd0;
            first_q <= 32'd0;
            point_overflow <= 1'b0;
            point_frame_error <= 1'b0;
            frequency_energy <= 64'd0;
            rx_dc_i_est <= {RX_DC_EST_WIDTH{1'b0}};
            rx_dc_q_est <= {RX_DC_EST_WIDTH{1'b0}};
            nco_phase_acc <= 32'd0;
            sample_fifo_wrptr <= 6'd0;
            sample_fifo_rdptr <= 6'd0;
            mul_valid <= 1'b0;
            mix_valid <= 1'b0;
            product_i_cos <= 25'sd0;
            product_q_sin <= 25'sd0;
            product_q_cos <= 25'sd0;
            product_i_sin <= 25'sd0;
            mixed_i <= 26'sd0;
            mixed_q <= 26'sd0;
            ddc_sum_i <= 48'sd0;
            ddc_sum_q <= 48'sd0;
            energy_i_square <= 64'sd0;
            energy_q_square <= 64'sd0;
            coherent_count <= 16'd0;
            noncoherent_count <= 16'd0;
            freq_index <= 7'd0;
            tx_active <= 1'b0;
            busy <= 1'b0;
            done <= 1'b0;
            capture_count <= 16'd0;
            rx_samples_last <= 32'd0;
            rx_overflow <= 1'b0;
        end
        else begin
            start_meta <= start_toggle;
            start_sync <= start_meta;
            stop_meta <= stop_toggle;
            stop_sync <= stop_meta;

            // Track during idle/guard/RX, but freeze during TX so strong
            // self-leakage cannot pull the estimate away from the RX baseline.
            if (rx_valid && !tx_active) begin
                rx_dc_i_est <= rx_dc_i_est +
                    ($signed(rx_i_dc_delta) >>> RX_DC_TRACK_SHIFT);
                rx_dc_q_est <= rx_dc_q_est +
                    ($signed(rx_q_dc_delta) >>> RX_DC_TRACK_SHIFT);
            end

            if (stop_sync != stop_seen) begin
                stop_seen <= stop_sync;
                state <= ST_IDLE;
                state_count <= 32'd0;
                tx_active <= 1'b0;
                busy <= 1'b0;
                done <= 1'b0;
                coherent_count <= 16'd0;
                noncoherent_count <= 16'd0;
                advance_frequency <= 1'b0;
                sample_index <= {SAMPLE_COUNT_WIDTH{1'b0}};
                processed_sample_count <= {SAMPLE_COUNT_WIDTH{1'b0}};
                mul_valid <= 1'b0;
                mix_valid <= 1'b0;
                ddc_sum_i <= 48'sd0;
                ddc_sum_q <= 48'sd0;
                frequency_energy <= 64'd0;
            end
            else begin
                // CORDIC output and complex multiplier pipeline.  The AD9361 RX
                // interface used here represents an RF tone below the LO as a
                // positive-rotating complex tone, so rotate by +NCO phase:
                // (I+jQ)*(cos+j sin) = (I cos-Q sin)+j(Q cos+I sin).
                mul_valid <= nco_output_valid;
                if (nco_output_valid) begin
                    product_i_cos <= $signed(sample_i_fifo[sample_fifo_rdptr]) *
                                     $signed(nco_cos);
                    product_q_sin <= $signed(sample_q_fifo[sample_fifo_rdptr]) *
                                     $signed(nco_sin);
                    product_q_cos <= $signed(sample_q_fifo[sample_fifo_rdptr]) *
                                     $signed(nco_cos);
                    product_i_sin <= $signed(sample_i_fifo[sample_fifo_rdptr]) *
                                     $signed(nco_sin);
                    sample_fifo_rdptr <= sample_fifo_rdptr + 6'd1;
                end

                mix_valid <= mul_valid;
                if (mul_valid) begin
                    mixed_i <= $signed(product_i_cos) + $signed(product_q_sin);
                    mixed_q <= $signed(product_q_cos) - $signed(product_i_sin);
                end

                if (mix_valid) begin
                    ddc_sum_i <= ddc_sum_i + {{22{mixed_i[25]}}, mixed_i};
                    ddc_sum_q <= ddc_sum_q + {{22{mixed_q[25]}}, mixed_q};
                    processed_sample_count <= processed_sample_count + 1'b1;
                end

                if (nco_input_valid) begin
                    sample_i_fifo[sample_fifo_wrptr] <= rx_i_dc_removed;
                    sample_q_fifo[sample_fifo_wrptr] <= rx_q_dc_removed;
                    sample_fifo_wrptr <= sample_fifo_wrptr + 6'd1;
                    nco_phase_acc <= nco_phase_acc + nco_phase_inc;

                    if (sample_index == 0) begin
                        first_i <= {{19{rx_i_dc_removed[12]}}, rx_i_dc_removed};
                        first_q <= {{19{rx_q_dc_removed[12]}}, rx_q_dc_removed};
                    end
                    sample_index <= sample_index + 1'b1;
                end
                else if ((state == ST_RX) && rx_valid &&
                         (sample_index >= RX_BUFFER_DEPTH)) begin
                    point_overflow <= 1'b1;
                    rx_overflow <= 1'b1;
                end

                if ((state == ST_RX) && decoder_frame_error) begin
                    point_frame_error <= 1'b1;
                end

                case (state)
                    ST_IDLE: begin
                        tx_active <= 1'b0;
                        busy <= 1'b0;
                        state_count <= 32'd0;
                        if (start_sync != start_seen) begin
                            start_seen <= start_sync;
                            busy <= 1'b1;
                            done <= 1'b0;
                            tx_active <= 1'b1;
                            freq_index <= 7'd0;
                            capture_count <= 16'd0;
                            coherent_count <= 16'd0;
                            noncoherent_count <= 16'd0;
                            frequency_energy <= 64'd0;
                            point_overflow <= 1'b0;
                            point_frame_error <= 1'b0;
                            rx_overflow <= 1'b0;
                            state <= ST_TX;
                        end
                    end

                    ST_TX: begin
                        busy <= 1'b1;
                        tx_active <= 1'b1;
                        if ((tx_sample_count == 32'd0) ||
                            (state_count >= (tx_sample_count - 32'd1))) begin
                            state_count <= 32'd0;
                            tx_active <= 1'b0;
                            if (guard1_sample_count == 32'd0) begin
                                sample_index <= {SAMPLE_COUNT_WIDTH{1'b0}};
                                processed_sample_count <= {SAMPLE_COUNT_WIDTH{1'b0}};
                                first_i <= 32'd0;
                                first_q <= 32'd0;
                                nco_phase_acc <= 32'd0;
                                ddc_sum_i <= 48'sd0;
                                ddc_sum_q <= 48'sd0;
                                state <= ST_RX;
                            end
                            else begin
                                state <= ST_GUARD1;
                            end
                        end
                        else begin
                            state_count <= state_count + 32'd1;
                        end
                    end

                    ST_GUARD1: begin
                        busy <= 1'b1;
                        tx_active <= 1'b0;
                        if (state_count >= (guard1_sample_count - 32'd1)) begin
                            state_count <= 32'd0;
                            sample_index <= {SAMPLE_COUNT_WIDTH{1'b0}};
                            processed_sample_count <= {SAMPLE_COUNT_WIDTH{1'b0}};
                            first_i <= 32'd0;
                            first_q <= 32'd0;
                            nco_phase_acc <= 32'd0;
                            ddc_sum_i <= 48'sd0;
                            ddc_sum_q <= 48'sd0;
                            state <= ST_RX;
                        end
                        else begin
                            state_count <= state_count + 32'd1;
                        end
                    end

                    ST_RX: begin
                        busy <= 1'b1;
                        tx_active <= 1'b0;
                        if ((rx_sample_count == 32'd0) ||
                            (state_count >= (rx_sample_count - 32'd1))) begin
                            state_count <= 32'd0;
                            state <= ST_RX_FLUSH;
                        end
                        else begin
                            state_count <= state_count + 32'd1;
                        end
                    end

                    ST_RX_FLUSH: begin
                        busy <= 1'b1;
                        tx_active <= 1'b0;
                        // Wait until every CORDIC/multiplier result belonging to
                        // this window has reached the coherent accumulators.
                        if ((processed_sample_count == sample_index) &&
                            !nco_output_valid && !mul_valid && !mix_valid) begin
                            rx_samples_last <= sample_index;
                            energy_i_square <= ddc_sum_i_scaled * ddc_sum_i_scaled;
                            energy_q_square <= ddc_sum_q_scaled * ddc_sum_q_scaled;
                            state <= ST_POWER;
                        end
                    end

                    ST_POWER: begin
                        busy <= 1'b1;
                        tx_active <= 1'b0;
                        frequency_energy <= frequency_energy_next;

                        if (coherent_block_complete) begin
                            coherent_count <= 16'd0;
                            if ((noncoherent_count + 16'd1) >=
                                noncoherent_segments) begin
                                noncoherent_count <= 16'd0;
                            end
                            else begin
                                noncoherent_count <= noncoherent_count + 16'd1;
                            end
                        end
                        else begin
                            coherent_count <= coherent_count + 16'd1;
                        end

                        if (point_complete) begin
                            capture_mem[freq_index] <= {
                                first_q,
                                first_i,
                                {{(32-SAMPLE_COUNT_WIDTH){1'b0}}, sample_index},
                                frequency_energy_next[63:32],
                                frequency_energy_next[31:0],
                                {freq_index, 1'b1, point_overflow,
                                 point_frame_error, 6'd0,
                                 sample_index[15:0]}
                            };
                            if (capture_count < {8'd0, point_count}) begin
                                capture_count <= capture_count + 16'd1;
                            end
                            advance_frequency <= 1'b1;
                        end
                        else begin
                            advance_frequency <= 1'b0;
                        end

                        if (guard2_sample_count == 32'd0) begin
                            if (point_complete) begin
                                if (({1'b0, freq_index} + 8'd1) >= point_count) begin
                                    freq_index <= 7'd0;
                                    done <= 1'b1;
                                    busy <= 1'b0;
                                    tx_active <= 1'b0;
                                    frequency_energy <= 64'd0;
                                    state <= ST_IDLE;
                                end
                                else begin
                                    freq_index <= freq_index + 7'd1;
                                    tx_active <= 1'b1;
                                    frequency_energy <= 64'd0;
                                    point_overflow <= 1'b0;
                                    point_frame_error <= 1'b0;
                                    rx_overflow <= 1'b0;
                                    state <= ST_TX;
                                end
                            end
                            else begin
                                tx_active <= 1'b1;
                                state <= ST_TX;
                            end
                        end
                        else begin
                            state_count <= 32'd0;
                            state <= ST_GUARD2;
                        end
                    end

                    ST_GUARD2: begin
                        busy <= 1'b1;
                        tx_active <= 1'b0;
                        if (state_count >= (guard2_sample_count - 32'd1)) begin
                            state_count <= 32'd0;
                            if (advance_frequency) begin
                                if (({1'b0, freq_index} + 8'd1) >= point_count) begin
                                    freq_index <= 7'd0;
                                    done <= 1'b1;
                                    busy <= 1'b0;
                                    tx_active <= 1'b0;
                                    frequency_energy <= 64'd0;
                                    state <= ST_IDLE;
                                end
                                else begin
                                    freq_index <= freq_index + 7'd1;
                                    tx_active <= 1'b1;
                                    frequency_energy <= 64'd0;
                                    point_overflow <= 1'b0;
                                    point_frame_error <= 1'b0;
                                    rx_overflow <= 1'b0;
                                    state <= ST_TX;
                                end
                            end
                            else begin
                                tx_active <= 1'b1;
                                state <= ST_TX;
                            end
                            advance_frequency <= 1'b0;
                        end
                        else begin
                            state_count <= state_count + 32'd1;
                        end
                    end

                    default: begin
                        state <= ST_IDLE;
                    end
                endcase
            end
        end
    end

endmodule
