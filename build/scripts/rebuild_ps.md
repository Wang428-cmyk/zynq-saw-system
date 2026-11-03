# PS application, FSBL and boot-image rebuild

## Prerequisites

Use Python 3.10+ and a complete Vitis 2021.2 installation, including the Cortex-A9
toolchain and embedded software repository. No old Vitis workspace, `.xpfm`, BSP,
or `F:/CCS_v10_WP` directory is needed. The entry point reads the unmodified primary
application directly from `src/ps/vitis_workspace.zip`.

## One-command build

From the repository root:

```text
python tools/build_ps.py --work-dir E:/saw_rebuild
```

Vitis is located through `--vitis-home`, `XILINX_VITIS`, `SAW_VITIS_HOME`, PATH or
the Windows Vitis 2021.2 installation registry entry. An explicit example is:

```text
python tools/build_ps.py --vitis-home F:/Vivado2021.2/Xilinx/Vitis/2021.2 --work-dir E:/saw_rebuild
```

`tools/rebuild_ps_windows.bat` is an equivalent Windows entry point; its arguments
are passed to `build_ps.py`. The package may reside in a Chinese path: only the
temporary Xilinx-tool working directory must use ASCII characters. Every build
gets a new subdirectory; existing workspaces are never deleted or reused.

## Inputs and generated outputs

The hardware inputs are the matched `board/firmware/top_module.xsa` and
`top_module.bit`. The bit is checked against the XSA's embedded bitstream.
Rebuilding PS against this handoff does not rebuild or change the PL logic.

The builder creates a platform and application domain, applies `bsp_config.tcl`,
generates the BSP and FSBL, imports the original application/linker script,
compiles the ELF and invokes Bootgen with architecture `zynq`.

Outputs are copied to `build/generated/ps/` (override with `--output`):

* `saw_app.elf`, `fsbl.elf`, `top_module.xsa`, `top_module.bit`, `BOOT.BIN`;
* relative-path `boot_image.bif`, `application_bsp.mss`;
* `ps_build.log` and `build_receipt.json` with input/output SHA-256 values.

The generated BIF is reusable by running the following command **from its own
directory**, with the Vitis tools on PATH:

```text
bootgen -arch zynq -image boot_image.bif -o BOOT.BIN -w on
```

The BIF utility is `tools/make_boot_bif.py`. The BIF in `board/firmware/` links
the prototype application/bitstream with the generated FSBL using relative paths.

## Explicit BSP profile and source preservation

The new profile pins standalone 7.6, lwip211 1.6 and xilffs 4.6 from Vitis 2021.2.
It uses RAW networking, DHCP, UART1 console and the supplied historical MSS's
network-buffer settings. FatFs options are explicitly recorded in the Tcl.
This profile defines the application BSP for the 2021.2 source build. The application C files, linker script and native ZIP retain their original content.

Build receipts and comparison logs are summarized in `report/ps_rebuild_validation.md`. Prototype operation is described in `docs/board_operation.md`.
