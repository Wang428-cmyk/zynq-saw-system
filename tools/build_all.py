#!/usr/bin/env python3
"""Rebuild PL, then rebuild PS against that exact newly generated XSA/bit pair."""
import argparse
from pathlib import Path
import subprocess
import sys
from build_ps import ROOT


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--work-dir", type=Path, required=True)
    p.add_argument("--vitis-home")
    p.add_argument("--vivado-home")
    a = p.parse_args()
    pl = [sys.executable, str(ROOT / "tools/build_pl.py"), "--work-dir", str(a.work_dir)]
    if a.vivado_home:
        pl += ["--vivado-home", a.vivado_home]
    result = subprocess.run(pl)
    if result.returncode:
        return result.returncode
    ps = [sys.executable, str(ROOT / "tools/build_ps.py"), "--work-dir", str(a.work_dir),
          "--xsa", str(ROOT / "build/generated/pl/top_module.xsa"),
          "--bit", str(ROOT / "build/generated/pl/top_module.bit"),
          "--output", str(ROOT / "build/generated/ps_from_pl")]
    if a.vitis_home:
        ps += ["--vitis-home", a.vitis_home]
    return subprocess.run(ps).returncode


if __name__ == "__main__":
    raise SystemExit(main())
