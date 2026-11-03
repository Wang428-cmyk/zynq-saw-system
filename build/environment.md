# Engineering environment

* Vivado: 2021.2 (recorded tool build 3367213), target `xc7z035ffg676-2`, top module `Top_Module`.
* Vitis: 2021.2, ARM Cortex-A9 Standalone application with Xilinx lwIP/AD9361 drivers.
* Verified ARM compiler: bundled GCC 10.2.0, hard-float Cortex-A9/VFPv3 target; application uses the Vitis Debug configuration.
* Hardware handoff: `board/firmware/top_module.xsa`.
* FPGA memory and IP: original `System.bd`, `.xci`, `.coe`, `.xdc` are inside `src/pl/vivado_project.zip` and retain native filename capitalization.
* PC tools: Python 3.10 or later; standard library only for the Web bridge and verification script.
* Board network: default `192.168.2.10:4001` for control, UDP `4002` for notification.

Use `python tools/build_pl.py --work-dir E:/sw` for an isolated fresh PL build.
`tools/prepare_sources.py` restores native layouts for inspection in the IDE.
Tool reports in `build/reports/` are historical source records; new build evidence
belongs in `build/generated/`. See `docs/full_rebuild.md` for the PL-to-PS chain.

## PS build profile

`python tools/build_ps.py --work-dir E:/saw_rebuild` creates a new Cortex-A9
platform, BSP, ELF, FSBL and Zynq boot image. Inputs are the original native PS
source ZIP and the matched XSA/bit in `board/firmware/`. No historical workspace
or fixed platform/library paths are used by this build.

The profile is pinned to Vitis 2021.2, standalone 7.6, lwip211 1.6, xilffs 4.6,
RAW networking, DHCP, UART1 stdin/stdout and SD/eMMC FatFs. The complete explicit
settings are in `build/scripts/bsp_config.tcl`; generated driver assignments and
versions are recorded in `build/generated/ps/application_bsp.mss`.

The original MSS reference is the supplied PL/Vitis platform's
`ps7_cortexa9_0/standalone_ps7_cortexa9_0/bsp/system.mss`, which records the same
RAW/DHCP network buffer settings. The older reference snapshot records standalone 7.3, lwip211 1.3 and xilffs 4.4. The active build profile uses the pinned 2021.2 versions listed above.

The Python launcher stages to a new ASCII-only working directory and copies
artifacts back to the package, so the release path can contain Chinese text.
See `build/scripts/rebuild_ps.md` and `report/ps_rebuild_validation.md` for commands,
build inputs, generated outputs and validation records.
