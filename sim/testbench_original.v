`timescale 1ns / 1ps

module top_tb;
reg clk_fs;
reg rst;
reg start;
wire [17:0] sig_shaped;
wire [33:0] mul_sig_shaped;
wire [15:0] pcm_phase;
wire [11:0] sin;
wire [11:0] cos;
wire signed    [17:0]      res0;
wire signed    [17:0]      res1;
wire signed    [17:0]      res2;
wire signed    [17:0]      res3;
wire signed    [17:0]      res4;
wire signed    [17:0]      res5;
wire signed    [17:0]      res6;

//文件路径变量
integer Sin_path      ;
integer Cos_path      ;
integer Shape_path    ;
integer KfShape_path  ;
integer CumPhase_path ;
integer Res0_path ;
integer Res1_path ;
integer Res2_path ;
integer Res3_path ;
integer Res4_path ;
integer Res5_path ;
integer Res6_path ;
    
initial begin
      clk_fs <= 1'b0;   
      rst <= 1'b1;
      start <= 1'b0;
      #100 rst <= 1'b0; 
      #10 start <= 1'b1;
      
      //若文件不存在，则会自动创建文件。每次打开文件会自动清空上一次写进去的内容。关闭后自动保存。
      Sin_path      = $fopen("E:/Code/Matlab/Single_Channel_Simulator_SimData_Process/Sin.txt");
      Cos_path      = $fopen("E:/Code/Matlab/Single_Channel_Simulator_SimData_Process/Cos.txt");
      Shape_path    = $fopen("E:/Code/Matlab/Single_Channel_Simulator_SimData_Process/Shape.txt");
      KfShape_path  = $fopen("E:/Code/Matlab/Single_Channel_Simulator_SimData_Process/KfShape.txt");
      CumPhase_path = $fopen("E:/Code/Matlab/Single_Channel_Simulator_SimData_Process/CumPhase.txt");
      Res0_path = $fopen("E:/Code/Matlab/Single_Channel_Simulator_SimData_Process/Res0_6_144M.txt");
      Res1_path = $fopen("E:/Code/Matlab/Single_Channel_Simulator_SimData_Process/Res1_6_144M.txt");
      Res2_path = $fopen("E:/Code/Matlab/Single_Channel_Simulator_SimData_Process/Res2_6_144M.txt");
      Res3_path = $fopen("E:/Code/Matlab/Single_Channel_Simulator_SimData_Process/Res3_6_144M.txt");
      Res4_path = $fopen("E:/Code/Matlab/Single_Channel_Simulator_SimData_Process/Res4_6_144M.txt");
      Res5_path = $fopen("E:/Code/Matlab/Single_Channel_Simulator_SimData_Process/Res5_6_144M.txt");
      Res6_path = $fopen("E:/Code/Matlab/Single_Channel_Simulator_SimData_Process/Res6_6_144M.txt");
end 

always #10  clk_fs = ~clk_fs;
    
always @(posedge clk_fs) begin
	   if ((Sin_path == 0)||(Cos_path == 0)||(Shape_path == 0)||(KfShape_path == 0)||(CumPhase_path == 0)||(Res0_path == 0)||(Res1_path == 0)||(Res2_path == 0)||(Res3_path == 0)||(Res4_path == 0)||(Res5_path == 0)||(Res6_path == 0)) begin
           $display("Error: File, output.dat could not be opened.\nExiting Simulation.");
           $stop;
       end
	   else  begin
	       $fdisplay(Sin_path, "%d",$signed(sin));
	       $fdisplay(Cos_path, "%d",$signed(cos));
	       $fdisplay(Shape_path, "%d",$signed(sig_shaped));
	       $fdisplay(KfShape_path, "%d",$signed(mul_sig_shaped));
	       $fdisplay(CumPhase_path, "%d",$signed(pcm_phase));
	       $fdisplay(Res0_path, "%d",$signed(res0));
	       $fdisplay(Res1_path, "%d",$signed(res1));
	       $fdisplay(Res2_path, "%d",$signed(res2));
	       $fdisplay(Res3_path, "%d",$signed(res3));
	       $fdisplay(Res4_path, "%d",$signed(res4));
	       $fdisplay(Res5_path, "%d",$signed(res5));
	       $fdisplay(Res6_path, "%d",$signed(res6));
	   end
 end
    
    pcm_fm_baseband baseband_inst0(
     .clk_fs            (    clk_fs     ),
     .rst               (    rst        ),
     .start             (    start      ),
     .sin               (    sin        ),
     .cos               (    cos        ),
     .sig_shaped        (  sig_shaped   ),
     .mul_sig_shaped    (mul_sig_shaped ),
     .pcm_phase         (  pcm_phase    ),
     .res0              (   res0        ),
     .res1              (   res1        ),
     .res2              (   res2        ),
     .res3              (   res3        ),
     .res4              (   res4        ),
     .res5              (   res5        ),
     .res6              (   res6        ),
     .modu_fre          (  16'd4587     ), // 调频灵敏度 16位小数
     .pcm_rate          (  21'd402653   ), // PCM 码速率，单位Mbps  5位整数位，16位小数位 共21位
	 .pcm_period        (  24'd170666   )  // PCM 码元周期，单位 us  4位整数位，20位小数位 共24位    
    );
    
endmodule
