`timescale 1ns / 1ps

module saw_chirp_baseband(
    input               clk_fs,
    input               rst,
    input               active,
    input       [6:0]   freq_index,
    input signed [31:0] start_phase_inc,
    input signed [31:0] step_phase_inc,
    output      [11:0]  sin,
    output      [11:0]  cos
    );

    reg active_d;
    reg [31:0] phase_acc;
    reg signed [31:0] phase_inc;
    wire signed [31:0] indexed_phase_inc;
    wire [31:0] sin_cos;
    wire signed [17:0] phase_word;
    wire [23:0] phase_tdata;
    wire en_out;

    assign indexed_phase_inc = start_phase_inc + ($signed({1'b0, freq_index}) * step_phase_inc);
    assign phase_word = {
        {2{phase_acc[31]}},
        phase_acc[31:16]
    };
    assign phase_tdata = {{6{phase_word[17]}}, phase_word};
    assign sin = active ? sin_cos[27:16] : 12'd0;
    assign cos = active ? sin_cos[11:0]  : 12'd0;

    always @(posedge clk_fs) begin
        if (rst) begin
            active_d <= 1'b0;
            phase_acc <= 32'd0;
            phase_inc <= start_phase_inc;
        end
        else begin
            active_d <= active;

            if (active && !active_d) begin
                phase_acc <= 32'd0;
                phase_inc <= indexed_phase_inc;
            end
            else if (active) begin
                phase_acc <= phase_acc + phase_inc;
            end
            else begin
                phase_acc <= 32'd0;
                phase_inc <= indexed_phase_inc;
            end
        end
    end

    cos_ph cos_ph (
      .aclk(clk_fs),
      .s_axis_phase_tvalid(active),
      .s_axis_phase_tdata(phase_tdata),
      .m_axis_dout_tvalid(en_out),
      .m_axis_dout_tdata(sin_cos)
    );

endmodule
