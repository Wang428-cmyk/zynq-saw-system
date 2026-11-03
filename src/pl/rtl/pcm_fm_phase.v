`timescale 1ns / 1ps

module pcm_fm_phase
#(
    parameter       DEBUG_MARK = "FALSE"
)(
        input                      clk_fs,
        input                      rst,
        input                      start,
        input       [15:0]         modu_fre, // 调频灵敏度  16位无符号小数
        input       [20:0]         pcm_rate,    // PCM 码速率，单位Mbps  5位整数位，16位小数位 共21位
        input       [23:0]         pcm_period,  // PCM 码元周期，单位 us  4位整数位，20位小数位 共24位
        input       [7:0]          pcm_data,    //pcm码流输入
        input       [13:0]         max_pcm_ram_addr,
        output                     ena_pcm_ram,
        output      [13:0]         pcm_ram_addr,
        output [17:0] sig_shaped,
        output signed [33:0] mul_sig_shaped,
        output signed [15:0]       pcm_phase,        //pcm累加相位输出，1位符号位，15位小数位        
        
        //用于FPGA仿真输出成型滤波器系数
        output signed    [17:0]      res0,
        output signed    [17:0]      res1,
        output signed    [17:0]      res2,
        output signed    [17:0]      res3,
        output signed    [17:0]      res4,
        output signed    [17:0]      res5,
        output signed    [17:0]      res6
    );
    
    // 频偏相位累加值  2Π*fd*n/fs = Π*2fd/fs*n     频偏单位：MHz，采样率单位：MHz
    localparam Freq_0ffset_Phase = 33'd27962026;    // 频偏相位 = 2fd/fs； 2*0.2/61.44*2^32 = 4026531    整数位1位，小数位32位
    
    (* mark_debug = DEBUG_MARK *)wire pcm_code_out;     
    wire code_en; 
    wire shape_done;
    reg [32:0] cum_mul_shape;

    always@(posedge clk_fs) begin
        if(rst == 1'b1)begin
            cum_mul_shape <= 33'd0;
        end
        else begin
            if(shape_done == 1'b1) begin
                /* always中不能使用两次x<=x+c;因为是并行执行，所以会根据实际布局布线执行其中一条指令。
                   这里有两条语句，分别对应无频率基带信号和有频率基带信号，有频率基带信号用于仿真数据生成。
                */
                //成型滤波对应基带信号相位
                cum_mul_shape <= cum_mul_shape + {mul_sig_shaped[33],mul_sig_shaped[31:0]};
                //成型滤波对应基带信号相位加上信号频偏对应基带信号相位
                //cum_mul_shape <= cum_mul_shape + {mul_sig_shaped[33],mul_sig_shaped[31:0]} + Freq_0ffset_Phase;
            end
            else begin
                cum_mul_shape <= cum_mul_shape;
            end
        end  
    end
     
    assign pcm_phase = cum_mul_shape[32:17];
 
    pcm_code gen_pcm_code(
         .clk_fs(clk_fs),
         .rst(rst),
         .start(start),
         .data_from_pcm(pcm_data),   //从PCM-RAM中提取的PCM数据
         .code_en(code_en),
         .ram_addr(pcm_ram_addr),       //pcm ram 地址
         .ena_pcm_ram(ena_pcm_ram),    //pcm ram使能信号
         .pcm_code_out(pcm_code_out),         //输出的码元
         .max_ram_addr(max_pcm_ram_addr)
        );
   
     SqrtRcosFlt_pulse_shaping SqrtRcosFlt_pulse_shaping_1 (
        .rst(rst), 
        .clk(clk_fs), 
        .shape_en(start), 
        .pcm_rate(pcm_rate), 
        .pcm_period(pcm_period), 
        .code_en(code_en), 
        .pcm_code_in(pcm_code_out), 
        .sig_shaped_out(sig_shaped), 
        .shape_done(shape_done),
        //用于FPGA仿真输出成型滤波器系数
        .res0(res0),
        .res1(res1),
        .res2(res2),
        .res3(res3),
        .res4(res4),
        .res5(res5),
        .res6(res6)
        );
        
     mult_pcm multi_pcm (
      .CLK(clk_fs),         // input  wire CLK
      .A(modu_fre),         // input  wire [15 : 0] A  16位无符号小数
      .B(sig_shaped),       // input  wire [17 : 0] B  符号位1位，整数位1位，小数位16位
      .P(mul_sig_shaped)    // output wire [33 : 0] P  符号位1位，整数位1位，小数位32位
    );    

endmodule
