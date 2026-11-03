`timescale 1ns / 1ps

module PCM_Period_Data(
    input               bram_clk,
    input               rst,
    input               bram_en,
    input               wr_state,
    input [31:0]        bram_wr_data,
    
    output    reg    [23:0]  pcm_period   // PCM 码元周期，单位 us  4位整数位，20位小数位 共24位
    );
        
    reg [3:0] receive_cnt;
    reg [31:0] wr_data;
    reg wr_state_d1;
    reg wr_state_d2;
        
    always @ (posedge bram_clk)
    begin
        if (rst == 1'b1) begin
            wr_data <= 32'd0;
            wr_state_d1 <= 1'b0;
            wr_state_d2 <= 1'b0;
        end
        else begin
            wr_state_d1 <= wr_state;
            wr_state_d2 <= wr_state_d1;
            wr_data <= bram_wr_data;
        end
    end
        
    always @ (posedge bram_clk)
    begin
        if (rst == 1'b1) begin
            receive_cnt <= 4'h0;
        end
        else begin
            if ((wr_state_d1 == 1'b1) && (wr_state_d2 == 1'b0))
                receive_cnt <= 4'h0;
            else if ((wr_state_d2 == 1'b1) && (bram_en == 1'b1))
                receive_cnt <= receive_cnt + 1'b1;
        end
    end
        
    always @ (posedge bram_clk)
    begin
        if (rst == 1'b1) begin
            pcm_period <= 24'd0;
        end
        else if ((wr_state_d2 == 1'b1) && (bram_en == 1'b1)) begin
            case (receive_cnt)
                4'h1 :  begin
                            pcm_period <= wr_data[23:0];
                        end
                default :   begin
                                pcm_period <= pcm_period;
                            end
            endcase
        end
    end 
endmodule
