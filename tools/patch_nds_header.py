#!/usr/bin/env python3
from pathlib import Path
import sys

def crc16_nds(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if (crc & 1) else 0)
    return crc & 0xFFFF

def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: patch_nds_header.py <rom.nds>')
    p = Path(sys.argv[1])
    d = bytearray(p.read_bytes())
    if len(d) < 0x160:
        raise SystemExit('ROM too small')
    hdr_title = b'ZENONIA LOST'  # NDS header title is exactly 12 bytes
    d[0x000:0x00C] = hdr_title
    d[0x00C:0x010] = b'ZLOM'
    crc = crc16_nds(d[:0x15E])
    d[0x15E:0x160] = crc.to_bytes(2, 'little')
    p.write_bytes(d)
    print(f'[HEADER] title=ZENONIA LOST code=ZLOM crc={crc:04X}')

if __name__ == '__main__':
    main()
