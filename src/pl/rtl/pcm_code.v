`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// ROM存储8bit
// 输出是9bit
// 例如ROM存储 00110101
// 输出        001101011
// 输出扩展了最后一位
//////////////////////////////////////////////////////////////////////////////////


module pcm_code(
    input                      clk_fs,
     input                      rst,
     input                      start,
     input	      [7:0]         data_from_pcm,   //从PCM-RAM中提取的PCM数据
     input                      code_en,
     output   reg[13:0]         ram_addr,       //pcm ram 地址
     output   reg               ena_pcm_ram,    //pcm ram使能信号
     output   reg	            pcm_code_out,         //输出的码元0,1
     input     [13:0]            max_ram_addr
    );
    
    
    reg	  [7:0]       reg_pcm;//一次取8bit PCM数据的存储寄存器
    reg	  [3:0]       shift_cnt; //8bit序号左移位数计数 最后一个码元时，提前取值
    reg               shift_flag; 
     
    always @ (posedge clk_fs)
    begin 
        if (rst == 1'b1)begin 
            ram_addr <= 14'd0;
            ena_pcm_ram <= 1'd0;
            shift_cnt <= 4'd0;
            pcm_code_out <= 1'b0;
            shift_flag <= 1'd0;
            reg_pcm <= 8'd0;
        end
        else if (rst == 1'b0 && start == 1'b1)begin
            ena_pcm_ram <= 1'd1; 
            if (shift_flag == 1'b1)begin              //从ram读新的8bit数据
                shift_flag <= 1'b0;
                if(ram_addr < max_ram_addr)begin           //循环
                    ram_addr <= ram_addr +1'b1;
                    reg_pcm <= data_from_pcm;
                end
                if(ram_addr == max_ram_addr)begin
                    ram_addr <= 14'd0;
                    reg_pcm <= data_from_pcm;
                end 
            end
            else if(code_en ==1'b1)begin
                reg_pcm	<=	reg_pcm << 1;              //左移一位，使得数据时钟处理第7bit位
                pcm_code_out <=  reg_pcm[7];
                shift_cnt <= shift_cnt + 1'd1;
                if(shift_cnt == 4'd6)
                    shift_flag <= 1'd1;
                if(shift_cnt == 4'd7)
                    shift_cnt <= 4'd0;
            end
            else begin
                ram_addr <= ram_addr;
                ena_pcm_ram <= ena_pcm_ram;
                shift_cnt <= shift_cnt;
                pcm_code_out <= pcm_code_out;
                shift_flag <= shift_flag;
                reg_pcm <= reg_pcm;
            end
        end
        else begin
            ram_addr <= ram_addr;
            ena_pcm_ram <= ena_pcm_ram;
            shift_cnt <= shift_cnt;
            pcm_code_out <= pcm_code_out;
            shift_flag <= shift_flag;
            reg_pcm <= reg_pcm;
        end
 end       
endmodule
