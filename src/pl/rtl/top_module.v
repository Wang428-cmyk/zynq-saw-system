`timescale 1ns / 1ps

module Top_Module(
    input clk_in_p,
    input clk_in_n,
    
    inout [14:0]DDR_addr,
    inout [2:0]DDR_ba,
    inout DDR_cas_n,
    inout DDR_ck_n,
    inout DDR_ck_p,
    inout DDR_cke,
    inout DDR_cs_n,
    inout [3:0]DDR_dm,
    inout [31:0]DDR_dq,
    inout [3:0]DDR_dqs_n,
    inout [3:0]DDR_dqs_p,
    inout DDR_odt,
    inout DDR_ras_n,
    inout DDR_reset_n,
    inout DDR_we_n,
    inout FIXED_IO_ddr_vrn,
    inout FIXED_IO_ddr_vrp,
    inout [53:0]FIXED_IO_mio,
    inout FIXED_IO_ps_clk,
    inout FIXED_IO_ps_porb,
    inout FIXED_IO_ps_srstb,
    
    //AD9361 Reset
    inout           gpio_resetb,

    //AD9361 FDD Independent Control
    output          ad9361_enable,
    output          ad9361_txnrx,

    //External RF switches: high only while the PL TX window is active
    output          rf_switch_ctrl_1,
    output          rf_switch_ctrl_2,
    
    //AD9361 SPI Config
    output          spi_csn,
    output          spi_clk,
    output          spi_mosi,
    input           spi_miso,
    
    //发送数据相关引脚
    output          tx_clk_p,
    output          tx_clk_n,
    output          tx_frame_p,
    output          tx_frame_n,
    output  [5:0]   tx_data_p,
    output  [5:0]   tx_data_n,

    //AD9361 RX digital interface
    input           rx_frame_p,
    input           rx_frame_n,
    input   [5:0]   rx_data_p,
    input   [5:0]   rx_data_n
    );

    //Clock IP
    wire clk_122_88M;
    wire clk_122_88M_invert;
    wire clk_61_44M;
    wire clk_122_88M_rx_90;
    
    //AXI GPIO
    wire            rst;
    
    //GPIO EMIO
    //GPIO EMIO 一共64个引脚，所以这里为63:0，三组数据分别对应输入、输出和三态
    wire    [63:0]  gpio_i;
    wire    [63:0]  gpio_o;
    wire    [63:0]  gpio_t;
    
    //BRAM Control
    wire               bram_clk_a;
    wire [12:0]        bram_addr_a;
    wire [31:0]        bram_wrdata_a;
    wire [31:0]        bram_rddata_a;
    wire               bram_en_a;
    wire               bram_rst_a;
    wire [3:0]         bram_we_a;
    
    //PS to PL Data
    wire [20:0]         pcm_rate; 
    wire [23:0]         pcm_period;
    wire [15:0]         modu_coff;

    //SAW minimal capture control
    wire                saw_start_toggle;
    wire                saw_stop_toggle;
    wire [31:0]         saw_tx_sample_count;
    wire [31:0]         saw_guard1_sample_count;
    wire [31:0]         saw_rx_sample_count;
    wire [31:0]         saw_guard2_sample_count;
    wire [15:0]         saw_coherent_repetitions;
    wire [15:0]         saw_noncoherent_segments;
    wire [7:0]          saw_point_count;
    wire [6:0]          saw_freq_index;
    wire signed [31:0]  saw_start_phase_inc;
    wire signed [31:0]  saw_step_phase_inc;
    wire [9:0]          saw_capture_rdaddr;
    wire [31:0]         saw_capture_rddata;
    wire                saw_capture_busy;
    wire                saw_capture_done;
    wire [15:0]         saw_capture_count;
    wire [15:0]         saw_coherent_count;
    wire [15:0]         saw_noncoherent_count;
    wire [2:0]          saw_capture_state;
    wire                saw_tx_active;
    wire                saw_rx_capture_en;
    wire                saw_force_rx_on;
    wire                saw_force_tx_on;
    wire [31:0]         saw_rx_samples_last;
    wire                saw_rx_overflow;

    assign ad9361_enable = saw_force_rx_on;
    assign ad9361_txnrx = saw_force_tx_on;
    assign rf_switch_ctrl_1 = saw_force_tx_on;
    assign rf_switch_ctrl_2 = saw_force_tx_on;
    /* 时钟模块 */
    //输入时钟： 122.88MHz   差分信号
    //输出时钟1：122.88MHz   单端信号        控制基带信号分帧处理，并转为差分发送数据
    //输出时钟2：122.88MHz   反相单端信号    转为差分时钟，控制基带数据发送
    //输出时钟3：61.44MHz    单端信号        控制IQ数据产生
    clk_wiz_0 clock
      (
       // Clock out ports
       .clk_out1(clk_122_88M),         // AD9361 1R1T DATA/FB clock
       .clk_out2(clk_122_88M_invert),  // 180-degree FB clock
       .clk_out3(clk_61_44M),          // output clk_out3
       .clk_out4(clk_122_88M_rx_90),   // 90-degree RX IDDR sampling clock
       
       // Status and control signals
       .reset(0), // input reset
       .locked(),       // output locked
      // Clock in ports
       .clk_in1_p(clk_in_p),    // input clk_in1_p
       .clk_in1_n(clk_in_n));    // input clk_in1_n 
    
    /* 发送模块 */
    //产生基带发送差分数据
    Tx_Module Tx_Module_inst0(
        .clk_122_88M(clk_122_88M),
        .clk_122_88M_invert(clk_122_88M_invert),
        .clk_61_44M(clk_61_44M),
        .rst(rst),
        .saw_tx_active(saw_tx_active),
        .saw_freq_index(saw_freq_index),
        .saw_start_phase_inc(saw_start_phase_inc),
        .saw_step_phase_inc(saw_step_phase_inc),
        .pcm_rate(pcm_rate),
        .pcm_period(pcm_period),
        .modu_coff(modu_coff),
        .tx_clk_p(tx_clk_p),
        .tx_clk_n(tx_clk_n),
        .tx_frame_p(tx_frame_p),
        .tx_frame_n(tx_frame_n),
        .tx_data_p(tx_data_p),
        .tx_data_n(tx_data_n)
        );    
  
    /* 双向端口模块 */
    //根据输入dio_i，输出dio_o和三态dio_t信号，产生双向信号dio_p
    //gpio_resetb信号指AD9361的复位Reset信号，这里设置为46的原因是，ADI公司提供的no_OS源程序的parameter.h文件中
    //将AD9361的复位引脚序号定义为了100。由于PS端54个MIO引脚序号为：0-53，PL端64个引脚序号为：54-107。所以序号100
    //对应的引脚就是EMIO中第47个引脚（从0开始编号，序号100对应第101个引脚，所以EMIO引脚为101-54=47），这里EMIO引脚
    //序号从0开始编号，所以对应编号46
    ad_iobuf #(.DATA_WIDTH(1)) i_iobuf_gpio (
        .dio_t (gpio_t[46]),
        .dio_i (gpio_o[46]),
        .dio_o (gpio_i[46]),
        .dio_p (gpio_resetb)
    );   

    /* PS端数据接收模块 */
    //通过BRAM接口接收PS端数据
    Data_Receive Data_Receive_inst0(
        .bram_clk_a(bram_clk_a),
        .bram_wrdata_a(bram_wrdata_a),
        .bram_we_a(bram_we_a),
        .bram_en_a(bram_en_a),
        .rst(bram_rst_a),
        .pcm_rate(pcm_rate),        // PCM 码速率，单位Mbps  5位整数位，16位小数位 共21位
        .pcm_period(pcm_period),    // PCM 码元周期，单位 us  4位整数位，20位小数位 共24位
        .modu_coff(modu_coff)       // 调频灵敏度
        );

    Saw_Bram_Control #(
        .CAPTURE_DEPTH(1024)
    ) Saw_Bram_Control_inst0(
        .bram_clk(bram_clk_a),
        .bram_addr(bram_addr_a),
        .bram_wrdata(bram_wrdata_a),
        .bram_we(bram_we_a),
        .bram_en(bram_en_a),
        .rst(bram_rst_a),
        .capture_rddata(saw_capture_rddata),
        .capture_busy(saw_capture_busy),
        .capture_done(saw_capture_done),
        .capture_count(saw_capture_count),
        .capture_state(saw_capture_state),
        .coherent_count(saw_coherent_count),
        .noncoherent_count(saw_noncoherent_count),
        .current_freq_index(saw_freq_index),
        .force_rx_on(saw_force_rx_on),
        .force_tx_on(saw_force_tx_on),
        .rx_samples_last(saw_rx_samples_last),
        .rx_overflow(saw_rx_overflow),
        .bram_rddata(bram_rddata_a),
        .saw_start_toggle(saw_start_toggle),
        .saw_stop_toggle(saw_stop_toggle),
        .tx_sample_count(saw_tx_sample_count),
        .guard1_sample_count(saw_guard1_sample_count),
        .rx_sample_count(saw_rx_sample_count),
        .guard2_sample_count(saw_guard2_sample_count),
        .coherent_repetitions(saw_coherent_repetitions),
        .noncoherent_segments(saw_noncoherent_segments),
        .point_count(saw_point_count),
        .start_phase_inc(saw_start_phase_inc),
        .step_phase_inc(saw_step_phase_inc),
        .capture_rdaddr(saw_capture_rdaddr)
        );

    Saw_Rx_Capture #(
        .RX_BUFFER_DEPTH(65536)
    ) Saw_Rx_Capture_inst0(
        .rx_clk(clk_122_88M),
        .rx_frame_p(rx_frame_p),
        .rx_frame_n(rx_frame_n),
        .rx_data_p(rx_data_p),
        .rx_data_n(rx_data_n),
        .rst(bram_rst_a),
        .start_toggle(saw_start_toggle),
        .stop_toggle(saw_stop_toggle),
        .tx_sample_count(saw_tx_sample_count),
        .guard1_sample_count(saw_guard1_sample_count),
        .rx_sample_count(saw_rx_sample_count),
        .guard2_sample_count(saw_guard2_sample_count),
        .coherent_repetitions(saw_coherent_repetitions),
        .noncoherent_segments(saw_noncoherent_segments),
        .point_count(saw_point_count),
        .start_phase_inc(saw_start_phase_inc),
        .step_phase_inc(saw_step_phase_inc),
        .bram_clk(bram_clk_a),
        .bram_rdaddr(saw_capture_rdaddr),
        .bram_rddata(saw_capture_rddata),
        .tx_active(saw_tx_active),
        .rx_capture_en(saw_rx_capture_en),
        .force_rx_on(saw_force_rx_on),
        .force_tx_on(saw_force_tx_on),
        .busy(saw_capture_busy),
        .done(saw_capture_done),
        .capture_count(saw_capture_count),
        .coherent_count(saw_coherent_count),
        .noncoherent_count(saw_noncoherent_count),
        .rx_samples_last(saw_rx_samples_last),
        .rx_overflow(saw_rx_overflow),
        .state_debug(saw_capture_state),
        .freq_index(saw_freq_index)
        );
        
        ila_0 ila_0_inst (
        .clk(clk_122_88M),
        .probe0(saw_tx_active),
        .probe1(saw_freq_index),
        .probe2(saw_capture_busy),
        .probe3(saw_capture_done)
);
    /* Block Design顶层模块 */
    //将Block Design封装为Verilog代码
    System_wrapper System_wrapper_inst0
           (.DDR_addr(DDR_addr),
            .DDR_ba(DDR_ba),
            .DDR_cas_n(DDR_cas_n),
            .DDR_ck_n(DDR_ck_n),
            .DDR_ck_p(DDR_ck_p),
            .DDR_cke(DDR_cke),
            .DDR_cs_n(DDR_cs_n),
            .DDR_dm(DDR_dm),
            .DDR_dq(DDR_dq),
            .DDR_dqs_n(DDR_dqs_n),
            .DDR_dqs_p(DDR_dqs_p),
            .DDR_odt(DDR_odt),
            .DDR_ras_n(DDR_ras_n),
            .DDR_reset_n(DDR_reset_n),
            .DDR_we_n(DDR_we_n),
            .FIXED_IO_ddr_vrn(FIXED_IO_ddr_vrn),
            .FIXED_IO_ddr_vrp(FIXED_IO_ddr_vrp),
            .FIXED_IO_mio(FIXED_IO_mio),
            .FIXED_IO_ps_clk(FIXED_IO_ps_clk),
            .FIXED_IO_ps_porb(FIXED_IO_ps_porb),
            .FIXED_IO_ps_srstb(FIXED_IO_ps_srstb),
            .bram_addr_a_0(bram_addr_a),
            .bram_clk_a_0(bram_clk_a),
            .bram_en_a_0(bram_en_a),
            .bram_rddata_a_0(bram_rddata_a),
            .bram_rst_a_0(bram_rst_a),
            .bram_we_a_0(bram_we_a),
            .bram_wrdata_a_0(bram_wrdata_a),
            .gpio_reset_fpga_tri_o(rst),
            .gpio_i 				(gpio_i),
            .gpio_o 				(gpio_o),
            .gpio_t 				(gpio_t),
            .spi0_clk_i 			(1'b0),
            .spi0_clk_o 			(spi_clk),
            .spi0_csn_0_o 			(spi_csn),
            .spi0_csn_1_o 			(),
            .spi0_csn_2_o 			(),
            .spi0_csn_i 			(1'b1),
            .spi0_sdi_i 			(spi_miso),
            .spi0_sdo_i 			(1'b0),
            .spi0_sdo_o 			(spi_mosi));        
    
endmodule
