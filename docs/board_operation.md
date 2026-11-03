# Prototype operation

## Firmware and boot medium

`board/firmware/` contains the prototype firmware set: `boot.bin`, `simulator.elf`, `top_module.bit` and `top_module.xsa`. For SD boot, copy the selected image to the boot medium as `BOOT.BIN` and select the board's SD boot mode.

Rebuilt PS images are in `build/generated/ps/`; images paired with the newly generated PL are in `build/generated/ps_from_pl/`. Each set includes its XSA/bit pair, ELF, FSBL, BIF and build receipt.

## Measurement connection

Connect the AD9361 RF path to the antenna and passive SAW sample, apply the board power supply, and connect Ethernet to the PC. Device control defaults to `192.168.2.10:4001`, with results on UDP port `4002`.

```text
python src/upper_computer/server.py --open
```

Open `http://127.0.0.1:8080`. The interface provides configuration, monitoring, coarse spectra, fine resonance readout and continuous records. The TFT provides local frequency and amplitude output.

## Measurement records

The project team has completed prototype board testing. Recorded scan and stability data are in `data/historical/`, with configuration and timestamps in each session. The technical report summarizes the prototype, RF readout and measured frequency statistics. Sensor-specific calibration relates resonance frequency to pressure/load.

Build commands are in `docs/full_rebuild.md`; UDP-v2 fields are in `docs/communication.md`.
