# GRAIN PIANO

A firmware for the M-VAVE FM-1 by **swtdr34mz**: one instrument, a sampled grand piano that a granulator breaks
into grains. Inspired by [Tonitor](https://github.com/nzimas/tonitor) (Nuno Zimas), built on
[fm1-base](https://github.com/charlesvestal/fm1-base) and [felucca-bsp](https://github.com/hugelton/felucca-bsp).
The piano is the **Salamander Grand Piano V3** by Alexander Holm (CC-BY 3.0), the same samples as CHORD RUN's.

**Manual:** [Deutsch](docs/HANDBUCH.md) ([PDF](docs/GRAIN_PIANO_HANDBUCH.pdf)) · [English](docs/MANUAL.md)
([PDF](docs/GRAIN_PIANO_MANUAL.pdf))

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
| **PLAY** | the LOOP from the top / stopped (lit while it plays); MIDI start, stop and clock out |
| HOME | The main page (the cloud). Held 1 s: the preset back as it came |
| SEL | PIANO: PRESET, TONE, DYNAMICS, MIX (piano .. grains) |
| EDIT | GRAINS: SIZE, DENSITY, POSITION (hammer .. tail), SPRAY (scatter, then octaves and fifths) |
| ENV | ENVELOPE: ATTACK, RELEASE (the grains'), PEDAL (the piano's damper), FREEZE |
| LFO | MOVEMENT: RATE, DEPTH, TARGET (position, pitch, filter, pan), WOBBLE (tape) |
| FX | ROOM: ROOM (size), REVERB, DELAY, FELT (damped, warm) |
| GLO | MAGIC: REVERSE (grains played backwards), HALO (octave, fifth, oct+5th, major, minor, sus, maj9), HALO MIX, WIDTH |
| EDIT again | TEXTURE: TEXTURE (rain, vinyl, tape, wind, sea, room: made in the FM-1, no recordings), LEVEL, CHAR (light .. heavy), REACT (it follows your playing) |
| GLO again | MAGIC 2: PRESSURE (what pressing a key harder does: DENSITY, POSITION, VIBRATO, SWELL), AMOUNT, MEMORY (its cloud's level), WANDER (how fast that cloud moves through the phrase) |
| REC | on the LOOP page: record (a bar counted in; again: stop recording; held 1 s: clear). Elsewhere MEMORY: the last 2 seconds played, frozen into a cloud of their own; REC again, it fades |
| LFO again | PULSE: the grains in bursts on the beat (OFF, 1/4, 1/8, 1/8T, 1/16, 1/32), BPM (70..170; an outside MIDI clock wins: EXT), SWING, PUMP (the sound ducking on each beat) |
| SEQ | LOOP: a sequencer of 1 to 16 bars recording notes and knob moves (K1..K4, the macros), played back live: LENGTH, QUANTIZE, CLICK, BPM |
| ARP | SCALE (for every preset): SCALE, ROOT, KEYS (SNAP: each key to the scale's step at or below it; WHITE: the white keys in a row, the black ones silent; ALL: every key the next step), LIGHT (the roots' keys lit) |
| K1..K4 | The page's four knobs |
| PRESET | The preset: CONCERT GRAND, FELT PIANO, CLOUD, AURORA, MIDNIGHT RAIN, DREAMCORE, UNDERWATER, STARDUST. Each keeps your changes |
| SELECT / ALGORITHM | the preset's two macros: each moves several knobs at once (CONCERT GRAND: BLOOM and SHIMMER, CLOUD: STORM and HEAVEN ...) |
| SAVE | Keep the settings (lit: something changed) |

### The scales

CHROMATIC, MAJOR, MINOR; PHONK (phrygian), TRAP (harmonic minor), DRILL (hungarian minor), LOFI (dorian), DREAMCORE
(lydian), ARABIC (double harmonic); and some between the piano's keys: MAQAM RAST (quarter tones), OVERTONE (the
harmonic series 8..15), PELOG (the gamelan's tuning), FLOAT MAJ9 and FLOAT MIN11 (only a chord's notes), ALIEN
(Bohlen-Pierce: it repeats at an octave and a fifth, not the octave). The samples are retuned freely, so every step
sounds where it should; MIDI out sends the nearest key.

## Building

GitHub builds it on every push (Actions > the run > Artifacts > `grainpiano-firmware`); a tag `v1.0` makes a release
with the `.fwsc`. On a computer: `git clone --recursive`, then `python3 fm1-base/tools/build.py . --host` and
`build/host/grainpiano_host tests/basic.txt build/host` for the simulator (WAVs and screenshots).

## Licence and credits

GPL-3.0-only (see `LICENSE`). The piano samples (`res/piano.bin`, from `tools/make_piano.py`): Salamander Grand Piano
V3 by Alexander Holm, CC-BY 3.0. fm1-base and felucca-bsp stay under MPL-2.0. See `CREDITS.md`.
