#!/usr/bin/env python3
import struct, sys
from pathlib import Path

pack=Path(sys.argv[1] if len(sys.argv)>1 else 'data/inotia_assets.bin')
out=Path(sys.argv[2] if len(sys.argv)>2 else 'extracted_assets')
d=pack.read_bytes()
if d[:8] != b'INOASPK1': raise SystemExit('bad pack magic')
count=struct.unpack_from('<I',d,8)[0]
out.mkdir(parents=True,exist_ok=True)
for i in range(count):
    base=12+i*64
    name=d[base:base+56].split(b'\0',1)[0].decode('ascii')
    off,size=struct.unpack_from('<II',d,base+56)
    if off+size>len(d): raise SystemExit(f'bad entry {name}')
    (out/name).write_bytes(d[off:off+size])
print(f'extracted {count} assets -> {out}')
