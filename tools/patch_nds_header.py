#!/usr/bin/env python3
from pathlib import Path
import sys

PROFILE_GAMECODE = b'AAFA'
PROFILE_ROM_SIZE = 8 * 1024 * 1024
PROFILE_DEVICE_CAPACITY = 6
REQUIRED_ARM9_OFFSET = 0x8000
REQUIRED_HEADER_SIZE = 0x8000


def crc16_nds(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if (crc & 1) else 0)
    return crc & 0xFFFF


def u32le(d: bytearray, off: int) -> int:
    return int.from_bytes(d[off:off + 4], 'little')


def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: patch_nds_header.py <rom.nds>')

    p = Path(sys.argv[1])
    d = bytearray(p.read_bytes())
    if len(d) < 0x160:
        raise SystemExit('ROM too small')

    arm9_off = u32le(d, 0x20)
    header_size = u32le(d, 0x84)
    if arm9_off != REQUIRED_ARM9_OFFSET or header_size != REQUIRED_HEADER_SIZE:
        raise SystemExit(
            f'ROM layout mismatch: ARM9=0x{arm9_off:X}, header=0x{header_size:X}; '
            'v033 requires ndstool -h 0x8000'
        )

    if len(d) > PROFILE_ROM_SIZE:
        raise SystemExit(
            f'ROM is {len(d)} bytes, larger than EEPROM128 profile ROM size '
            f'{PROFILE_ROM_SIZE} bytes'
        )
    if len(d) < PROFILE_ROM_SIZE:
        d.extend(b'\x00' * (PROFILE_ROM_SIZE - len(d)))

    # Keep the user-facing title Zenonia.  AAFA is intentionally only the
    # cartridge save-memory profile ID used to obtain a regular EEPROM128 .sav.
    d[0x000:0x00C] = b'ZENONIA LOST'
    d[0x00C:0x010] = PROFILE_GAMECODE
    d[0x014] = PROFILE_DEVICE_CAPACITY

    crc = crc16_nds(d[:0x15E])
    d[0x15E:0x160] = crc.to_bytes(2, 'little')
    p.write_bytes(d)

    print('[HEADER] title=ZENONIA LOST code=AAFA save=EEPROM128')
    print(f'[HEADER] arm9=0x{arm9_off:04X} header=0x{header_size:04X} '
          f'rom={len(d)} devcap={d[0x14]} crc={crc:04X}')


if __name__ == '__main__':
    main()
