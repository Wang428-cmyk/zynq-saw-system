`timescale 1ns / 1ps

module Frame_Head_Check(
    input               bram_clk_a,
    input [31:0]        bram_wrdata_a,
    input [3:0]         bram_we_a,
    input               bram_en_a,
    input               rst,
    output reg          state_kf,
    output reg          state_pcm_rate,
    output reg          state_pcm_period
    );
    
    always @ (posedge bram_clk_a)
       begin
           if (rst == 1'b1) begin
               state_pcm_rate <= 1'b0;
               state_pcm_period <= 1'b0;
               state_kf <= 1'b0;
           end
           else if ((bram_en_a == 1'b1) && (bram_we_a == 4'hF)) begin
               case (bram_wrdata_a)                                                       
                   32'hAA5501BB :  begin//码速率
                                       state_pcm_rate <= 1'b1;
                                       state_pcm_period <= 1'b0;
                                       state_kf <= 1'b0;
                                   end
                   32'hAA5501BC :  begin//码元周期
                                       state_pcm_rate <= 1'b0;
                                       state_pcm_period <= 1'b1;
                                       state_kf <= 1'b0;
                                   end
                   32'hAA5501BD :  begin//调频灵敏度
                                       state_pcm_rate <= 1'b0;
                                       state_pcm_period <= 1'b0;
                                       state_kf <= 1'b1;
                                   end
                   32'hAA5501AA :  begin//参数配置完毕，清空状态位
                                       state_pcm_rate <= 1'b0;
                                       state_pcm_period <= 1'b0;
                                       state_kf <= 1'b0;
                                   end
                   default :       begin
                                       state_pcm_rate <= state_pcm_rate;
                                       state_pcm_period <= state_pcm_period;
                                       state_kf <= state_kf;
                                   end
               endcase
           end
           else begin
               state_pcm_rate <= state_pcm_rate;
               state_pcm_period <= state_pcm_period;
               state_kf <= state_kf;
           end
       end
endmodule
