# Reconstructed, explicit Vitis 2021.2 application BSP profile.
# The historical native archives are preserved; this profile builds a new BSP.
# UART/RAW networking come from the supplied hardware and historical MSS files.
# Library versions below are pinned to the installed 2021.2 software repository.
bsp setosversion -ver 7.6
bsp setlib -name lwip211 -ver 1.6
bsp setlib -name xilffs -ver 4.6
bsp config stdin ps7_uart_1
bsp config stdout ps7_uart_1
bsp config api_mode RAW_API
bsp config lwip_dhcp true
bsp config dhcp_does_arp_check true
bsp config mem_size 524288
bsp config memp_n_pbuf 1024
bsp config n_rx_descriptors 512
bsp config pbuf_pool_size 16384
bsp config fs_interface 1
bsp config read_only false
bsp config enable_exfat false
bsp config use_lfn 0
bsp config use_mkfs true
bsp config enable_multi_partition false
bsp config num_logical_vol 2

