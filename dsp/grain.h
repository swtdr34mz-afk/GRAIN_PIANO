/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 swtdr34mz */
/* GRAIN PIANO's sound engine: one instrument, a sampled grand piano (the Salamander Grand Piano V3, nine notes, as CHORD RUN's,
 * carried in the image) that a granulator reads in grains, straight from the samples in the flash.
 * Eight voices; each plays the piano note itself (the dry piano) and sends grains out of the same sample, at the
 * note's pitch, from a position that scans through the sample or stands still (FREEZE). Then a tone filter, a soft
 * "felt", a ping-pong delay and a reverb of variable size.
 * The main loop talks to it through g_* calls (a small queue); g_render runs in the audio interrupt. */
#pragma once
#include <stdint.h>

#define G_SR 44100.0f
#define G_NV 8                          /* voices (notes at once) */
#define G_NREV 5                        /* grains played backwards at once (each a RAM buffer) */
#define G_NG 24                         /* grains at once at most, shared by the voices (CLOUD at 16: 63 % CPU) */

enum { LFO_POS, LFO_PITCH, LFO_FILTER, LFO_PAN, NLFO_TARGET };
enum { HALO_OFF, HALO_OCT, HALO_FIFTH, HALO_OCT5, HALO_MAJ, HALO_MIN, HALO_SUS, HALO_MAJ9, NHALO };
enum { PRESS_OFF, PRESS_DENSITY, PRESS_POSITION, PRESS_VIBRATO, PRESS_SWELL, NPRESS };
enum { PULSE_OFF, PULSE_4, PULSE_8, PULSE_8T, PULSE_16, PULSE_32, NPULSE };
enum { TEX_OFF, TEX_RAIN, TEX_VINYL, TEX_TAPE, TEX_WIND, TEX_SEA, TEX_ROOM, NTEX };

typedef struct {                        /* every value 0..1, but ltarget (LFO_*) */
    float tone, dyn, mix;               /* SEL: brightness, velocity response, piano (0) .. grains (1) */
    float size, dens, pos, spray;       /* EDIT: grain size, grains a second, where in the sample, scatter */
    float atk, rel, pedal, freeze;      /* ENV: attack, the grains' release, the piano's damper (pedal), scan .. frozen */
    float lrate, ldepth, wobble;        /* LFO: rate, depth, tape wobble */
    float rsize, rmix, delay, felt;     /* FX: room size, reverb mix, delay, felt (damped, warm) */
    float rev, hamt, width;             /* MAGIC: the share of grains played backwards, of grains in the HALO's
                                         * intervals, the grains' stereo width */
    float tlev, tchar, treact;          /* TEXTURE: level, character (light .. heavy), how much it follows the playing */
    float pamt, mlev, mscan;            /* MAGIC 2: what the keys' pressure does, how much; MEMORY's level, how fast its
                                         * cloud wanders through the phrase */
    float bpm, swing, pump;             /* PULSE: tempo (an outside MIDI clock wins: g_tempo), swing, the pumping */
    uint8_t ltarget, halo, tex, press, pulse;   /* LFO_*, HALO_*, TEX_*, PRESS_*, PULSE_* */
} gparams_t;

void g_init(void);
/* the piano's samples (res/piano.bin, tools/make_piano.py: "PNO1"); 1 = taken, 0 = none there */
int g_samples(const uint8_t *p, uint32_t len);
void g_params(const gparams_t *p);
/* a note: id names it (the same id again strikes it again; g_note_off lets it go), pitch in MIDI semitones, fractions
 * too (the scales between the piano's keys), velocity 0..1 */
void g_note_on(int id, float pitch, float vel);
void g_note_off(int id);
void g_press(int id, float p);          /* a key's pressure, 0..1 (PRESS_*) */
void g_tempo(float bpm);                /* an outside MIDI clock's tempo; 0: none (PULSE plays at its own BPM) */
void g_memory(int on);                  /* MEMORY: 1 the last seconds played, frozen into a cloud of their own; 0 it fades */
int g_memory_on(void);
/* the sequencer: notes and knob moves in a loop of 1 .. 16 bars at the tempo (gparams_t bpm, or an outside clock) */
enum { SEQ_STOP, SEQ_PLAY, SEQ_REC_ON, SEQ_REC_OFF, SEQ_CLEAR };
enum { CLICK_OFF, CLICK_REC, CLICK_ON, NCLICK };
void g_seq(int op);                     /* SEQ_*: play from the top / stop, record (from a bar's count-in when stopped) */
void g_seq_cfg(int bars, int quant_ticks, int click);   /* its length, the grid notes land on (0: as played), CLICK_* */
void g_seq_knob(int k, int v);          /* a knob moved: kept in the loop while it records */
int g_seq_knob_out(int *k, int *v);     /* a knob move the loop plays back: 1 = one (the app sets it) */
/* where it is: playing, recording (1 the count-in, 2 recording), the place 0..1, the bar (0 ..), the events (for the
 * screen: each one's place 0..1, its kind (0 a note's start, 1 its end, 2 a knob move) and pitch) */
int g_seq_state(int *play, int *rec, float *where, int *bar);
int g_seq_event(int i, float *where, int *kind, float *pitch);
uint32_t g_seq_fired(void);             /* notes the loop has played back since the start (the host's checks) */
void g_all_off(void);                   /* every note released (they fade) */
void g_panic(void);                     /* silence now */
void g_render(int32_t *lr, uint32_t n, float gain);
void g_shed(void);                      /* the render ran late: fewer grains for a while (audio interrupt) */

/* for the screen: the grains sounding (where in their sample 0..255, how loud 0..255, their voice), and the voices */
int g_grains(uint8_t *pos, uint8_t *lvl, uint8_t *voice, int max);
int g_voice_pos(int v, uint8_t *note, uint8_t *pos);   /* 1 = voice v sounds: its note, its scan position 0..255 */
float g_level(void);                    /* the output level, 0..1, smoothed */
