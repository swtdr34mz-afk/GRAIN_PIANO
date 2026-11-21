/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 swtdr34mz */
/* GRAIN PIANO's sound engine: one instrument, a sampled grand piano (the Salamander Grand Piano V3, nine notes in the
 * resource region, as CHORD RUN's) that a granulator reads in grains, straight from the samples in the flash.
 * Eight voices; each plays the piano note itself (the dry piano) and sends grains out of the same sample, at the
 * note's pitch, from a position that scans through the sample or stands still (FREEZE). Then a tone filter, a soft
 * "felt", a ping-pong delay and a reverb of variable size.
 * The main loop talks to it through g_* calls (a small queue); g_render runs in the audio interrupt. */
#pragma once
#include <stdint.h>

#define G_SR 44100.0f
#define G_NV 8                          /* voices (notes at once) */
#define G_NG 16                         /* grains at once, shared by the voices */

enum { LFO_POS, LFO_PITCH, LFO_FILTER, LFO_PAN, NLFO_TARGET };

typedef struct {                        /* every value 0..1, but ltarget (LFO_*) */
    float tone, dyn, mix;               /* SEL: brightness, velocity response, piano (0) .. grains (1) */
    float size, dens, pos, spray;       /* EDIT: grain size, grains a second, where in the sample, scatter */
    float atk, rel, pedal, freeze;      /* ENV: attack, the grains' release, the piano's damper (pedal), scan .. frozen */
    float lrate, ldepth, wobble;        /* LFO: rate, depth, tape wobble */
    float rsize, rmix, delay, felt;     /* FX: room size, reverb mix, delay, felt (damped, warm) */
    uint8_t ltarget;
} gparams_t;

void g_init(void);
/* the piano's samples (the resource region's payload, tools/make_piano.py: "PNO1"); 1 = taken, 0 = none there */
int g_samples(const uint8_t *p, uint32_t len);
void g_params(const gparams_t *p);
void g_note_on(int note, float vel);    /* MIDI note, velocity 0..1; the same note again strikes it again */
void g_note_off(int note);
void g_all_off(void);                   /* every note released (they fade) */
void g_panic(void);                     /* silence now */
void g_render(int32_t *lr, uint32_t n, float gain);

/* for the screen: the grains sounding (where in their sample 0..255, how loud 0..255, their voice), and the voices */
int g_grains(uint8_t *pos, uint8_t *lvl, uint8_t *voice, int max);
int g_voice_pos(int v, uint8_t *note, uint8_t *pos);   /* 1 = voice v sounds: its note, its scan position 0..255 */
float g_level(void);                    /* the output level, 0..1, smoothed */
