# Recorded PS firmware set

The firmware files in this folder are copied from the **same `材料/PS` source family** in the uploaded original archive:

| Release file | Original source |
| --- | --- |
| `boot.bin` | `PS/Simulator_system/Debug/sd_card/BOOT.BIN` |
| `simulator.elf` | `PS/Simulator/Debug/Simulator.elf` |
| `top_module.bit` | `PS/Simulator/_ide/bitstream/Top_Module.bit` |
| `top_module.xsa` | `PS/Top_Module.xsa` |

The bitstream embedded in `top_module.xsa` has the same SHA-256 as `top_module.bit`. Other historical bitstream variants exist in the original PL/Vitis workspace and are retained as separate archival sources rather than substituted for this set. Detailed source-to-release hashes are in `source_provenance.csv` and `manifest_sha256.txt`.

`boot_image.bif` packages the prototype bitstream and application ELF with the FSBL generated from their matched hardware handoff in `build/generated/ps/`. Run Bootgen from this directory to use its relative paths. The supplied `boot.bin` remains the recorded prototype image.
