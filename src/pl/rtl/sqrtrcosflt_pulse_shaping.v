`timescale 1ns / 1ps 

module SqrtRcosFlt_pulse_shaping(
	input                rst,            // 模块复位信号，高电平有效
	input                clk,            // 时钟，其频率与采样速率相同
	input                shape_en,       // 码元成型滤波使能,若为高电平则模块开始工作
	input       [20:0]   pcm_rate,       // PCM 码速率，单位Mbps  5位整数位，16位小数位 共21位
	input       [23:0]   pcm_period,     // PCM 码元周期，单位 us  4位整数位，20位小数位 共24位
	input                pcm_code_in,    // 码元输入，无符号数，1代表1，0代表-1
	output               code_en,        // 新的码元读入使能
	output      [17:0]   sig_shaped_out, // 成型滤波输出，小数位16位，整数位1位，符号位1位
    output reg           shape_done,     // 成型滤波输出使能信号
    
    //用于仿真输出FPGA成型滤波所用的滤波器系数
    output signed    [17:0]      res0,
	output signed    [17:0]      res1,
	output signed    [17:0]      res2,
	output signed    [17:0]      res3,
	output signed    [17:0]      res4,
	output signed    [17:0]      res5,
	output signed    [17:0]      res6
    );
	
// FPGA采样速率和采样周期
parameter SAMPLE_T     =    20'd273067; // 采样信号周期 1/61.44*2^20 = 17066 单位：us，小数位20位
parameter SAMPLE_RATE  =    24'd4026531;// 采样速率 61.44*2^16 = 4026531    单位：Mhz，小数位16位

// 根据输入码速率计算所得的常数，均为无符号数
reg     [24:0]      NUM_PER_SYMBOL;   // 每个码元的采样点数SAMPLE_RATE/pcm_rate，整数位9位，小数位16位
reg     [18:0]      SAMPLE_INC;       // 每个采样点码元相位的增量 pcm_rate/SAMPLE_RATE，小数位为20位
reg     [15:0]      FLT_LEN;          // 成型滤波器的阶数，ceil(NUM_PER_SYMBOL*6)，且保证其为奇数 范围58~2880 并留有裕量

// +++++++++++++++++++++++++ 内部信号 +++++++++++++++++++++++++++++++ //
// Part 1 信号：参数计算 信号
wire    [24:0]      num_per_symbol_cmp;
wire    [18:0]      sample_inc_cmp;
reg     [31:0]      flt_len_cmp_s1;
reg     [15:0]      flt_len_cmp_s2;
reg     [3:0]       delay_cnt1;            // 延时计数器 1，完成NUM_PER_SYMBOL等参数的计算需要5个时钟周期
reg                 param_done;            // 参数计算完成指示信号


// Part 2 信号：查找表地址计算
reg     [5:0]       cmp_piple_enable;        // 流水线使能信号
reg     [6:0]       flt_tab_rst;             // 查找表使能信号
reg     [3:0]       cycle_var;               // 循环变量

reg     [23:0]      code_phase_nco;          // 对每个采样点的码相位进行累加，以判断是否有新的码元移入
reg     [23:0]      code_position_cmp1;      // 当前采样点相对于码元输入位置超前的采样点数（值小于1）
reg     [23:0]      code_res_position [6:0]; // 码元冲激响应位置
reg     [15:0]      code_res_cnt      [6:0]; // 码元冲激响应计数器
reg     [8:0]       table_addr        [6:0]; // 存放查找表的地址 共七组


// Part 3 信号: 构建查找表模型，各相滤波器响应查表输出，并完成最终成型滤波，并输出
wire signed    [17:0]      impulse_res       [6:0];      // 查表所得的码元的冲激响应 共七组
reg  signed    [17:0]      impulse_res_code  [6:0];
reg                        code_buff         [6:0];

//用于仿真输出当前码速率下的成型滤波器系数
assign res0 = impulse_res[0];
assign res1 = impulse_res[1];
assign res2 = impulse_res[2];
assign res3 = impulse_res[3];
assign res4 = impulse_res[4];
assign res5 = impulse_res[5];
assign res6 = impulse_res[6];

// +++++++++++++++++++++++++ 主 程 序 +++++++++++++++++++++++++++++++ //
//  计算NUM_PER_SYMBOL、SAMPLE_INC、FLT_LEN
//          对于Vetex-6芯片 24*24乘法 输出有两个时钟的延时
always @(posedge clk) begin
	if(rst | ~shape_en) begin
		NUM_PER_SYMBOL <= 25'h0;   // 参数
		SAMPLE_INC     <= 19'h0;
		FLT_LEN        <= 16'h0;
		
		flt_len_cmp_s1 <= 32'h0;  // 参数计算辅助信号
		flt_len_cmp_s2 <= 16'h0;
		delay_cnt1     <= 4'h0;
		param_done     <= 1'b0;
	end
	else begin
		flt_len_cmp_s1 <= (num_per_symbol_cmp << 2) + (num_per_symbol_cmp << 1);  // 每个码采样点数乘以 6 
		flt_len_cmp_s2 <= flt_len_cmp_s1[31:16] + |(flt_len_cmp_s1[15:0]);          // 向上近似取整数 ceil
		if (delay_cnt1 == 4) begin  // 计算过程需要四个时钟周期
			param_done     <= 1'b1;                                 // 参数计算完成
			FLT_LEN        <= flt_len_cmp_s2 | 16'h0001;            // 保证码元长度为奇数
			NUM_PER_SYMBOL <= num_per_symbol_cmp;
			SAMPLE_INC     <= sample_inc_cmp;
			delay_cnt1     <= delay_cnt1;
		end
		else begin
			delay_cnt1     <= delay_cnt1 + 1'b1;
			NUM_PER_SYMBOL <= 25'h0;   // 参数
			SAMPLE_INC     <= 19'h0;
			FLT_LEN        <= 16'h0;
		end
	end
end

// 计算每个码元的采样点数
NumPerSymbol CMP_NumPerSymbol (
  .CLK(clk),                    // input wire CLK       
  .A(SAMPLE_RATE),              // input wire [23 : 0] A
  .B(pcm_period),               // input wire [23 : 0] B
  .CE(shape_en),                // input wire CE        
  .P(num_per_symbol_cmp)        // output wire [24 : 0] P
);

// 计算采样周期与码元周期的比值，即每个采样点对应的码相位
SampleInc CMP_SampleInc (
  .CLK(clk), // input clk
  .A(SAMPLE_T), // input [23 : 0] a
  .B(pcm_rate), // input [19 : 0] b
  
  .CE(shape_en), // input ce
  .P(sample_inc_cmp) // output [17 : 0] p
);

// ------------------- Part 2：查找表地址计算 ------------------- //
// 码相位累加器，判断是否有码元移入，并计算当前采样点相对于码元移入位置的码相位
always @(posedge clk) begin
	if(rst | ~param_done) begin
		code_phase_nco       <= 24'h0;         // 对于每个码元的采样点数进行计数，以判断是否有新的码元移入
		code_position_cmp1   <= 24'h0;         // 当前采样点相对于码元输入位置超前的采样点数（小于1）
		cmp_piple_enable     <= 5'h0;
	end
	else begin
		if(code_phase_nco >= 24'h10_0000) begin
			code_position_cmp1 <= code_phase_nco - 24'h10_0000;
			code_phase_nco     <= code_phase_nco - 24'h10_0000 + {5'b000000,SAMPLE_INC};
			cmp_piple_enable   <= {cmp_piple_enable[4:0],1'b1};
		end
		else begin
			code_position_cmp1 <= code_position_cmp1;
			code_phase_nco     <= code_phase_nco + SAMPLE_INC;
			cmp_piple_enable   <= {cmp_piple_enable[4:0],1'b0};
		end
	end
end

// 对码相位累加，得到当前时刻之前7个码元相对于当前采样点的位置
always @(posedge clk) begin
	if(rst | ~param_done)
		for(cycle_var=0;cycle_var<=6;cycle_var=cycle_var+1) begin
			code_res_position[cycle_var] <= 24'h0;
			code_res_cnt[cycle_var]      <= 16'h0;
		end			

	else begin
		if(cmp_piple_enable[0]) begin
			code_res_position[0] <= code_position_cmp1;
			code_res_cnt[0]      <= 16'h1;
			for(cycle_var=1;cycle_var<=6;cycle_var=cycle_var+1) begin
				code_res_position[cycle_var] <= code_res_position[cycle_var-1'b1] + SAMPLE_INC;
				code_res_cnt[cycle_var]      <= code_res_cnt[cycle_var-1'b1] + 1'b1;
			end
		end
		else begin
			for(cycle_var=0; cycle_var<=6; cycle_var=cycle_var+1) begin
				code_res_position[cycle_var] <= code_res_position[cycle_var] + SAMPLE_INC;
				code_res_cnt[cycle_var]      <= code_res_cnt[cycle_var] + 1'b1;
			end
		end
	end
end

// 依据上一步计算所得的结果，计算查找表的索引值（地址）
always @(posedge clk) begin
	if(rst | ~param_done)	 begin	
		flt_tab_rst <= 7'h7f;
		for(cycle_var=0; cycle_var<=6; cycle_var=cycle_var+1) 
			table_addr[cycle_var] <= 9'h0;
	end
	else begin
		for(cycle_var=0; cycle_var<=6; cycle_var=cycle_var+1) begin
			if ((code_res_cnt[cycle_var] <= FLT_LEN) && (code_res_cnt[cycle_var] != 0))
				flt_tab_rst[cycle_var] <= 1'b0;
			else
				flt_tab_rst[cycle_var] <= 1'b1;
			table_addr[cycle_var] <= code_res_position[cycle_var][19:11];
		end
	end
	
end

// ------- Part 3: 构建ROM查找表模型，并通过查找表输出时变滤波器的系数 --------- //
// 例化ROM
FltImpResTab_1 FltTab_ROM_1 (
  .clka(clk), // input clka
  .rsta(flt_tab_rst[0]), // input rsta
  .addra(table_addr[0]), // input [8 : 0] addra
  .douta(impulse_res[0]) // output [17 : 0] douta
);
FltImpResTab_2 FltTab_ROM_2 (
  .clka(clk), // input clka
  .rsta(flt_tab_rst[1]), // input rsta
  .addra(table_addr[1]), // input [8 : 0] addra
  .douta(impulse_res[1]) // output [17 : 0] douta
);
FltImpResTab_3 FltTab_ROM_3 (
  .clka(clk), // input clka
  .rsta(flt_tab_rst[2]), // input rsta
  .addra(table_addr[2]), // input [8 : 0] addra
  .douta(impulse_res[2]) // output [17 : 0] douta
);
FltImpResTab_4 FltTab_ROM_4 (
  .clka(clk), // input clka
  .rsta(flt_tab_rst[3]), // input rsta
  .addra(table_addr[3]), // input [8 : 0] addra
  .douta(impulse_res[3]) // output [17 : 0] douta
);
FltImpResTab_5 FltTab_ROM_5 (
  .clka(clk), // input clka
  .rsta(flt_tab_rst[4]), // input rsta
  .addra(table_addr[4]), // input [8 : 0] addra
  .douta(impulse_res[4]) // output [17 : 0] douta
);
FltImpResTab_6 FltTab_ROM_6 (
  .clka(clk), // input clka
  .rsta(flt_tab_rst[5]), // input rsta
  .addra(table_addr[5]), // input [8 : 0] addra
  .douta(impulse_res[5]) // output [17 : 0] douta
);
FltImpResTab_7 FltTab_ROM_7 (
  .clka(clk), // input clka
  .rsta(flt_tab_rst[6]), // input rsta
  .addra(table_addr[6]), // input [8 : 0] addra
  .douta(impulse_res[6]) // output [17 : 0] douta
);

// 码元移入
assign code_en = cmp_piple_enable[1];
always @(posedge clk) begin
	if(rst | ~param_done)	 begin	
	    shape_done <= 1'b0;
		for(cycle_var=0; cycle_var<=6; cycle_var=cycle_var+1) 
			code_buff[cycle_var] <= 2'h0;
	end
	else begin
		shape_done <= shape_done | cmp_piple_enable[5];
		if (cmp_piple_enable[3]) begin
			code_buff[0] <= pcm_code_in;
			for(cycle_var=1; cycle_var<=6; cycle_var=cycle_var+1)
				code_buff[cycle_var] <= code_buff[cycle_var - 1'b1];
		end
		else 
			for(cycle_var=0; cycle_var<=6; cycle_var=cycle_var+1)
				code_buff[cycle_var] <= code_buff[cycle_var];

	end
end

// FIR滤波输出
reg  signed [17:0]   sig_shaped;
assign sig_shaped_out = sig_shaped;
always @(posedge clk) begin
	for(cycle_var=0; cycle_var<=6; cycle_var=cycle_var+1)
		impulse_res_code[cycle_var] <= code_buff[cycle_var]?impulse_res[cycle_var]:-impulse_res[cycle_var];

	if (~shape_done)
		sig_shaped <= 18'h0;
	else
		sig_shaped <= impulse_res_code[0] + impulse_res_code[1] + impulse_res_code[2] + impulse_res_code[3] + impulse_res_code[4]
						+ impulse_res_code[5] + impulse_res_code[6];	  	
end

endmodule
