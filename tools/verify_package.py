#!/usr/bin/env python3
"""Check release SHA-256 manifest and PS XSA/bitstream matching."""
import hashlib,zipfile,sys,csv,io,json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def sha(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda:f.read(1048576),b''):h.update(b)
    return h.hexdigest()

def verify():
    manifest=ROOT/'manifest_sha256.txt'
    count=0;errors=[]
    with manifest.open('r',encoding='utf-8') as f:
        for line in f:
            line=line.rstrip('\n')
            if not line:continue
            h,rel=line.split('  ',1)
            p=(ROOT/rel).resolve()
            if not p.is_relative_to(ROOT):errors.append('unsafe path '+rel);continue
            if not p.is_file():errors.append('missing '+rel);continue
            if sha(p)!=h:errors.append('sha256 mismatch '+rel)
            count+=1
    bit=ROOT/'board/firmware/top_module.bit'
    xsa=ROOT/'board/firmware/top_module.xsa'
    with zipfile.ZipFile(xsa) as z:
        xb=z.read('Top_Module.bit')
    if hashlib.sha256(xb).hexdigest()!=sha(bit):errors.append('XSA embedded bitstream differs from firmware bitstream')
    generated_receipts=0
    for folder_name in ('ps','pl','ps_from_pl'):
        folder=ROOT/'build/generated'/folder_name
        receipt_path=folder/'build_receipt.json'
        if not receipt_path.is_file():continue
        receipt=json.loads(receipt_path.read_text(encoding='utf-8'))
        if receipt.get('status')!='build_passed':
            errors.append('generated build not successful: '+folder_name)
        for name,expected in receipt.get('artifacts',{}).items():
            artifact=(folder/name).resolve()
            if not artifact.is_relative_to(folder.resolve()) or not artifact.is_file():
                errors.append('missing/unsafe generated artifact '+folder_name+'/'+name)
            elif sha(artifact)!=expected:
                errors.append('generated artifact hash mismatch '+folder_name+'/'+name)
        generated_receipts+=1
        generated_xsa=folder/'top_module.xsa'
        generated_bit=folder/'top_module.bit'
        if generated_xsa.is_file() and generated_bit.is_file():
            with zipfile.ZipFile(generated_xsa) as archive:
                names=[n for n in archive.namelist() if n.lower().endswith('.bit')]
                if len(names)!=1 or hashlib.sha256(archive.read(names[0])).hexdigest()!=sha(generated_bit):
                    errors.append('generated XSA/bitstream mismatch: '+folder_name)
    pl_folder=ROOT/'build/generated/pl'
    ps_folder=ROOT/'build/generated/ps_from_pl'
    if (pl_folder/'build_receipt.json').is_file() and (ps_folder/'build_receipt.json').is_file():
        for name in ('top_module.xsa','top_module.bit'):
            if not (pl_folder/name).is_file() or not (ps_folder/name).is_file() or sha(pl_folder/name)!=sha(ps_folder/name):
                errors.append('PL-to-PS handoff mismatch: '+name)
    print('Manifest entries checked:',count)
    print('Generated build receipts checked:',generated_receipts)
    print('XSA embedded bitstream consistency:', 'OK' if not any('bitstream' in e for e in errors) else 'FAIL')
    if errors:
        for e in errors:print(e,file=sys.stderr)
        print('Package verification: FAIL');return 1
    print('Package verification: PASS');return 0

if __name__=='__main__':sys.exit(verify())
