# Zenonia Lost Of Memories - Nintendo DSi port v026

Current test build of the Zenonia 2 WIPI port.

## Current layout

- Original 240x320 handset framebuffer is split exactly at Y=160.
- Top half is fitted to the full 256x192 upper LCD.
- Bottom half is fitted to the full 256x192 touch LCD.
- Quick slots on the lower LCD are touch-enabled.
- Runtime log opens only from the LOG button.

## Sound

- DS sound hardware is enabled during boot.
- WIPI media calls use the runner's real clip implementation.
- Yamaha SMAF/MMF sampled audio is decoded to PCM.
- SMAF MTR score tracks use the lightweight software synth for music/BGM.
- RIFF PCM WAV fallback remains supported.

## ROM identity

- Banner name: `Zenonia Lost Of Memories`
- Output ROM: `Zenonia Lost Of Memories.nds`
- Game code / ROM ID: `ZLOM`
- Internal 12-byte NDS header title: `ZENONIA LOST`
- WIPI program ID remains `0002C004` because that belongs to the original game runtime.

## Build

Run `BUILD_DSI_WINDOWS.bat`.

Every build writes beside the BAT and output ROM:

- `last_build.log`
- `last_build(YYYYMMDD-HHMMSS).log`
