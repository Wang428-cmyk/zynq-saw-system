#!/usr/bin/env python3
"""Write a Zynq bootgen BIF from explicit FSBL/bitstream/application ELF paths."""
import argparse
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--fsbl',required=True,type=Path)
p.add_argument('--bit',required=True,type=Path)
p.add_argument('--elf',required=True,type=Path)
p.add_argument('--output',default='build/generated/boot_image.bif',type=Path)
a=p.parse_args()
for x in (a.fsbl,a.bit,a.elf):
    if not x.is_file():p.error('missing '+str(x))
a.output.parent.mkdir(parents=True,exist_ok=True)
q=lambda x:str(x.resolve()).replace('\\','/')
a.output.write_text('the_ROM_image:\n{\n  [bootloader] '+q(a.fsbl)+'\n  '+q(a.bit)+'\n  '+q(a.elf)+'\n}\n',encoding='utf-8')
print('Wrote',a.output)
