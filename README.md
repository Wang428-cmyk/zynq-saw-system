# Zynq SAW wireless passive sensor acquisition system

This project uses an AMD Zynq-7000 SoC (XC7Z035FFG676-2), an AD9361 RF transceiver and passive SAW resonators for RF interrogation, automatic resonance scanning and frequency readout. FPGA PL generates excitation and capture control, decodes LVDS DDR I/Q data and accumulates echo energy. ARM PS configures the RF front end, schedules coarse and fine scans, selects resonance peaks and publishes measurements. The Python/Web interface displays spectra and records, with frequency and amplitude also available on the local TFT.

The project team has completed prototype board testing. The technical report describes the system, implementation, prototype demonstration and recorded measurements.

## Technical report

`report/design_report.docx` and `report/design_report.pdf` contain the current technical document. The Chinese build guide is `docs/rebuild_quickstart_zh.md`.

## Environment

| Component | Configuration |
| --- | --- |
| SoC | XC7Z035FFG676-2 |
| RF transceiver | AD9361 |
| FPGA tools | Vivado 2021.2 with XC7Z035 synthesis/implementation license |
| Embedded tools | Vitis 2021.2, standalone Cortex-A9, lwIP and FatFs |
| PC tools | Python 3.10+ and a web browser |
| Device control | Ethernet UDP 192.168.2.10:4001 |
| Result notification | UDP port 4002 |
| Web interface | http://127.0.0.1:8080 |

## Repository structure

| Directory | Contents |
| --- | --- |
| `src/pl/` | Native Vivado source archive and RTL browsing copies |
| `src/ps/` | Native Vitis application archive and C/header browsing copies |
| `src/upper_computer/` | Python/Web application and tests |
| `build/scripts/` | Vivado/XSCT build scripts and BSP configuration |
| `build/reference/` | BSP reference configurations and provenance |
| `build/reports/` | Original synthesis and implementation reports |
| `build/generated/` | Rebuilt hardware/software outputs, logs and hashes |
| `board/firmware/` | Prototype firmware and matched hardware handoff |
| `data/historical/` | Recorded measurement sessions and scan data |
| `sim/` | Testbench, software tests and validation records |
| `skill/` | Reusable sensor-session audit skill and tests |
| `report/` | Technical report, experimental summaries and AI-assisted records |
| `docs/` | Operation, protocol, source and build documentation |
| `licenses/` | Third-party attribution and license notes |

## Build from source

Run from the repository root:

```text
python tools/verify_package.py
python tools/build_all.py --work-dir C:/saw_build
```

The full build restores the native PL archive into a new ASCII workspace, runs Vivado and exports the XSA/bit pair. Vitis then creates a platform and BSP from that pair, compiles the application and FSBL, and generates BOOT.BIN. PL outputs are in `build/generated/pl/`; the corresponding PS outputs are in `build/generated/ps_from_pl/`.

To build PS with the supplied prototype hardware handoff:

```text
python tools/build_ps.py --work-dir C:/saw_build
```

These outputs are in `build/generated/ps/`. Tool installation paths can be supplied with `--vivado-home` and `--vitis-home`. The temporary build path uses ASCII characters, while the submission directory can use a Unicode path. Each invocation creates a fresh workspace. See `docs/full_rebuild.md` and `build/scripts/rebuild_ps.md` for inputs and outputs.

## Prototype and PC operation

Place the selected BOOT.BIN on the board's boot medium and select SD boot. Connect the board, RF front end, antenna, SAW sample, power supply and Ethernet as described in `docs/board_operation.md`.

```text
python src/upper_computer/server.py --open
```

The Web interface provides scan configuration, coarse/fine spectra, resonance peak display and continuous measurement records. UDP-v2 fields are documented in `docs/communication.md`.

## Validation and data

```text
python -m unittest discover -s src/upper_computer -p test_server.py -v
python -m unittest discover -s skill/sensor-session-audit/tests -v
python skill/sensor-session-audit/scripts/audit_sessions.py --input data/historical/stability --output sim/results/session_audit
```

Build receipts contain input/output SHA-256 values. The release manifest is checked by `tools/verify_package.py`. Experimental records contain resonance frequencies, signal responses and measurement settings; frequency dispersion is summarized in the technical report. Pressure conversion uses the sensor's frequency-to-load calibration relationship.

## Source and attribution

Native Vivado/Vitis archives retain original filenames and source bytes. Browsing mirrors use lowercase filenames. `source_provenance.csv` records source mappings; `manifest_sha256.txt` identifies release files. AD9361 driver attribution is retained in the original source and `licenses/third_party_notices.md`. AI-assisted documentation and reusable validation work are summarized in `report/ai_collaboration_records.md`.
