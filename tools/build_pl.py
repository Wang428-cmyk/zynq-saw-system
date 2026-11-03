#!/usr/bin/env python3
"""Restore and build the original PL project in an isolated ASCII workspace."""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile
from build_ps import ROOT, digest, find_vitis, publish


def find_vivado(explicit):
    candidates = [Path(explicit)] if explicit else []
    if os.environ.get("XILINX_VIVADO"):
        candidates.append(Path(os.environ["XILINX_VIVADO"]))
    found = shutil.which("vivado.bat" if os.name == "nt" else "vivado")
    if found:
        candidates.append(Path(found).resolve().parents[1])
    try:
        vitis, _ = find_vitis(None)
        candidates.append(vitis.parents[1] / "Vivado/2021.2")
    except RuntimeError:
        pass
    for candidate in candidates:
        tool = candidate / "bin" / ("vivado.bat" if os.name == "nt" else "vivado")
        if tool.is_file():
            return candidate.resolve(), tool.resolve()
    raise RuntimeError("Vivado 2021.2 not found; supply --vivado-home.")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--vivado-home")
    ap.add_argument("--work-dir", type=Path, default=Path(tempfile.gettempdir()) / "saw_rebuild")
    ap.add_argument("--output", type=Path, default=ROOT / "build/generated/pl")
    a = ap.parse_args()
    home, tool = find_vivado(a.vivado_home)
    base = a.work_dir.resolve()
    if not str(base).isascii():
        raise RuntimeError("Choose an ASCII-only working directory.")
    base.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix="pl_", dir=base))
    native = stage / "src/pl/native"
    native.mkdir(parents=True)
    with zipfile.ZipFile(ROOT / "src/pl/vivado_project.zip") as z:
        for entry in z.infolist():
            target = (native / entry.filename).resolve()
            if not target.is_relative_to(native.resolve()):
                raise RuntimeError("Invalid native PL archive member.")
            if not entry.is_dir():
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(z.read(entry))
    script = stage / "build/scripts/build_pl.tcl"
    script.parent.mkdir(parents=True)
    shutil.copy2(ROOT / "build/scripts/build_pl.tcl", script)
    log = stage / "pl_build.log"
    if os.name == "nt":
        # Invoke the vendor batch entry through CMD, including quoting for a
        # tool installed under Program Files. No project paths enter this shell
        # command; the process working directory is supplied separately.
        comspec = os.environ.get("COMSPEC", r"C:\Windows\System32\cmd.exe")
        cmd = ('"' + comspec + '" /d /s /c ""' + str(tool) +
               '" -mode batch -nojournal -log vivado_build.log -source build/scripts/build_pl.tcl"')
    else:
        cmd = [str(tool), "-mode", "batch", "-nojournal", "-log", "vivado_build.log", "-source", "build/scripts/build_pl.tcl"]
    print("Vivado:", home, "\nFresh stage:", stage, "\nBuild log:", log, flush=True)
    # Drain a pipe rather than passing a disk-file handle directly to the legacy
    # Windows process. Keep the raw console bytes in the build log.
    with log.open("wb") as stream:
        process = subprocess.Popen(cmd, cwd=stage, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        with process.stdout:
            for line in iter(process.stdout.readline, b""):
                stream.write(line)
                stream.flush()
        returncode = process.wait()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    generated = stage / "build/generated"
    if returncode or not (generated / "top_module.xsa").is_file() or "SAW_PL_BUILD_SUCCESS" not in log.read_text(encoding="utf-8", errors="replace"):
        publish(log, out / "pl_build_failed.log")
        raise RuntimeError(f"PL build failed (exit {returncode}); inspect {log}")
    publish(log, out / "pl_build.log")
    for p in generated.glob("*"):
        if p.is_file():
            publish(p, out / p.name)
    xsa = out / "top_module.xsa"
    with zipfile.ZipFile(xsa) as z:
        bits = [x for x in z.namelist() if x.lower().endswith(".bit")]
        if len(bits) != 1:
            raise RuntimeError("Expected one embedded bitstream in generated XSA.")
        bit = out / "top_module.bit"
        if bit.exists():
            import stat
            bit.chmod(bit.stat().st_mode | stat.S_IWRITE)
        bit.write_bytes(z.read(bits[0]))
    receipt = {"status": "build_passed",
               "finished_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
               "tool": str(home), "stage": str(stage),
               "native_zip_sha256": digest(ROOT / "src/pl/vivado_project.zip"),
               "build_script_sha256": digest(script),
               "launcher_sha256": digest(ROOT / "tools/build_pl.py"),
               "artifacts": {p.name: digest(p) for p in out.iterdir()
                             if p.is_file() and p.name not in {"build_receipt.json", "pl_build.log"}}}
    (out / "build_receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print("PL build: PASS", flush=True)
    print("Published artifacts:", out, flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError, zipfile.BadZipFile) as error:
        print("ERROR:", error, file=sys.stderr)
        raise SystemExit(1)
