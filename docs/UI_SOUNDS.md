# The UI's sound palette — specification

ElevenLabs prompts ready to paste, plus production tracking.

Fourteen sounds, **a single instrument**. Consistency of timbre is what
distinguishes a *designed* sound set from an *assembled* one — more than the
quality of any individual file. Generate them in order, starting with navigation,
and judge the other thirteen against it.

## What holds for all fourteen

| Rule | Why |
|---|---|
| Mono, 48 kHz, 16-bit, WAV PCM | `audout`'s native rate on console: no resampling |
| Peak normalised to −6 dBFS | the app then applies its own volume; with no headroom it could only turn things down |
| No silence at the head | beyond a millisecond the sound detaches from the gesture and the UI feels sluggish |
| Fade-in <= 5 ms | a hard start produces a spurious click |
| Decay contained within the file | nothing is cut at the end |
| Zero reverb | it stacks up when a direction is held and turns a list into mush |

## The fourteen

| File | Role | Duration | Character |
|---|---|---|---|
| `nav_1.wav` `nav_2` `nav_3` | cursor movement | 45 ms | round, mid-range, almost subliminal. **Three variants ±2 semitones**, played in rotation: a held direction fires it ~8 times a second, and the same sample repeated sounds like a machine gun |
| `nav_bord.wav` | end of list | 55 ms | the same body, matte and lower. Definitely **not** an error sound: hitting the end of a list is a normal gesture |
| `valeur.wav` | value changed (left/right) | 40 ms | **drier** than navigation. The two gestures follow each other constantly on a settings screen; two sounds that are too close make it impossible to tell which just happened |
| `rubrique.wav` | section change (L/R) | 130 ms | a short breath + a point. It must say "the whole page changed", not "a line moved" |
| `valider.wav` | A button | 120 ms | two warm notes rising a fifth, a marimba body. The only sound in the family with any heft |
| `retour.wav` | B button | 110 ms | the exact mirror. **Generate it in the same session** as `valider`, otherwise the timbres diverge and the symmetry is lost |
| `bascule_on.wav` | switch turned on | 90 ms | a small click then a note rising a tone. The click ties the sound to the object drawn |
| `bascule_off.wav` | switch turned off | 90 ms | the same click, a descending note, mattier. The difference must be audible **with your eyes closed** |
| `ouvrir.wav` | a page is pushed | 160 ms | a rising breath, very much in the background. It overlaps `valider` (the same press): if it competes, you hear mush |
| `fermer.wav` | a page is popped | 140 ms | the same, descending, lower still |
| `connecte.wav` | session established | 420 ms (the delivered file lasts **718 ms**, 34,464 frames: measured 2026-09-11, AUDC-1) | the only **musical** sound: three rising notes in a major chord. It is the reward after seven steps of waiting — but dry, with no pad |
| `echec.wav` | connection failed | 300 ms | two low, muffled thuds. **Not an alarm**: the screen already shows a sentence explaining what to do |
| `alerte.wav` | an incident during a session | 220 ms | a warm mid-range note with a slight wobble. It plays over the game's sound: present without being shrill |
| `demarrage.wav` | launch | 500 ms | two notes rising an octave. The signature — make it **last**, once the rest has set the timbre |

## On the code side

- Module: `clients/borealis/ui/sfx.{hpp,cpp}` — events, not files: a call site says what
  *is happening*.
- Header reading: `clients/borealis/ui/wav.{h,c}`, **pure and tested offline**
  (`tests/test_wav.c`, 38 checks, including malformed headers — it is the only
  place in the repo where we read a file a human dropped in).
- Where the files live: `resources/sfx/`
  (hence `romfs:/sfx/` on console).
- Settings: `Interface -> Sounds` (a toggle) and a volume **separate** from the
  stream's.

**The sounds are muted during a stream**, and that is not a limitation endured:
the console has only one audio output, two producers play their buffers there one
AFTER the other, and beeping over a game would be worse than silence. The device
is opened on the first sound and closed after two seconds of inactivity;
`sfx::liberer()` gives it back explicitly when a session starts.
