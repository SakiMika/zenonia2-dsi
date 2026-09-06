#!/usr/bin/env python3
import struct, sys, collections
from pathlib import Path

def from_pack(path, wanted='binary.mod'):
    d=Path(path).read_bytes()
    if d[:8] != b'INOASPK1': raise SystemExit('bad pack magic')
    count=struct.unpack_from('<I',d,8)[0]
    for i in range(count):
        b=12+i*64
        name=d[b:b+56].split(b'\0',1)[0].decode('ascii')
        off,size=struct.unpack_from('<II',d,b+56)
        if name==wanted:
            return d[off:off+size]
    raise SystemExit(f'{wanted} not found')

if len(sys.argv)>1:
    d=Path(sys.argv[1]).read_bytes()
else:
    d=from_pack('data/inotia_assets.bin')
eh=struct.unpack_from('<16sHHIIIIIHHHHHH',d,0)
shoff,shentsz,shnum,shstrndx=eh[6],eh[11],eh[12],eh[13]
secs=[struct.unpack_from('<IIIIIIIIII',d,shoff+i*shentsz) for i in range(shnum)]
ss=secs[shstrndx]; names=d[ss[4]:ss[4]+ss[5]]
def n(o):
    e=names.find(b'\0',o); return names[o:e].decode('ascii','replace')
print('entry',hex(eh[4]))
for i,s in enumerate(secs): print(i,n(s[0]),'addr',hex(s[3]),'off',hex(s[4]),'size',hex(s[5]),'type',s[1])
for s in secs:
    if s[1]==9:
        c=collections.Counter()
        for o in range(s[4],s[4]+s[5],s[9] or 8):
            _,ri=struct.unpack_from('<II',d,o); c[ri&0xff]+=1
        print(n(s[0]),'relocations',dict(sorted(c.items())))
