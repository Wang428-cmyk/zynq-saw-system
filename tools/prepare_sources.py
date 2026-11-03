#!/usr/bin/env python3
"""Restore original Vivado/Vitis project hierarchy from portable ZIPs."""
from pathlib import Path
from zipfile import ZipFile
import sys
ROOT=Path(__file__).resolve().parents[1]
PAIRS=[('src/pl/vivado_project.zip','src/pl/native'),('src/ps/vitis_workspace.zip','src/ps/native')]

def restore(archive,destination):
    dest=(ROOT/destination).resolve()
    dest.mkdir(parents=True,exist_ok=True)
    with ZipFile(ROOT/archive) as z:
        for m in z.infolist():
            name=m.filename.replace('\\','/')
            target=(dest/name).resolve()
            if not target.is_relative_to(dest):raise RuntimeError('invalid archive path: '+name)
            if m.is_dir():continue
            target.parent.mkdir(parents=True,exist_ok=True)
            with z.open(m) as src, target.open('wb') as dst:
                import shutil
                shutil.copyfileobj(src,dst)
    print('Restored',archive,'=>',destination)

if __name__=='__main__':
    for pair in PAIRS:restore(*pair)
    print('Vivado project:',ROOT/'src/pl/native/Single_Channel_Simulator/Single_Channel_Simulator.xpr')
