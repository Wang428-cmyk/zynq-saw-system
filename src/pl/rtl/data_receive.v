`timescale 1ns / 1ps

module Data_Receive(
    input               bram_clk_a,
    (* mark_debug = "true" *)input [31:0]        bram_wrdata_a,
    (* mark_debug = "true" *)input [3:0]         bram_we_a,
    (* mark_debug = "true" *)input               bram_en_a,
    (* mark_debug = "true" *)input               rst,
    
    (* mark_debug = "true" *)output  [20:0]	     pcm_rate,     // PCM 码速率，单位Mbps  5位整数位，16位小数位 共21位
    (* mark_debug = "true" *)output  [23:0]      pcm_period,   // PCM 码元周期，单位 us  4位整数位，20位小数位 共24位
    (* mark_debug = "true" *)output  [15:0]      modu_coff     // 调频灵敏度
    );
    
    wire state_kf;
    wire state_pcm_period;
    wire state_pcm_rate;   
    
    Frame_Head_Check Frame_Head_Check_inst0(
        .bram_clk_a(bram_clk_a),
        .bram_wrdata_a(bram_wrdata_a),
        .bram_we_a(bram_we_a),
        .bram_en_a(bram_en_a),
        .rst(rst),
        .state_kf(state_kf),
        .state_pcm_period(state_pcm_period),
        .state_pcm_rate(state_pcm_rate)
        );
    
    Kf_Data Kf_Data_inst0(
        .bram_clk(bram_clk_a),
        .rst(rst),
        .bram_en(bram_en_a),
        .wr_state(state_kf),
        .bram_wr_data(bram_wrdata_a),
        .modu_coff(modu_coff)
        );

    PCM_Period_Data PCM_Period_Data_inst0(
        .bram_clk(bram_clk_a),
        .rst(rst),
        .bram_en(bram_en_a),
        .wr_state(state_pcm_period),
        .bram_wr_data(bram_wrdata_a),
        .pcm_period(pcm_period)
        );
    
    PCM_Rate_Data PCM_Rate_Data_inst0(
        .bram_clk(bram_clk_a),
        .rst(rst),
        .bram_en(bram_en_a),
        .wr_state(state_pcm_rate),
        .bram_wr_data(bram_wrdata_a),
        .pcm_rate(pcm_rate)
        );

endmodule
