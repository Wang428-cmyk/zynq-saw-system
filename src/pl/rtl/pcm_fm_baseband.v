`timescale 1ns / 1ps

module pcm_fm_baseband
#(
    parameter       DEBUG_MARK = "FALSE"
)(
    input                      clk_fs,
    input                      rst,
    input                      start,
    input       [15:0]         modu_fre,   // 调频灵敏度 16位小数
    input       [20:0]         pcm_rate,   // PCM 码速率，单位Mbps  5位整数位，16位小数位 共21位
	input       [23:0]         pcm_period,  // PCM 码元周期，单位 us  4位整数位，20位小数位 共24位    
    (* mark_debug = DEBUG_MARK *)output     [11:0]          sin,
    (* mark_debug = DEBUG_MARK *)output     [11:0]          cos,
    
    //用于FPGA仿真输出成型滤波后的波形
    output      [17:0]         sig_shaped,
    //用于FPGA仿真输出乘以Kf*Rb/Fs之后的成型滤波波形
    output      [33:0]         mul_sig_shaped,
    //用于FPGA仿真输出除以Π之后范围为-1~+1之间的基带信号累加相位
    output      [15:0]         pcm_phase,
    //用于FPGA仿真输出成型滤波器系数
    output signed    [17:0]      res0,
	output signed    [17:0]      res1,
	output signed    [17:0]      res2,
	output signed    [17:0]      res3,
	output signed    [17:0]      res4,
	output signed    [17:0]      res5,
	output signed    [17:0]      res6
    );
    
    wire                ena_pcm_ram;
    wire       [7:0]    pcm_data;
    wire      [13:0]    pcm_ram_addr;
    
    pcm_rom pcm_rom (
      .clka(clk_fs),        // input wire clka
      .ena(ena_pcm_ram),    // input wire ena
      .addra(pcm_ram_addr), // input wire [13 : 0] addra
      .douta(pcm_data)      // output wire [7 : 0] douta
    );
    
    pcm_fm_phase #(
     .DEBUG_MARK(DEBUG_MARK)
    )
    gen_pcm_fm_phase(
      .clk_fs(clk_fs),
      .rst(rst),
      .start(start),
      .modu_fre(modu_fre), // 调频灵敏度
      .pcm_rate(pcm_rate),
      .pcm_period(pcm_period),
      .pcm_data(pcm_data),
      .max_pcm_ram_addr(14'd512),
      .ena_pcm_ram(ena_pcm_ram),
      .pcm_ram_addr(pcm_ram_addr),
      .pcm_phase(pcm_phase),
      //用于FPGA仿真输出成型滤波后的波形
      .sig_shaped(sig_shaped),
      //用于FPGA仿真输出乘以Kf*Rb/Fs之后的成型滤波波形
      .mul_sig_shaped(mul_sig_shaped),
      //用于FPGA仿真输出成型滤波器系数
      .res0(res0),
      .res1(res1),
      .res2(res2),
      .res3(res3),
      .res4(res4),
      .res5(res5),
      .res6(res6)
    );
    
    wire                en_out;
    wire [17:0]         phase_scale;
    assign phase_scale = {{2{pcm_phase[15]}},pcm_phase};
    
    wire        [31:0]  sin_cos;
    assign sin = (start==1'b0) ? 12'd0 : sin_cos[27:16];
    assign cos = (start==1'b0) ? 12'd0 : sin_cos[11:0];
    
    cos_ph cos_ph (
      .aclk(clk_fs),                                // input wire aclk
      .s_axis_phase_tvalid(start),  // input wire s_axis_phase_tvalid
      .s_axis_phase_tdata(phase_scale),    // input wire [15 : 0] s_axis_phase_tdata
      .m_axis_dout_tvalid(en_out),    // output wire m_axis_dout_tvalid
      .m_axis_dout_tdata(sin_cos)      // output wire [31 : 0] m_axis_dout_tdata
    );
    
endmodule
