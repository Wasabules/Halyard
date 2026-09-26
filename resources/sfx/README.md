# Interface sounds

Drop the fourteen files described by `docs/UI_SOUNDS.md` in here.

A MISSING file makes its event silent, without breaking anything: so they can be
added one at a time and listened to as they arrive, instead of waiting for the
whole set to exist before hearing anything at all.

A file that is PRESENT but in the wrong format is logged (`[S88] sound "X" is
present but unreadable`) and then ignored — an absence and a production mistake
must not look the same.

Expected format: WAV PCM 16-bit, mono or stereo, 8 to 192 kHz (48 kHz avoids any
resampling, since it is `audout`'s native rate on console).
