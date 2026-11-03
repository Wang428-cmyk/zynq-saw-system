library IEEE;
use IEEE.STD_LOGIC_1164.ALL;

entity cutter is
    port ( 
        rst         : in std_logic;
        clk_in      : in STD_LOGIC;
        data_in_I   : in std_logic_vector(11 downto 0);
        data_in_Q   : in std_logic_vector(11 downto 0);
        frame_out   : out std_logic;
        data_out_I  : out std_logic_vector(5 downto 0);
        data_out_Q  : out std_logic_vector(5 downto 0)
    );
end cutter;

architecture Behavioral of cutter is

signal frame_internal : std_logic;
signal data_I_internal : std_logic_vector(11 downto 0);
signal data_Q_internal : std_logic_vector(11 downto 0);
signal data_I_internal_delay : std_logic_vector(11 downto 0);
signal data_Q_internal_delay : std_logic_vector(11 downto 0);
signal data_out_I_s : std_logic_vector(5 downto 0);
signal data_out_Q_s : std_logic_vector(5 downto 0);

begin
    process(rst, clk_in)
    begin
        if rst = '1' then
            frame_internal <= '0';
            data_I_internal <= (others => '0');
            data_Q_internal <= (others => '0');
            data_I_internal_delay <= (others => '0');
            data_Q_internal_delay <= (others => '0');
        elsif clk_in'event and clk_in = '1' then
            frame_internal <= not frame_internal;
            data_I_internal <= data_in_I;
            data_Q_internal <= data_in_Q;
            data_I_internal_delay <= data_I_internal;
            data_Q_internal_delay <= data_Q_internal;
        end if;
    end process;

    process(rst, clk_in)
    begin
        if rst = '1' then
            data_out_I_s <= (others => '0');
            data_out_Q_s <= (others => '0');
        elsif clk_in'event and clk_in = '1' then
            if frame_internal = '1' then
                data_out_I_s <= data_I_internal(11 downto 6);
                data_out_Q_s <= data_Q_internal(11 downto 6);
            else
                data_out_I_s <= data_I_internal_delay(5 downto 0);
                data_out_Q_s <= data_Q_internal_delay(5 downto 0);
            end if;
        end if;
    end process;
    data_out_I <= data_out_I_s;
    data_out_Q <= data_out_Q_s;

    process(rst, clk_in)
    begin
        if rst = '1' then
            frame_out <= '0';
        elsif rising_edge(clk_in) then
            frame_out <= frame_internal;
        end if;
    end process;

end Behavioral;
