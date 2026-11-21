# GRAIN PIANO

A firmware for the M-VAVE FM-1 by **swtdr34mz**: one instrument, a sampled grand piano that a granulator breaks
into grains. Inspired by [Tonitor](https://github.com/nzimas/tonitor) (Nuno Zimas), built on
[fm1-base](https://github.com/charlesvestal/fm1-base) and [felucca-bsp](https://github.com/hugelton/felucca-bsp).
The piano is the **Salamander Grand Piano V3** by Alexander Holm (CC-BY 3.0), the same samples as CHORD RUN's.

> Unofficial: not made, endorsed or supported by M-VAVE. Installing it replaces the FM-1's firmware and may void any
> warranty. At your own risk.

## How it sounds

Each note plays the piano itself and, at the same time, sends grains out of the same sample: tiny pieces, at the
note's pitch, taken from a point that moves through the sound (from the hammer to the tail) or stands still
(FREEZE). With FREEZE up, a held note becomes an endless cloud.

## Controls

| Control | What it does |
| --- | --- |
| Keys | The piano, with velocity. OCT- / OCT+: the octave |
| **PLAY** | **HOLD** on / off (PLAY lit). Held 0.8 s: every held note lets go |
| HOME | The main page (the cloud). Held 1 s: the preset back as it came |
| SEL | PIANO: PRESET, TONE, DYNAMICS, MIX (piano .. grains) |
| EDIT | GRAINS: SIZE, DENSITY, POSITION (hammer .. tail), SPRAY (scatter, then octaves and fifths) |
| ENV | ENVELOPE: ATTACK, RELEASE (the grains'), PEDAL (the piano's damper), FREEZE |
| LFO | MOVEMENT: RATE, DEPTH, TARGET (position, pitch, filter, pan), WOBBLE (tape) |
| FX | ROOM: ROOM (size), REVERB, DELAY, FELT (damped, warm) |
| GLO | MAGIC: REVERSE (grains played backwards), HALO (octave, fifth, oct+5th, major, minor, sus, maj9), HALO MIX, WIDTH |
| ARP | TEXTURE: TEXTURE (rain, vinyl, tape, wind, sea, room: made in the FM-1, no recordings), LEVEL, CHAR (light .. heavy), REACT (it follows your playing) |
| SEQ | SCALE (for every preset): SCALE, ROOT, KEYS (SNAP: each key to the scale's step at or below it; WHITE: the white keys in a row, the black ones silent; ALL: every key the next step), LIGHT (the roots' keys lit) |
| K1..K4 | The page's four knobs |
| PRESET | The preset: CONCERT GRAND, FELT PIANO, CLOUD, AURORA, MIDNIGHT RAIN, DREAMCORE, UNDERWATER, STARDUST. Each keeps your changes |
| SELECT / ALGORITHM | POSITION and SPRAY, on every page |
| SAVE | Keep the settings (lit: something changed) |

### The scales

CHROMATIC, MAJOR, MINOR; PHONK (phrygian), TRAP (harmonic minor), DRILL (hungarian minor), LOFI (dorian), DREAMCORE
(lydian), ARABIC (double harmonic); and some between the piano's keys: MAQAM RAST (quarter tones), OVERTONE (the
harmonic series 8..15), PELOG (the gamelan's tuning), FLOAT MAJ9 and FLOAT MIN11 (only a chord's notes), ALIEN
(Bohlen-Pierce: it repeats at an octave and a fifth, not the octave). The samples are retuned freely, so every step
sounds where it should; MIDI out sends the nearest key.

### The smart HOLD

With HOLD on, what you play stays, so both hands are free for the knobs:

- keys pressed together, or while one is still down, are one chord: they add up
- a new key with no key down starts a new chord: the old one fades out (with RELEASE)
- a held note tapped again alone lets go of that note only
- a held note pressed and kept down (0.4 s) starts a new chord from it
- at most eight notes: the oldest goes
- held notes stay through presets, pages and octave changes; HOLD off lets them go

## Building

GitHub builds it on every push (Actions > the run > Artifacts > `grainpiano-firmware`); a tag `v1.0` makes a release
with the `.fwsc`. On a computer: `git clone --recursive`, then `python3 fm1-base/tools/build.py . --host` and
`build/host/grainpiano_host tests/basic.txt build/host` for the simulator (WAVs and screenshots).

## Licence and credits

GPL-3.0-only (see `LICENSE`). The piano samples (`res/piano.bin`, from `tools/make_piano.py`): Salamander Grand Piano
V3 by Alexander Holm, CC-BY 3.0. fm1-base and felucca-bsp stay under MPL-2.0. See `CREDITS.md`.
