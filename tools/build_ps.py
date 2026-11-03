#!/usr/bin/env python3
"""Rebuild the ARM application, FSBL and Zynq boot image with Vitis 2021.2.

No old workspace/platform or source-code changes are required. Native sources
are read directly from the release ZIP. Xilinx tools run in a new ASCII-only
stage because their older Windows build tools do not reliably handle Unicode.
"""
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def publish(source, target):
    # Native archive files can carry Windows' read-only attribute. Only generated
    # output targets are made writable; historical input files are never changed.
    if target.exists():
        target.chmod(target.stat().st_mode | stat.S_IWRITE)
    shutil.copy2(source, target)


def find_vitis(explicit):
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    for key in ("XILINX_VITIS", "SAW_VITIS_HOME"):
        if os.environ.get(key):
            candidates.append(Path(os.environ[key]))
    located = shutil.which("xsct.bat" if os.name == "nt" else "xsct")
    if located:
        candidates.append(Path(located).resolve().parents[1])
    if os.name == "nt":
        import winreg
        for hive, key in ((winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"),
                          (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"),
                          (winreg.HKEY_CURRENT_USER, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall")):
            try:
                with winreg.OpenKey(hive, key) as parent:
                    for index in range(winreg.QueryInfoKey(parent)[0]):
                        try:
                            with winreg.OpenKey(parent, winreg.EnumKey(parent, index)) as child:
                                name = winreg.QueryValueEx(child, "DisplayName")[0]
                                if "Vitis Unified Software Platform 2021.2" in name:
                                    location = Path(winreg.QueryValueEx(child, "InstallLocation")[0])
                                    candidates.append(location / "Vitis" / "2021.2")
                        except OSError:
                            continue
            except OSError:
                continue
    for candidate in candidates:
        executable = candidate / "bin" / ("xsct.bat" if os.name == "nt" else "xsct")
        if executable.is_file():
            return candidate.resolve(), executable.resolve()
    raise RuntimeError("Vitis 2021.2 not found; supply --vitis-home <Vitis/2021.2>.")


def extract_inputs(stage, xsa_input, bit_input):
    inputs = stage / "inputs"
    source = stage / "source"
    scripts = stage / "scripts"
    for directory in (inputs, source, scripts):
        directory.mkdir()
    shutil.copy2(xsa_input, inputs / "top_module.xsa")
    shutil.copy2(bit_input, inputs / "top_module.bit")
    with zipfile.ZipFile(inputs / "top_module.xsa") as archive:
        bit_members = [name for name in archive.namelist() if name.lower().endswith(".bit")]
        if len(bit_members) != 1:
            raise RuntimeError("Expected one embedded bitstream in the input XSA.")
        embedded = archive.read(bit_members[0])
    if hashlib.sha256(embedded).hexdigest() != digest(inputs / "top_module.bit"):
        raise RuntimeError("Input XSA and external bitstream differ.")
    prefix = "PS/Simulator/src/"
    with zipfile.ZipFile(ROOT / "src/ps/vitis_workspace.zip") as archive:
        for entry in archive.infolist():
            if not entry.filename.startswith(prefix) or entry.is_dir():
                continue
            relative = entry.filename[len(prefix):]
            target = (source / relative).resolve()
            if not target.is_relative_to(source.resolve()):
                raise RuntimeError("Invalid native source archive member.")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(archive.read(entry))
    for name in ("build_ps.tcl", "bsp_config.tcl"):
        shutil.copy2(ROOT / "build/scripts" / name, scripts / name)
    return inputs, source, scripts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vitis-home")
    parser.add_argument("--work-dir", type=Path, default=Path(tempfile.gettempdir()) / "saw_rebuild")
    parser.add_argument("--output", type=Path, default=ROOT / "build/generated/ps")
    parser.add_argument("--xsa", type=Path, help="Rebuilt PL handoff; requires --bit.")
    parser.add_argument("--bit", type=Path, help="Matching rebuilt PL bitstream; requires --xsa.")
    args = parser.parse_args()
    if bool(args.xsa) != bool(args.bit):
        parser.error("--xsa and --bit must be supplied together.")
    xsa_input = (args.xsa or ROOT / "board/firmware/top_module.xsa").resolve()
    bit_input = (args.bit or ROOT / "board/firmware/top_module.bit").resolve()
    vitis, xsct = find_vitis(args.vitis_home)
    base = args.work_dir.resolve()
    if not str(base).isascii():
        raise RuntimeError("Choose an ASCII work directory, for example --work-dir E:/saw_rebuild.")
    base.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix="ps_", dir=base))
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    inputs, source, scripts = extract_inputs(stage, xsa_input, bit_input)
    log = stage / "ps_build.log"
    if os.name == "nt":
        launcher = stage / "launch.ps1"
        launcher.write_text("param([string]$Tool,[string]$Script,[string]$Stage)\n"
                            "& $Tool $Script $Stage\nexit $LASTEXITCODE\n", encoding="ascii")
        command = ["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                   "-File", str(launcher), "-Tool", str(xsct), "-Script", str(scripts / "build_ps.tcl"), "-Stage", str(stage)]
    else:
        command = [str(xsct), str(scripts / "build_ps.tcl"), str(stage)]
    print("Vitis:", vitis, flush=True)
    print("Fresh stage:", stage, flush=True)
    print("Build log:", log, flush=True)
    started = dt.datetime.now(dt.timezone.utc).isoformat()
    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run(command, cwd=stage, stdout=stream, stderr=subprocess.STDOUT,
                                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    publish(log, output / "ps_build.log")
    text = log.read_text(encoding="utf-8", errors="replace")
    if result.returncode or "SAW_BUILD_SUCCESS" not in text:
        print("\n".join(text.splitlines()[-35:]), file=sys.stderr)
        raise RuntimeError(f"Build failed (exit {result.returncode}); full log: {log}")
    required = ("saw_app.elf", "fsbl.elf", "top_module.xsa", "top_module.bit", "BOOT.BIN", "boot_image.bif")
    for name in required:
        if not (stage / "artifacts" / name).is_file():
            raise RuntimeError("Missing build artifact: " + name)
    for path in (stage / "artifacts").iterdir():
        if path.is_file():
            publish(path, output / path.name)
    # Make the published BIF portable; the build-time BIF is retained in its log stage.
    (output / "boot_image.bif").write_text(
        'the_ROM_image:\n{\n  [bootloader] fsbl.elf\n  top_module.bit\n  saw_app.elf\n}\n', encoding="ascii")
    receipt = {"status": "build_passed", "started_at_utc": started,
               "finished_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
               "tool": "Vitis/XSCT 2021.2", "vitis_home": str(vitis), "stage": str(stage),
               "inputs": {"native_ps_zip_sha256": digest(ROOT / "src/ps/vitis_workspace.zip"),
                          "xsa_sha256": digest(inputs / "top_module.xsa"),
                          "bit_sha256": digest(inputs / "top_module.bit"),
                          "build_script_sha256": digest(scripts / "build_ps.tcl"),
                          "bsp_config_sha256": digest(scripts / "bsp_config.tcl"),
                          "launcher_sha256": digest(ROOT / "tools/build_ps.py")},
               "artifacts": {name: digest(output / name) for name in required}}
    (output / "build_receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print("PS rebuild: PASS", flush=True)
    print("Published artifacts:", output, flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError, zipfile.BadZipFile) as error:
        print("ERROR:", error, file=sys.stderr)
        raise SystemExit(1)
