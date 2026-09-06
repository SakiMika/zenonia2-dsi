# Zenonia Lost Of Memories DSi v029

- Audio single-owner/deduplication: native Player.play kills matching runner fallback before starting.
- Only one native BGM owner at a time.
- Removed soundGetActiveChannels polling; one-shot END callbacks use unscaled real-time PCM duration.
- Audio timing is separated from the 1.20x guest clock.
- SFX fallback waits 90 ms for native playback before recovery.
- BGM fallback waits 300 ms for native playback before recovery.
- Guest-visible speed increased from 1.15x to 1.20x.
- Existing full-fit split and quickslot touch retained.
