/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 swtdr34mz */
/* GRAIN PIANO: one instrument, a sampled grand piano read in grains (dsp/grain.c).
 *
 *   the keys          the piano, with velocity; OCT- / OCT+ the octave
 *   HOME              the main page (the cloud); held 1 s: the preset back as it came
 *   SEL EDIT ENV LFO FX   the pages, four knobs each (K1..K4):
 *                     SEL  piano: PRESET, TONE, DYNAMICS, MIX (piano .. grains)
 *                     EDIT grains: SIZE, DENSITY, POSITION, SPRAY
 *                     ENV  envelope: ATTACK, RELEASE, PEDAL, FREEZE
 *                     LFO  movement: RATE, DEPTH, TARGET (position, pitch, filter, pan), WOBBLE
 *                     FX   room: ROOM (size), REVERB, DELAY, FELT
 *   PRESET            the preset (each keeps its own changes); SELECT: POSITION, ALGORITHM: SPRAY, on every page
 *   PLAY              HOLD on / off; held 0.8 s: every held note lets go
 *   SAVE              keep the settings (lit: something changed)
 *
 * HOLD, the smart part: with it on, the notes you play stay, so both hands are free for the knobs.
 *   - keys pressed together (or while one is still down) are one chord: they add up
 *   - a new key, with no key down, starts a new chord: the old one lets go (it fades with RELEASE)
 *   - a held note tapped again alone (a short press) lets go of that note only
 *   - a held note pressed and kept down starts a new chord from it
 *   - at most eight notes: the oldest goes
 * Turning HOLD off lets go of the held notes (but the keys still down). The held notes stay through presets, pages
 * and octaves. Part of the unity build (device/unity.c, host/plat_host.c), after the platform. */
#include "fastmath.h"
#include "grain.h"
#include "piano_data.h"              /* the samples, in the image (tools/gen_piano_h.py) */

/* ------------------------------------------------------------------ the knobs --- */
enum { K_TONE, K_DYN, K_MIX, K_SIZE, K_DENS, K_POS, K_SPRAY, K_ATK, K_REL, K_PEDAL, K_FREEZE, K_LRATE, K_LDEPTH,
       K_LTARGET, K_WOBBLE, K_RSIZE, K_RMIX, K_DELAY, K_FELT, K_REV, K_HALO, K_HAMT, K_WIDTH, K_TEX, K_TLEV, K_TCHAR,
       K_TREACT, K_PRESS, K_PAMT, K_MLEV, K_MSCAN, K_PULSE, K_BPM, K_SWING, K_PUMP, K_M1, K_M2, NK, K_PRESET = 100, K_SCALE, K_ROOT, K_KEYS, K_LIGHT, K_LEN, K_QUANT, K_CLICK, K_TEMPO, K_SOCT, K_SPOINT, K_SLSC };   /* (100..: not a preset's own) */
static const char *const K_NAME[NK] = {"TONE", "DYNAMICS", "MIX", "SIZE", "DENSITY", "POSITION", "SPRAY", "ATTACK",
    "RELEASE", "PEDAL", "FREEZE", "RATE", "DEPTH", "TARGET", "WOBBLE", "ROOM", "REVERB", "DELAY", "FELT", "REVERSE",
    "HALO", "HALO MIX", "WIDTH", "TEXTURE", "LEVEL", "CHAR", "REACT", "PRESSURE", "AMOUNT", "MEMORY", "WANDER", "PULSE",
    "BPM", "SWING", "PUMP", "MACRO 1", "MACRO 2"};
static const char *const TARGET_NAME[NLFO_TARGET] = {"POSITION", "PITCH", "FILTER", "PAN"};
static const char *const HALO_NAME[NHALO] = {"OFF", "OCTAVE", "FIFTH", "OCT+5TH", "MAJOR", "MINOR", "SUS", "MAJ9"};
static const char *const TEX_NAME[NTEX] = {"OFF", "RAIN", "VINYL", "TAPE", "WIND", "SEA", "ROOM"};
static const char *const PRESS_NAME[NPRESS] = {"OFF", "DENSITY", "POSITION", "VIBRATO", "SWELL"};
static const char *const PULSE_NAME[NPULSE] = {"OFF", "1/4", "1/8", "1/8T", "1/16", "1/32"};
/* a knob that picks from names: their list and count (0: a value 0..100) */
static const char *const *knob_names(int k, uint32_t *n)
{
    if (k == K_LTARGET) { *n = NLFO_TARGET; return TARGET_NAME; }
    if (k == K_HALO) { *n = NHALO; return HALO_NAME; }
    if (k == K_TEX) { *n = NTEX; return TEX_NAME; }
    if (k == K_PRESS) { *n = NPRESS; return PRESS_NAME; }
    if (k == K_PULSE) { *n = NPULSE; return PULSE_NAME; }
    *n = 0;
    return 0;
}

enum { PG_MAIN, PG_SEL, PG_EDIT, PG_ENV, PG_LFO, PG_FX, PG_MAGIC, PG_TEX, PG_SCALE, PG_MAGIC2, PG_PULSE, PG_LOOP, PG_SPLIT,
       NPAGE };
static const char *const PAGE_NAME[NPAGE] = {"MAIN", "PIANO", "GRAINS", "ENVELOPE", "MOVEMENT", "ROOM", "MAGIC",
                                             "TEXTURE", "SCALE", "MAGIC 2", "PULSE", "LOOP", "SPLIT"};
static const uint8_t PAGE_K[NPAGE][4] = {
    {K_POS, K_DENS, K_SIZE, K_MIX},
    {K_PRESET, K_TONE, K_DYN, K_MIX},
    {K_SIZE, K_DENS, K_POS, K_SPRAY},
    {K_ATK, K_REL, K_PEDAL, K_FREEZE},
    {K_LRATE, K_LDEPTH, K_LTARGET, K_WOBBLE},
    {K_RSIZE, K_RMIX, K_DELAY, K_FELT},
    {K_REV, K_HALO, K_HAMT, K_WIDTH},
    {K_TEX, K_TLEV, K_TCHAR, K_TREACT},
    {K_SCALE, K_ROOT, K_KEYS, K_LIGHT},
    {K_PRESS, K_PAMT, K_MLEV, K_MSCAN},
    {K_PULSE, K_TEMPO, K_SWING, K_PUMP},
    {K_LEN, K_QUANT, K_CLICK, K_TEMPO},
    {K_SOCT, K_SPOINT, K_SLSC, K_SCALE},
};

/* ------------------------------------------------------------------ the presets --- */
#define NPRESET 8
#define NPRESET_MAX 12                   /* room kept in the settings for presets to come */
#define NK_MAX 40                        /* and for knobs to come: a new one never resets the settings */
static const char *const PRESET_NAME[NPRESET] = {"CONCERT GRAND", "FELT PIANO", "CLOUD", "AURORA", "MIDNIGHT RAIN",
                                                 "DREAMCORE", "UNDERWATER", "STARDUST"};
static const char *const PRESET_SHORT[NPRESET] = {"CONCERT", "FELT", "CLOUD", "AURORA", "RAIN", "DREAM", "UNDERW.",
                                                  "STARDUST"};   /* (in a knob's cell) */
/* TONE DYN MIX | SIZE DENS POS SPRAY | ATK REL PEDAL FREEZE | RATE DEPTH TARGET WOBBLE | ROOM REVERB DELAY FELT |
 * REVERSE HALO HALOMIX WIDTH | TEXTURE LEVEL CHARACTER REACT | PRESSURE AMOUNT MEMORY WANDER | PULSE BPM(70 + n) SWING PUMP
 * (0..100, or a name's number) */
static const uint8_t PRESET_K[NPRESET][NK] = {
    {72, 70, 10, 50, 25, 15, 10, 0, 35, 45, 0, 30, 0, LFO_POS, 0, 45, 22, 0, 0, 0, HALO_OFF, 0, 50, TEX_OFF, 30, 50, 0, PRESS_OFF, 60, 70, 30, PULSE_OFF, 50, 0, 40, 0, 0},
    {45, 55, 22, 60, 30, 25, 10, 2, 50, 60, 30, 20, 0, LFO_POS, 15, 35, 30, 10, 75, 0, HALO_OFF, 0, 50, TEX_OFF, 30, 40, 0, PRESS_OFF, 60, 70, 30, PULSE_OFF, 50, 0, 40, 0, 0},
    {60, 40, 85, 80, 65, 35, 30, 45, 80, 80, 85, 15, 40, LFO_POS, 10, 85, 55, 25, 10, 0, HALO_OFF, 0, 60, TEX_OFF, 30, 50, 0, PRESS_DENSITY, 60, 70, 30, PULSE_OFF, 50, 0, 40, 0, 0},
    /* AURORA: the cloud, an octave and a fifth above it, wide, a huge room */
    {78, 40, 80, 75, 60, 40, 25, 50, 85, 80, 90, 12, 35, LFO_POS, 5, 95, 60, 20, 0, 10, HALO_OCT5, 55, 85, TEX_OFF, 20, 40, 0, PRESS_DENSITY, 60, 75, 25, PULSE_OFF, 50, 0, 40, 0, 0},
    /* MIDNIGHT RAIN: a felt piano in the rain, the filter breathing */
    {42, 50, 55, 70, 45, 30, 20, 20, 70, 75, 70, 10, 30, LFO_FILTER, 10, 70, 45, 15, 60, 15, HALO_OFF, 0, 60, TEX_RAIN, 20, 55, 30, PRESS_SWELL, 50, 65, 30, PULSE_OFF, 45, 0, 40, 0, 0},
    /* DREAMCORE: grains backwards, a major halo, a tape that leans */
    {58, 40, 75, 65, 55, 45, 35, 35, 75, 70, 60, 20, 25, LFO_PITCH, 35, 75, 50, 30, 20, 60, HALO_MAJ, 35, 70, TEX_TAPE, 14, 40, 20, PRESS_SWELL, 60, 70, 40, PULSE_OFF, 50, 20, 40, 0, 0},
    /* UNDERWATER: dark, slow waves, the filter rolling with the sea */
    {35, 40, 70, 85, 55, 40, 25, 40, 80, 80, 80, 8, 70, LFO_FILTER, 15, 80, 55, 10, 40, 20, HALO_OCT, 20, 60, TEX_SEA, 11, 60, 10, PRESS_DENSITY, 60, 70, 20, PULSE_OFF, 40, 0, 40, 0, 0},
    /* STARDUST: lo-fi maj9 dust on vinyl */
    {62, 50, 65, 45, 60, 50, 40, 15, 70, 70, 65, 25, 30, LFO_PAN, 10, 70, 50, 25, 30, 25, HALO_MAJ9, 45, 75, TEX_VINYL, 20, 45, 25, PRESS_SWELL, 50, 70, 35, PULSE_OFF, 55, 25, 40, 0, 0},
};

#define SET_MAGIC 0x33505247u            /* "GRP3": the settings record; another magic loads defaults */
typedef struct {
    uint32_t magic;
    uint8_t preset, hold;
    int8_t octave;                       /* -2 .. +2 */
    uint8_t rsv;
    uint8_t nk, np;                      /* the knobs and presets these settings know: newer ones come as they came */
    uint8_t rsv1[2];
    uint8_t k[NPRESET_MAX][NK_MAX];      /* each preset's knobs, as changed (room for more of both) */
    uint8_t rsv2[16];
} settings_t;
static settings_t set;
static uint8_t set_dirty, piano_ok, bk_hold;

static void settings_defaults(void)
{
    uint32_t i, j;
    set.magic = SET_MAGIC;
    set.preset = 0; set.hold = 0; set.octave = 0; set.rsv = 0;
    for (i = 0; i < NPRESET_MAX; i++)
        for (j = 0; j < NK_MAX; j++) set.k[i][j] = i < NPRESET && j < NK ? PRESET_K[i][j] : 0u;
    set.nk = NK; set.np = NPRESET;
}

static void settings_load(void)
{
    uint32_t i, j, bad = 0;
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || set.magic != SET_MAGIC ||
        set.preset >= NPRESET || set.hold > 1u || set.octave < -2 || set.octave > 2 || set.nk > NK || set.np > NPRESET)
        bad = 1;
    for (i = 0; !bad && i < NPRESET; i++)   /* the presets and knobs these settings did not know yet: as they came */
        for (j = 0; j < NK; j++)
            if (i >= set.np || j >= set.nk) set.k[i][j] = PRESET_K[i][j];
    if (!bad) { set.nk = NK; set.np = NPRESET; }
    for (i = 0; !bad && i < NPRESET; i++)
        for (j = 0; j < NK; j++) {
            uint32_t nn;
            knob_names((int)j, &nn);
            if (set.k[i][j] > (nn ? nn - 1u : 100u)) bad = 1;
        }
    if (!bad) {                          /* the texture levels as first made (too loud): the new ones, unless changed */
        static const uint8_t OLD[NPRESET] = {255, 255, 255, 255, 50, 30, 45, 40};
        for (i = 0; i < NPRESET; i++)
            if (set.k[i][K_TLEV] == OLD[i]) set.k[i][K_TLEV] = PRESET_K[i][K_TLEV];
    }
    if (bad) settings_defaults();
    if (set.rsv2[0] >= 15u || set.rsv2[1] > 11u || set.rsv2[2] > 2u || set.rsv2[3] > 1u)   /* the scale page's (below) */
        set.rsv2[0] = set.rsv2[1] = set.rsv2[2] = set.rsv2[3] = 0;
    if (set.rsv2[8] != 0xA5u || set.rsv2[4] > 4u || set.rsv2[5] > 3u || set.rsv2[6] > 2u || set.rsv2[7] > 100u) {
        set.rsv2[4] = 2; set.rsv2[5] = 1; set.rsv2[6] = CLICK_REC; set.rsv2[7] = 50; set.rsv2[8] = 0xA5u;   /* the loop:
                                                                           4 bars, 1/16, a click while recording, 120 */
    }
    set.hold = 0;                        /* (HOLD is gone: PLAY runs the loop) */
    if (set.rsv2[12] != 0xA5u || set.rsv2[9] > 3u || set.rsv2[10] > NKEYS - 2u || set.rsv2[11] > 15u)   /* (15 scales) */ {
        set.rsv2[9] = 0; set.rsv2[10] = 6; set.rsv2[11] = 0; set.rsv2[12] = 0xA5u;   /* SPLIT: off, at C4, the same scale */
    }
}

static uint8_t *kv(void) { return set.k[set.preset]; }

/* ------------------------------------------------------------------ the macros (SELECT, ALGORITHM) --- */
/* Two per preset, each moving several knobs at once, with a name: at 0 the preset as it is, turned up it comes to life.
 * An entry adds d x the macro (0..100) to a knob, or, with thr, sets the knob to d once the macro reaches thr (for
 * the knobs that pick a name: HALO, TEXTURE, PULSE ...). What is shown on the pages is the knobs as set, before the
 * macros. */
typedef struct { uint8_t k; int8_t d; uint8_t thr; } mac_ent_t;
typedef struct { const char *name; uint8_t n; mac_ent_t e[8]; } macro_t;
static const macro_t MACRO[NPRESET][2] = {
    {   /* CONCERT GRAND: the grand opens into a cloud; a halo of octaves and fifths rises over it */
        {"BLOOM", 7, {{K_MIX, 70, 0}, {K_DENS, 45, 0}, {K_SIZE, 25, 0}, {K_FREEZE, 60, 0}, {K_RMIX, 30, 0},
                      {K_RSIZE, 35, 0}, {K_ATK, 15, 0}}},
        {"SHIMMER", 7, {{K_HALO, HALO_OCT5, 5}, {K_HAMT, 65, 0}, {K_MIX, 50, 0}, {K_DENS, 35, 0}, {K_TONE, 15, 0},
                        {K_RMIX, 25, 0}, {K_WIDTH, 30, 0}}},
    },
    {   /* FELT PIANO: drifts into a dream; an old cassette full of dust */
        {"DREAM", 7, {{K_MIX, 55, 0}, {K_FREEZE, 55, 0}, {K_REL, 40, 0}, {K_ATK, 25, 0}, {K_RMIX, 30, 0},
                      {K_WOBBLE, 25, 0}, {K_DENS, 30, 0}}},
        {"DUSTY", 7, {{K_TEX, TEX_VINYL, 5}, {K_TLEV, 35, 0}, {K_FELT, 20, 0}, {K_WOBBLE, 50, 0}, {K_TONE, -20, 0},
                      {K_LTARGET, LFO_PITCH, 5}, {K_LDEPTH, 30, 0}}},
    },
    {   /* CLOUD: a storm of scattered grains, backwards; heaven: the cloud lifts an octave and a fifth */
        {"STORM", 7, {{K_SPRAY, 60, 0}, {K_DENS, 30, 0}, {K_SIZE, -50, 0}, {K_REV, 40, 0}, {K_LRATE, 50, 0},
                      {K_LDEPTH, 50, 0}, {K_WIDTH, 15, 0}}},
        {"HEAVEN", 6, {{K_HALO, HALO_OCT5, 5}, {K_HAMT, 65, 0}, {K_RSIZE, 15, 0}, {K_RMIX, 30, 0}, {K_TONE, 20, 0},
                       {K_FREEZE, 15, 0}}},
    },
    {   /* AURORA: the lights move; then it bursts into glittering sixteenths */
        {"N. LIGHTS", 6, {{K_LRATE, 30, 0}, {K_LDEPTH, 55, 0}, {K_SPRAY, 40, 0}, {K_HAMT, 35, 0}, {K_WIDTH, 15, 0},
                          {K_DELAY, 35, 0}}},
        {"SUPERNOVA", 7, {{K_SIZE, -45, 0}, {K_DENS, 35, 0}, {K_SPRAY, 55, 0}, {K_REV, 35, 0},
                          {K_PULSE, PULSE_16, 55}, {K_PUMP, 30, 0}, {K_TONE, 15, 0}}},
    },
    {   /* MIDNIGHT RAIN: the rain gets heavy; city lights: a minor halo, echoes, then a slow pulse */
        {"DOWNPOUR", 6, {{K_TLEV, 60, 0}, {K_TCHAR, 45, 0}, {K_LDEPTH, 30, 0}, {K_RMIX, 20, 0}, {K_DENS, 25, 0},
                         {K_SPRAY, 20, 0}}},
        {"NEON", 7, {{K_HALO, HALO_MIN, 5}, {K_HAMT, 50, 0}, {K_DELAY, 45, 0}, {K_TONE, 25, 0},
                     {K_PULSE, PULSE_8, 60}, {K_PUMP, 40, 0}, {K_FELT, -40, 0}}},
    },
    {   /* DREAMCORE: everything melts; then it glitches in swung sixteenths */
        {"MELT", 6, {{K_WOBBLE, 55, 0}, {K_REV, 35, 0}, {K_LDEPTH, 40, 0}, {K_FREEZE, 30, 0}, {K_TONE, -25, 0},
                     {K_SIZE, 20, 0}}},
        {"GLITCH", 6, {{K_SIZE, -60, 0}, {K_DENS, 35, 0}, {K_SPRAY, 50, 0}, {K_PULSE, PULSE_16, 30},
                       {K_PUMP, 50, 0}, {K_SWING, 40, 0}}},
    },
    {   /* UNDERWATER: down into the deep sea; up to the surface, the light coming through */
        {"DEEPER", 6, {{K_TONE, -30, 0}, {K_LDEPTH, 25, 0}, {K_TLEV, 40, 0}, {K_TCHAR, 35, 0}, {K_RSIZE, 15, 0},
                       {K_WOBBLE, 25, 0}}},
        {"SURFACE", 6, {{K_TONE, 45, 0}, {K_HALO, HALO_OCT, 5}, {K_HAMT, 50, 0}, {K_SPRAY, 35, 0},
                        {K_LRATE, 40, 0}, {K_WIDTH, 25, 0}}},
    },
    {   /* STARDUST: out into space; then a lo-fi beat */
        {"COSMOS", 6, {{K_RSIZE, 25, 0}, {K_RMIX, 35, 0}, {K_FREEZE, 30, 0}, {K_HAMT, 40, 0}, {K_WIDTH, 20, 0},
                       {K_DELAY, 30, 0}}},
        {"BEAT", 6, {{K_PULSE, PULSE_8, 5}, {K_PUMP, 50, 0}, {K_SWING, 30, 0}, {K_DENS, 30, 0}, {K_TLEV, 30, 0},
                     {K_TCHAR, 25, 0}}},
    },
};

static void send_params(void)
{
    uint8_t k[NK];
    gparams_t p;
    uint32_t i, m;
    for (i = 0; i < NK; i++) k[i] = kv()[i];
    for (m = 0; m < 2u; m++) {          /* the macros, on top of the knobs */
        const macro_t *mc = &MACRO[set.preset % NPRESET][m];
        int32_t v = kv()[K_M1 + m];
        for (i = 0; i < mc->n; i++) {
            const mac_ent_t *x = &mc->e[i];
            if (x->thr) { if (v >= x->thr) k[x->k] = (uint8_t)x->d; }
            else k[x->k] = (uint8_t)clamp((int32_t)k[x->k] + x->d * v / 100, 0, 100);
        }
    }
#define F(i) ((float)k[i] * 0.01f)
    p.tone = F(K_TONE); p.dyn = F(K_DYN); p.mix = F(K_MIX);
    p.size = F(K_SIZE); p.dens = F(K_DENS); p.pos = F(K_POS); p.spray = F(K_SPRAY);
    p.atk = F(K_ATK); p.rel = F(K_REL); p.pedal = F(K_PEDAL); p.freeze = F(K_FREEZE);
    p.lrate = F(K_LRATE); p.ldepth = F(K_LDEPTH); p.wobble = F(K_WOBBLE); p.ltarget = k[K_LTARGET];
    p.rsize = F(K_RSIZE); p.rmix = F(K_RMIX); p.delay = F(K_DELAY); p.felt = F(K_FELT);
    p.rev = F(K_REV); p.halo = k[K_HALO]; p.hamt = F(K_HAMT); p.width = F(K_WIDTH);
    p.tex = k[K_TEX]; p.tlev = F(K_TLEV); p.tchar = F(K_TCHAR); p.treact = F(K_TREACT);
    p.press = k[K_PRESS]; p.pamt = F(K_PAMT); p.mlev = F(K_MLEV); p.mscan = F(K_MSCAN);
    p.pulse = k[K_PULSE]; p.bpm = 70.0f + (float)set.rsv2[7];   /* (the tempo: one for all, the loop's and PULSE's) */ p.swing = F(K_SWING); p.pump = F(K_PUMP);
#undef F
    g_params(&p);
}

static void send_loop(void);

/* ------------------------------------------------------------------ the scales --- */
/* Each a list of steps in cents from the root, and the period it repeats at (an octave, 1200, but for ALIEN: an
 * octave and a fifth). The piano's samples are retuned freely, so steps between the keys are no trouble. */
typedef struct { const char *name, *shrt; uint8_t n; float period; float c[13]; } scale_t;
static const scale_t SCALES[] = {
    {"CHROMATIC", "CHROM", 12, 1200.0f, {0, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 1100}},
    {"MAJOR", "MAJOR", 7, 1200.0f, {0, 200, 400, 500, 700, 900, 1100}},
    {"MINOR", "MINOR", 7, 1200.0f, {0, 200, 300, 500, 700, 800, 1000}},
    {"PHONK", "PHONK", 7, 1200.0f, {0, 100, 300, 500, 700, 800, 1000}},          /* phrygian: the dark half step */
    {"TRAP", "TRAP", 7, 1200.0f, {0, 200, 300, 500, 700, 800, 1100}},           /* harmonic minor: the dark flute */
    {"DRILL", "DRILL", 7, 1200.0f, {0, 200, 300, 600, 700, 800, 1100}},         /* hungarian minor: more tension */
    {"LOFI", "LOFI", 7, 1200.0f, {0, 200, 300, 500, 700, 900, 1000}},           /* dorian: warm, nostalgic */
    {"DREAMCORE", "DREAM", 7, 1200.0f, {0, 200, 400, 600, 700, 900, 1100}},     /* lydian: floating */
    {"ARABIC", "ARABIC", 7, 1200.0f, {0, 100, 400, 500, 700, 800, 1100}},       /* double harmonic */
    {"MAQAM RAST", "RAST", 7, 1200.0f, {0, 200, 350, 500, 700, 900, 1050}},     /* quarter tones: the neutral 3rd, 7th */
    {"OVERTONE", "OVERT.", 8, 1200.0f, {0, 203.9f, 386.3f, 551.3f, 702.0f, 840.5f, 968.8f, 1088.3f}},   /* harmonics 8..15 */
    {"PELOG", "PELOG", 7, 1200.0f, {0, 120, 270, 540, 670, 785, 950}},          /* the gamelan's tuning */
    {"FLOAT MAJ9", "MAJ9", 5, 1200.0f, {0, 200, 400, 700, 1100}},               /* only a maj9's notes */
    {"FLOAT MIN11", "MIN11", 6, 1200.0f, {0, 200, 300, 500, 700, 1000}},        /* only a m11's notes */
    {"ALIEN", "ALIEN", 9, 1901.955f, {0, 292.6f, 438.9f, 585.2f, 877.8f, 1024.1f, 1316.7f, 1463.0f, 1755.6f}},
    /* (Bohlen-Pierce, its lambda mode: 13 equal steps of an octave and a fifth, 9 of them) */
};
#define NSCALE (sizeof SCALES / sizeof SCALES[0])
_Static_assert(sizeof SCALES / sizeof SCALES[0] == 15u, "settings_load checks the scale against 15");
enum { KEYS_SNAP, KEYS_WHITE, KEYS_ALL, NKEYMODE };
static const char *const KEYS_NAME[NKEYMODE] = {"SNAP", "WHITE", "ALL"};
static const char *const ROOT_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *const ONOFF_NAME[2] = {"OFF", "ON"};
#define SC (set.rsv2[0])                 /* the scale, the root, the keys' layout, the roots lit: for every preset */
#define ROOT (set.rsv2[1])
#define KEYMODE (set.rsv2[2])
#define LIGHT (set.rsv2[3])

/* a degree's pitch, MIDI (fractions too), from the root's C at octave base */
static float degree_pitch(float base, int32_t d)
{
    const scale_t *sc = &SCALES[SC % NSCALE];
    int32_t per = d >= 0 ? d / sc->n : -((-d + sc->n - 1) / sc->n), st = d - per * sc->n;
    return base + ((float)per * sc->period + sc->c[st]) * 0.01f;
}

/* what key k plays: 0 = nothing (a black key in WHITE); the pitch, and whether it is a root */
/* SPLIT (SEL again, after the Take 5's low split): the keys left of the split point play OCTAVE lower, in their own
 * scale if LEFT SCALE says one (SAME: the right's) */
#define SPLIT_OCT (set.rsv2[9])
#define SPLIT_KEY (set.rsv2[10] + 1u)       /* the first key of the right part (1 .. 26) */
#define SPLIT_SC (set.rsv2[11])
static int key_left(uint32_t k) { return SPLIT_OCT && k < SPLIT_KEY; }

static int key_pitch(uint32_t k, float *pitch, int *root)
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS;
    const scale_t *sc = &SCALES[(key_left(k) && SPLIT_SC ? SPLIT_SC - 1u : SC) % NSCALE];
    float base = 48.0f + (float)(ROOT % 12u) + 12.0f * (float)set.octave -   /* the root, in the octave of C3 */
                 (key_left(k) ? 12.0f * (float)SPLIT_OCT : 0.0f);
    int32_t d = 0, i;
    if (KEYMODE == KEYS_ALL || KEYMODE == KEYS_WHITE) {
        if (KEYMODE == KEYS_ALL) d = (int32_t)k;
        else {
            for (i = 0; i < 16 && WK[i] != k; i++) {}
            if (i == 16) return 0;
            d = i;
        }
        if (base + (key_left(k) ? 12.0f * (float)SPLIT_OCT : 0.0f) < 53.0f - 6.0f) base += 12.0f;   /* (the first
                                                                                         key near F3) */
        *pitch = degree_pitch(base, d);
        *root = (d % sc->n) == 0;
        return 1;
    }
    {                                    /* SNAP: the key's own pitch, to the nearest step at or below it */
        float c = (53.0f + (float)k + 12.0f * (float)set.octave - (key_left(k) ? 12.0f * (float)SPLIT_OCT : 0.0f) -
                   (base - 12.0f)) * 100.0f, best = -1e9f, x;
        int32_t per = (int32_t)(c / sc->period), bd = 0;
        for (i = -1; i <= 1; i++) {
            int32_t s2;
            for (s2 = 0; s2 < sc->n; s2++) {
                x = (float)(per + i) * sc->period + sc->c[s2];
                if (x <= c + 0.5f && x > best) { best = x; bd = s2; }
            }
        }
        *pitch = base - 12.0f + best * 0.01f;
        *root = bd == 0 && c - best < 0.5f;
        return 1;
    }
}

/* ------------------------------------------------------------------ the notes and HOLD --- */
/* A note's id: the key and the octave it was played in (128 ..), or a MIDI note (0..127). Two keys may play the same
 * pitch (SNAP), they are still two notes. */
#define NHELD 8
static struct {
    uint16_t id[NHELD];                  /* the held notes (HOLD), oldest first */
    float pitch[NHELD];
    uint8_t n;
    int16_t pending;                     /* a held note pressed alone: let go if tapped, a new chord if kept (-1 none) */
    uint8_t pending_key;
    uint32_t pending_t;
} hd = {{0}, {0}, 0, -1, 0, 0};
static uint16_t key_id[NKEYS];           /* the note each key plays while down (the octave can change meanwhile) */
static float key_pch[NKEYS];
static uint8_t key_on[NKEYS];            /* the key plays something (a black key in WHITE does not) */
static uint8_t snd_midi[512];
static float last_pitch;                 /* (the host's checks: the last key's pitch) */            /* the notes on, by id: the MIDI note sent out + 1 (0: none) */

static int note_id(uint32_t k) { return 128 + (int)k + 27 * (set.octave + 2); }
static void midi_note(int on, int note, int vel)
{
    plat_midi_send((on ? 0x09u | 0x90u << 8 : 0x08u | 0x80u << 8) | (uint32_t)note << 16 | (uint32_t)(on ? vel : 64) << 24,
                   TO_USB);
}
static void play(int id, float pitch, int vel)
{
    int mn = clamp((int32_t)(pitch + 0.5f), 0, 127);
    g_note_on(id, pitch, (float)vel * (1.0f / 127.0f));
    if (snd_midi[id & 511]) midi_note(0, snd_midi[id & 511] - 1, 0);
    snd_midi[id & 511] = (uint8_t)(mn + 1);
    midi_note(1, mn, vel);
}
static void stop(int id)
{
    g_note_off(id);
    if (snd_midi[id & 511]) midi_note(0, snd_midi[id & 511] - 1, 0);
    snd_midi[id & 511] = 0;
}

static int held_find(int id)
{
    int i;
    for (i = 0; i < hd.n; i++)
        if (hd.id[i] == id) return i;
    return -1;
}
static void held_remove(int i)
{
    int j;
    for (j = i; j + 1 < hd.n; j++) { hd.id[j] = hd.id[j + 1]; hd.pitch[j] = hd.pitch[j + 1]; }
    hd.n--;
}
static void held_drop(int i) { stop(hd.id[i]); held_remove(i); }   /* one held note lets go */
static void held_add(int id, float pitch, int vel)
{
    int i = held_find(id);
    if (i >= 0) { hd.pitch[i] = pitch; play(id, pitch, vel); return; }   /* (already held: struck again) */
    if (hd.n >= NHELD) held_drop(0);    /* the oldest goes */
    hd.id[hd.n] = (uint16_t)id; hd.pitch[hd.n] = pitch; hd.n++;
    play(id, pitch, vel);
}
static void held_clear(int keep)         /* every held note lets go, but keep (-1: none) */
{
    int i = 0;
    while (i < hd.n) {
        if (hd.id[i] == keep) { i++; continue; }
        held_drop(i);
    }
}

/* a held note's key, if it was played in the octave now (-1: not on the keys now) */
static int32_t held_key(int i)
{
    int32_t id = hd.id[i] - 128;
    if (id < 0 || id / 27 - 2 != set.octave) return -1;
    return id % 27;
}

/* a key went down; others: the keys already down */
static void key_press(uint32_t k, int vel, uint32_t others)
{
    int id = note_id(k), root;
    float pitch;
    key_on[k] = (uint8_t)key_pitch(k, &pitch, &root);
    if (!key_on[k]) return;
    last_pitch = pitch;
    key_id[k] = (uint16_t)id; key_pch[k] = pitch;
    if (!set.hold) { play(id, pitch, vel); return; }
    if (hd.pending >= 0) {               /* a second key while a held note is pressed alone: a new chord from both */
        held_clear(hd.pending);
        hd.pending = -1;
        held_add(id, pitch, vel);
        return;
    }
    if (!others) {                       /* the first key of a gesture */
        int h = held_find(id);
        if (h >= 0 && fm_fabsf(hd.pitch[h] - pitch) < 0.01f) {   /* a held note (as it sounds): tapped, it goes; kept,
                                                                   * a new chord from it (decided later) */
            hd.pending = (int16_t)id; hd.pending_key = (uint8_t)k; hd.pending_t = plat_ms();
            return;
        }
        held_clear(-1);                  /* a new chord: the old one lets go */
    }
    held_add(id, pitch, vel);
}

static void key_release(uint32_t k)
{
    if (!key_on[k]) return;
    if (set.hold) {
        if (hd.pending >= 0 && hd.pending_key == k) {   /* a held note tapped alone: it lets go */
            int i = held_find(hd.pending);
            hd.pending = -1;
            if (i >= 0) held_drop(i);
        }
        return;
    }
    stop(key_id[k]);
}

static void hold_tick(uint32_t now)      /* a held note pressed alone and kept down: a new chord from it */
{
    if (hd.pending >= 0 && now - hd.pending_t >= 400u) {
        held_clear(hd.pending);
        hd.pending = -1;
    }
}

static void hold_set(int on, uint32_t keys)
{
    uint32_t k;
    set.hold = (uint8_t)(on != 0);
    set_dirty = 1;
    hd.pending = -1;
    if (on) {                            /* the keys down now become the held chord */
        held_clear(-1);
        for (k = 0; k < NKEYS; k++)
            if ((keys >> k & 1u) && key_on[k] && hd.n < NHELD) {
                hd.id[hd.n] = key_id[k]; hd.pitch[hd.n] = key_pch[k]; hd.n++;
            }
    } else {                             /* let go, but the keys still down */
        int i = 0;
        while (i < hd.n) {
            int down = 0;
            for (k = 0; k < NKEYS; k++)
                if ((keys >> k & 1u) && key_on[k] && key_id[k] == hd.id[i]) down = 1;
            if (down) held_remove(i);
            else held_drop(i);
        }
    }
}

/* ------------------------------------------------------------------ the platform's calls --- */
void app_init(void)
{
    settings_load();
    g_init();
    piano_ok = (uint8_t)g_samples(piano_data, PIANO_DATA_LEN);
    send_params();
    send_loop();
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    g_render(lr, n, (float)master_q12 * (1.0f / 4096.0f) * 8300000.0f);
}

void app_panic(void) { g_panic(); }
void app_overload(void) { g_shed(); }   /* the chip at its limit: fewer grains, not a click */

int app_backup_playing(void) { return g_level() > 0.0005f || hd.n > 0; }
void app_backup_flush(void)
{
    if (set_dirty && plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0) set_dirty = 0;
}
void app_backup_hold(int on) { bk_hold = (uint8_t)(on != 0); }
void app_backup_restored(void)
{
    settings_load();
    send_params();
}

/* ------------------------------------------------------------------ the UI --- */
static struct {
    char msg[2][24];
    uint32_t msg_until;
    uint32_t keys, btn;
    uint8_t page;
    uint8_t vel[NKEYS];
    uint32_t home_t, play_t, rec_t;      /* HOME / PLAY / REC went down (0: not held, or the hold already acted) */
    uint32_t sel_t;                      /* SEL is held: a key pressed sets the split (as the Take 5's) */
    uint8_t sel_used;                    /* a key was pressed while it was held (so no page change) */
    uint32_t turned_at[4];               /* when K1..K4 (as shown) were last turned: lit for a moment */
    uint32_t t_draw;
    uint8_t redraw, mem;                 /* mem: MEMORY asked on (REC) */
    uint8_t prs[NKEYS];                  /* each key's pressure as last sent */
    uint32_t clk_n, clk_t0, clk_last;    /* an outside MIDI clock: ticks counted, when the count began, the last tick */
    uint8_t ext;                         /* its tempo is followed */
    uint32_t mem_t;                      /* when MEMORY was asked on */
    uint8_t intro;                       /* the splash is running */
    uint32_t intro_t0;
} ui;

static void text_cat(char *d, const char *s)
{
    while (*d) d++;
    while (*s) *d++ = *s++;
    *d = 0;
}
static void int_text(int32_t v, char *b)
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do t[n++] = (char)('0' + u % 10u); while ((u /= 10u) && n < 11);
    if (v < 0) *b++ = '-';
    while (n) *b++ = t[--n];
    *b = 0;
}

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[48];
    for (i = 0; i < 23u && a && a[i]; i++) ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++) ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1300u;
    ui.redraw = 1;
    if (ann_on()) {
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) { text_cat(t, " "); text_cat(t, ui.msg[1]); }
        ann_say(t, 0, ANN_NOW);
    }
}

/* the scale page's four, kept for every preset: their value's place and how many values */
/* and the loop's (LENGTH, QUANTIZE, CLICK) and the tempo, for every preset too */
static const char *const LEN_NAME[5] = {"1 BAR", "2 BARS", "4 BARS", "8 BARS", "16 BARS"};
static const char *const QUANT_NAME[4] = {"OFF", "1/16", "1/8", "1/8T"};
static const char *const CLICK_NAME[NCLICK] = {"OFF", "REC", "ON"};
static uint8_t *gk(int which, uint32_t *n)
{
    static const uint8_t N[11] = {NSCALE, 12, NKEYMODE, 2, 5, 4, NCLICK, 101, 4, NKEYS - 1, NSCALE + 1};
    static const uint8_t AT[11] = {0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11};   /* (rsv2[8] marks the loop's defaults) */
    uint32_t i = (uint32_t)(which - K_SCALE);
    if (i > 10u) i = 0;
    *n = N[i];
    return &set.rsv2[AT[i]];
}
static const char *gk_text(int which, int shrt)
{
    static char bpm[8];
    uint32_t n, v = *gk(which, &n) % (n ? n : 1u);
    if (which == K_SCALE) return shrt ? SCALES[v].shrt : SCALES[v].name;
    if (which == K_ROOT) return ROOT_NAME[v];
    if (which == K_KEYS) return KEYS_NAME[v];
    if (which == K_LEN) return LEN_NAME[v];
    if (which == K_QUANT) return QUANT_NAME[v];
    if (which == K_CLICK) return CLICK_NAME[v];
    if (which == K_SOCT) { static const char *const O[4] = {"OFF", "-1 OCT", "-2 OCT", "-3 OCT"}; return O[v & 3u]; }
    if (which == K_SPOINT) {             /* the split: the first key of the right part */
        static char kn[6];
        static const char *const N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        uint32_t m = 54u + v;            /* (key 1 is F#3: the left part is at least a key) */
        char o[4];
        kn[0] = 0; text_cat(kn, N[m % 12u]); int_text((int32_t)(m / 12u) - 1, o); text_cat(kn, o);
        return kn;
    }
    if (which == K_SLSC) return v == 0u ? "SAME" : shrt ? SCALES[v - 1u].shrt : SCALES[v - 1u].name;
    if (which == K_TEMPO) {              /* the tempo, or the outside clock's */
        if (ui.ext) return "EXT";
        int_text(70 + (int32_t)v, bpm);
        return bpm;
    }
    return ONOFF_NAME[v & 1u];
}
static void send_loop(void)              /* the loop's length, grid and click, to the engine */
{
    static const uint8_t BARS[5] = {1, 2, 4, 8, 16}, Q[4] = {0, 24, 48, 32};
    g_seq_cfg(BARS[set.rsv2[4] % 5u], Q[set.rsv2[5] & 3u], set.rsv2[6] % NCLICK);
}

static void value_text(int which, char *t)   /* a knob's value as shown */
{
    t[0] = 0;
    if (which == K_PRESET) { text_cat(t, PRESET_NAME[set.preset]); return; }
    if (which > K_PRESET) { text_cat(t, gk_text(which, 0)); return; }
    if (which == K_BPM) {                /* the tempo: its own, or the outside clock's */
        if (ui.ext) { text_cat(t, "EXT"); return; }
        int_text(70 + kv()[which], t);
        return;
    }
    {
        uint32_t nn;
        const char *const *nm = knob_names(which, &nn);
        if (nm) { text_cat(t, nm[kv()[which] % nn]); return; }
    }
    int_text(kv()[which], t);
}
static const char *knob_name(int which)
{
    static const char *const G[11] = {"SCALE", "ROOT", "KEYS", "LIGHT", "LENGTH", "QUANTIZE", "CLICK", "BPM", "OCTAVE",
                                      "SPLIT", "LEFT"};
    if (which == K_SCALE && ui.page == PG_SPLIT) return "RIGHT";
    return which == K_PRESET ? "PRESET" : which > K_PRESET ? G[(uint32_t)(which - K_SCALE) % 11u] : K_NAME[which];
}

static void preset_step(int32_t d)
{
    int32_t p = clamp((int32_t)set.preset + d, 0, NPRESET - 1);
    if (p == set.preset) return;
    set.preset = (uint8_t)p;
    set_dirty = 1;
    send_params();
    app_say("PRESET", PRESET_NAME[p]);
}

static void knob_turn(int which, int32_t e)
{
    char t[24];
    uint8_t *k = kv();
    int32_t v;
    uint32_t nn;
    if (which == K_PRESET) { preset_step(e > 0 ? 1 : -1); return; }
    if (which > K_PRESET) {              /* the scale page: for every preset */
        uint8_t *g = gk(which, &nn);
        v = clamp((int32_t)*g + (which == K_TEMPO ? e * (e > 1 || e < -1 ? 4 : 1) : (e > 0 ? 1 : -1)), 0, (int32_t)nn - 1);
        if (v == *g) return;
        *g = (uint8_t)v;
        set_dirty = 1;
        if (which >= K_LEN) { send_loop(); send_params(); }
        app_say(knob_name(which), gk_text(which, 0));
        return;
    }
    if (knob_names(which, &nn)) v = clamp((int32_t)k[which] + (e > 0 ? 1 : -1), 0, (int32_t)nn - 1);
    else if (which == K_BPM) v = clamp((int32_t)k[which] + e * (e > 1 || e < -1 ? 4 : 1), 0, 100);   /* 70..170 */
    else v = clamp((int32_t)k[which] + e * (e > 1 || e < -1 ? 3 : 2), 0, 100);   /* a quick turn goes further */
    if (v == k[which]) return;
    k[which] = (uint8_t)v;
    set_dirty = 1;
    send_params();
    g_seq_knob(which, v);                /* (kept in the loop while it records) */
    value_text(which, t);
    app_say(knob_name(which), t);
}

static int seq_playing(void)
{
    int pl, rc, bar;
    float w;
    g_seq_state(&pl, &rc, &w, &bar);
    return pl;
}
static void loop_play(int on)            /* the loop from the top, or stopped; MIDI start / stop out with it */
{
    g_seq(on ? SEQ_PLAY : SEQ_STOP);
    plat_midi_send(0x0Fu | (on ? 0xFAu : 0xFCu) << 8, TO_USB);
    app_say("LOOP", on ? "PLAY" : "STOP");
}

static void macro_turn(int m, int32_t e)
{
    char t[8];
    uint8_t *k = kv();
    int32_t v = clamp((int32_t)k[K_M1 + m] + e * (e > 1 || e < -1 ? 6 : 3), 0, 100);
    if (v == k[K_M1 + m]) return;
    k[K_M1 + m] = (uint8_t)v;
    set_dirty = 1;
    send_params();
    g_seq_knob(K_M1 + m, v);
    int_text(v, t);
    app_say(MACRO[set.preset % NPRESET][m].name, t);
}

static void page_go(int pg)
{
    ui.page = (uint8_t)pg;
    ui.redraw = 1;
    app_say(PAGE_NAME[pg], "");
}

static void octave_step(int32_t d)
{
    int32_t o = clamp(set.octave + d, -2, 2);
    char t[4];
    if (o == set.octave) return;
    set.octave = (int8_t)o;
    set_dirty = 1;
    t[0] = o < 0 ? '-' : '+'; t[1] = (char)('0' + (o < 0 ? -o : o)); t[2] = 0;
    app_say("OCTAVE", t);
}

static void input(void)
{
    uint32_t keys, btn = plat_buttons(), k, now = plat_ms(), lit_keys = 0, dim_keys = 0, lit_btn = 0;
    int32_t e;
    int i;
    touch_poll();
    keys = touch_keys();
    for (k = 0; k < NKEYS; k++)          /* the releases first, then the presses, each knowing the keys down before it */
        if (!(keys >> k & 1u) && (ui.keys >> k & 1u)) { key_release(k); ui.redraw = 1; }
    {
        uint32_t cur = ui.keys & keys;
        for (k = 0; k < NKEYS; k++) {    /* key 0 is F3 (MIDI 53) */
            uint32_t bit = 1u << k;
            if ((keys & bit) && !(ui.keys & bit)) {
                int v = touch_vel(k);
                if (v <= 0) v = 100;
                ui.vel[k] = (uint8_t)v;
                if (btn >> B_SEL & 1u) {   /* SEL held: this key is the split (the right part's first) */
                    if (k > 0u) {
                        set.rsv2[10] = (uint8_t)(k - 1u);
                        if (!SPLIT_OCT) SPLIT_OCT = 1;
                        set_dirty = 1;
                        app_say("SPLIT", gk_text(K_SPOINT, 0));
                    }
                    key_on[k] = 0;
                    ui.sel_used = 1;
                } else key_press(k, v, cur);
                cur |= bit;
                ui.redraw = 1;
            }
        }
    }
    ui.keys = keys;
    hold_tick(now);
    if (kv()[K_PRESS] != PRESS_OFF)      /* the keys' pressure, to their notes (as it changes) */
        for (k = 0; k < NKEYS; k++) {
            uint8_t pr = (keys >> k & 1u) && key_on[k] ? touch_pressure(k) : 0u;
            if (pr / 3u != ui.prs[k] / 3u || (pr == 0u && ui.prs[k])) {
                ui.prs[k] = pr;
                if (key_on[k]) g_press(key_id[k], (float)pr * (1.0f / 127.0f));
            }
        }
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit)) continue;
        switch (k) {
        case B_FX: page_go(PG_FX); break;
        case B_SEL: ui.sel_t = now ? now : 1u; ui.sel_used = 0; break;   /* (the page on release: see below) */
        case B_ENV: page_go(PG_ENV); break;
        case B_LFO: page_go(ui.page == PG_LFO ? PG_PULSE : PG_LFO); break;   /* again: PULSE */
        case B_EDIT: page_go(ui.page == PG_EDIT ? PG_TEX : PG_EDIT); break;   /* again: TEXTURE */
        case B_GLO: page_go(ui.page == PG_MAGIC ? PG_MAGIC2 : PG_MAGIC); break;   /* again: the second page */
        case B_ARP: page_go(PG_SCALE); break;
        case B_SEQ: page_go(PG_LOOP); break;
        case B_REC:                      /* on LOOP: record (decided on release: held 1 s, the loop is cleared);
                                          * elsewhere MEMORY: the last seconds played, frozen into a cloud */
            if (ui.page == PG_LOOP) { ui.rec_t = now ? now : 1u; break; }
            if (g_memory_on() && ui.mem) { g_memory(0); ui.mem = 0; app_say("MEMORY", "FADES"); }
            else { g_memory(1); ui.mem = 1; ui.mem_t = now; app_say("MEMORY", "FROZEN"); }
            break;
        case B_HOME: ui.home_t = now ? now : 1u; page_go(PG_MAIN); break;
        case B_PLAY: ui.play_t = now ? now : 1u; break;
        case B_OCTDN: octave_step(-1); break;
        case B_OCTUP: octave_step(1); break;
        case B_SAVE:
            if (bk_hold) break;
            if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0) { set_dirty = 0; app_say("SAVED", ""); }
            else app_say("SAVE", "FAILED");
            break;
        default: break;
        }
    }
    if (ui.sel_t && !(btn >> B_SEL & 1u)) {   /* SEL let go: PIANO, or again SPLIT (unless it set the split) */
        ui.sel_t = 0;
        if (!ui.sel_used) page_go(ui.page == PG_SEL ? PG_SPLIT : PG_SEL);
    }
    if (ui.home_t) {                     /* HOME held 1 s: the preset as it came */
        if (!(btn >> B_HOME & 1u)) ui.home_t = 0;
        else if (now - ui.home_t >= 1000u) {
            ui.home_t = 0;
            for (k = 0; k < NK; k++) kv()[k] = PRESET_K[set.preset][k];
            set_dirty = 1;
            send_params();
            app_say("RESET", PRESET_NAME[set.preset]);
        }
    }
    if (ui.play_t) {                     /* PLAY: the loop starts from the top, or stops */
        ui.play_t = 0;
        loop_play(!seq_playing());
    }
    if (ui.rec_t) {                      /* REC on LOOP: a press records (again: it stops recording); held 1 s, cleared */
        if (btn >> B_REC & 1u) {
            if (now - ui.rec_t >= 1000u) { ui.rec_t = 0; g_seq(SEQ_CLEAR); app_say("LOOP", "CLEARED"); }
        } else {
            int pl, rc, bar;
            float w;
            ui.rec_t = 0;
            g_seq_state(&pl, &rc, &w, &bar);
            if (rc) { g_seq(SEQ_REC_OFF); app_say("LOOP", "PLAYS"); }
            else {
                if (!pl) plat_midi_send(0x0Fu | 0xFAu << 8, TO_USB);   /* (it starts: MIDI start out) */
                g_seq(SEQ_REC_ON);
                app_say("LOOP", pl ? "RECORDING" : "COUNT-IN");
            }
        }
    }
    ui.btn = btn;
    for (k = 0; k < 4u; k++)             /* K1..K4: the page's four */
        if ((e = plat_enc((int)(EN_K1 + k))) != 0) {
            knob_turn(PAGE_K[ui.page][k], e);
            ui.turned_at[k] = now ? now : 1u;
        }
    if ((e = plat_enc(EN_PRESET)) != 0) preset_step(e > 0 ? 1 : -1);
    if ((e = plat_enc(EN_SELECT)) != 0) macro_turn(0, e);   /* the preset's two macros */
    if ((e = plat_enc(EN_ALGO)) != 0) macro_turn(1, e);
    /* the lights: the page's button, PLAY while HOLD is on, SAVE when something changed; the keys down bright, the
     * held notes' keys dim */
    {
        static const uint8_t PB[NPAGE] = {B_HOME, B_SEL, B_EDIT, B_ENV, B_LFO, B_FX, B_GLO, B_EDIT, B_ARP, B_GLO, B_LFO,
                                          B_SEQ, B_SEL};
        lit_btn = 1u << PB[ui.page];
        int pl, rc, bar;
        float w;
        g_seq_state(&pl, &rc, &w, &bar);
        if (pl) lit_btn |= 1u << B_PLAY | LED_PLAY_GREEN;
        if (set_dirty) lit_btn |= 1u << B_SAVE;
        if (ui.mem || rc == 2 || (rc == 1 && (now / 250u) & 1u)) lit_btn |= 1u << B_REC;   /* (blinking: the count-in) */
    }
    lit_keys = keys;
    if (SPLIT_OCT)                       /* SPLIT: the left part glows */
        for (k = 0; k < NKEYS; k++) if (key_left(k)) dim_keys |= 1u << k;
    for (i = 0; i < hd.n; i++) {
        int32_t kk = held_key(i);
        if (kk >= 0) dim_keys |= 1u << kk;
    }
    plat_lights(lit_btn, lit_keys, ((1u << NB) - 1u) & ~lit_btn, dim_keys, 2);   /* every button glows a little, to be
                                                                                 * found in the dark; the lit ones bright */
    {                                    /* LIGHT: the roots' keys a little brighter */
        uint32_t mid = 0;
        for (k = 0; LIGHT && k < NKEYS; k++) {
            float pt;
            int root;
            if (key_pitch(k, &pt, &root) && root) mid |= 1u << k;
        }
        plat_lights_mid(0, mid);
    }
}

void app_input_only(void) { input(); }

/* ------------------------------------------------------------------ drawing --- */
#define COL_IVORY RGB(250, 238, 214)
#define COL_GOLD RGB(236, 186, 98)
#define COL_EMBER RGB(214, 112, 70)
#define COL_DUSK RGB(118, 92, 150)
#define COL_NIGHT RGB(22, 20, 30)
#define COL_LINE RGB(56, 50, 66)
#define COL_LABEL RGB(150, 140, 160)

static uint16_t mix565(uint16_t a, uint16_t b, uint32_t t)   /* a .. b, t 0..255 */
{
    uint32_t ra = a >> 11, ga = (a >> 5) & 63u, ba = a & 31u, rb = b >> 11, gb = (b >> 5) & 63u, bb = b & 31u;
    uint32_t r = (ra * (255u - t) + rb * t) / 255u, g = (ga * (255u - t) + gb * t) / 255u,
             bl = (ba * (255u - t) + bb * t) / 255u;
    return (uint16_t)(r << 11 | g << 5 | bl);
}

static uint16_t dim565(uint16_t c, uint32_t a);

/* ---- the facelift's pieces: a soft gradient behind, the battery, gauges in rings */
static uint16_t lerp565(uint16_t a, uint16_t b, int32_t t, int32_t n)   /* a .. b, t of n */
{
    if (n <= 0) return b;
    return mix565(a, b, (uint32_t)(clamp(t, 0, n) * 255 / n));
}
static void bg_rows(int32_t h, uint16_t top, uint16_t bot)   /* the canvas: top .. bot, row by row */
{
    int32_t y;
    for (y = 0; y < h; y++) cv_rect(0, y, 240, 1, lerp565(top, bot, y, h - 1));
}
static void frame_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)   /* an outline, its corners off */
{
    cv_rect(x + 1, y, w - 2, 1, c); cv_rect(x + 1, y + h - 1, w - 2, 1, c);
    cv_rect(x, y + 1, 1, h - 2, c); cv_rect(x + w - 1, y + 1, 1, h - 2, c);
}

/* the battery, at x, y (an icon 22 x 10, filled as it is charged, red when nearly empty): from plat_battery_raw's reading, between the
 * stock thresholds' ends (about 520 empty .. 605 full); with USB, a bolt */
static int battery_pct(void)
{
    int b = plat_battery();
    int32_t raw = plat_battery_raw();
#ifdef PLAT_HOST
    if (getenv("GP_BATT")) return atoi(getenv("GP_BATT"));   /* (the simulator's previews: a level, 101 USB) */
#endif
    if (b == 4) return 101;              /* (USB) */
    if (raw > 0) return clamp((raw - 520) * 100 / (605 - 520), 0, 100);
    return b >= 0 ? b * 33 + 1 : -1;
}
static void draw_battery(int32_t x, int32_t y)
{
    int pct = battery_pct();
    uint16_t c = pct < 0 ? COL_LINE : pct <= 15 ? RGB(232, 76, 60) : pct > 100 ? COL_IVORY : COL_GOLD;
    frame_rect(x, y, 20, 10, pct < 0 ? COL_LINE : COL_LABEL);
    cv_rect(x + 20, y + 3, 2, 4, pct < 0 ? COL_LINE : COL_LABEL);
    if (pct > 100) {                     /* USB: a bolt */
        static const int8_t B[10][2] = {{11, 1}, {10, 2}, {9, 3}, {8, 4}, {7, 5}, {8, 5}, {9, 5}, {10, 5}, {11, 6}, {10, 7}};
        int i;
        cv_rect(x + 2, y + 2, 16, 6, mix565(C_BLACK, COL_GOLD, 70));
        for (i = 0; i < 10; i++) cv_rect(x + B[i][0] - 1, y + B[i][1], 2, 1, COL_IVORY);
        cv_rect(x + 8, y + 8, 2, 1, COL_IVORY);
        return;
    }
    if (pct >= 0) {
        int32_t w = (16 * pct + 50) / 100;
        cv_rect(x + 2, y + 2, w, 6, c);
        if (w > 1) cv_rect(x + 2, y + 2, w, 1, mix565(c, COL_IVORY, 120));   /* (a highlight on top) */
    }
}

static void draw_header(uint32_t now)
{
    cv_begin(240, 32, C_BLACK);
    bg_rows(31, RGB(34, 24, 44), C_BLACK);
    if (now < ui.msg_until) {
        int32_t x = cv_text(10, 7, &FONT_B, ui.msg[0], COL_IVORY);
        cv_text(x + 8, 7, &FONT_B, ui.msg[1], COL_GOLD);
    } else {
        int32_t px, pw;
        cv_rect(10, 12, 3, 3, COL_GOLD); cv_rect(11, 11, 1, 5, COL_GOLD); cv_rect(9, 13, 5, 1, COL_GOLD);   /* a diamond */
        cv_text(19, 7, &FONT_B, PRESET_NAME[set.preset], COL_IVORY);
        pw = text_w(&FONT_T, PAGE_NAME[ui.page]) + 10;   /* the page, in a chip */
        px = 176 - pw;
        frame_rect(px, 8, pw, 15, mix565(C_BLACK, COL_GOLD, 110));
        cv_text(px + 5, 10, &FONT_T, PAGE_NAME[ui.page], COL_GOLD);
        if (g_memory_on()) cv_text(px - 6 - text_w(&FONT_T, "MEM"), 10, &FONT_T, "MEM", COL_EMBER);
    }
    draw_battery(214, 11);
    cv_rect(0, 31, 240, 1, mix565(C_BLACK, COL_GOLD, 60));
    cv_blit(0, 0);
}

/* the cloud: the sample from its hammer (left) to its tail (right), each voice's position a line, each grain a dot,
 * brighter as it sounds louder, its height the voice's note */
/* light, added: a pixel brightened by (r, g, b) out of 255 (light on light grows brighter, as it does) */
static inline void light(int32_t x, int32_t y, uint32_t r, uint32_t g, uint32_t b)
{
    uint16_t *px;
    uint32_t c, cr, cg, cb;
    y += cv_oy;
    if ((uint32_t)x >= cv_w || y < cv_y0 || y >= cv_y1) return;
    px = &cv_px[(uint32_t)y * cv_w + (uint32_t)x];
    c = swap16(*px);
    cr = ((c >> 11) << 3) + r; cg = (((c >> 5) & 63u) << 2) + g; cb = ((c & 31u) << 3) + b;
    if (cr > 255u) cr = 255u;
    if (cg > 255u) cg = 255u;
    if (cb > 255u) cb = 255u;
    *px = swap16(RGB(cr, cg, cb));
}
/* a soft point of light at a sub-pixel place (x, y in 1/16 px): its brightness shared among the four pixels around
 * it (no jumping from pixel to pixel), warm gold, whiter as it is brighter */
static void glow(int32_t x16, int32_t y16, uint32_t lv)
{
    int32_t x = x16 >> 4, y = y16 >> 4;
    uint32_t fx = (uint32_t)(x16 & 15), fy = (uint32_t)(y16 & 15), w[4], i;
    uint32_t r = 255u, g = 170u + (lv > 200u ? 70u : lv * 70u / 200u), b = 70u + (lv > 180u ? 140u : lv * 140u / 180u);
    w[0] = (16u - fx) * (16u - fy); w[1] = fx * (16u - fy); w[2] = (16u - fx) * fy; w[3] = fx * fy;
    for (i = 0; i < 4u; i++) {
        uint32_t a = w[i] * lv >> 8;     /* 0..255 */
        if (a) light(x + (int32_t)(i & 1u), y + (int32_t)(i >> 1), r * a >> 8, g * a >> 8, b * a >> 8);
    }
}

/* the dust: each grain sheds fine specks of light that rise, drift and glow out */
#define NDUST 420
static struct { int32_t x, y, vx, vy; uint16_t life, max; uint8_t lv; } dust[NDUST];   /* (1/16 px, frames) */
static uint32_t dust_rng = 0x9E3779B9u;
static inline uint32_t drnd(void) { dust_rng ^= dust_rng << 13; dust_rng ^= dust_rng >> 17; dust_rng ^= dust_rng << 5; return dust_rng; }
static void dust_shed(int32_t x16, int32_t y16, uint32_t lv, int n)
{
    static uint32_t at;
    while (n-- > 0) {
        uint32_t i, r = drnd();
        for (i = 0; i < NDUST && dust[at].life; i++) at = (at + 1u) % NDUST;
        if (dust[at].life) return;       /* (full) */
        dust[at].x = x16 + (int32_t)(r & 63u) - 32;
        dust[at].y = y16 + (int32_t)((r >> 6) & 63u) - 32;
        dust[at].vx = (int32_t)((r >> 12) & 31u) - 15;         /* (sideways) */
        dust[at].vy = -3 - (int32_t)((r >> 17) & 7u);           /* (rising) */
        dust[at].max = dust[at].life = (uint16_t)(20u + ((r >> 20) & 31u));
        dust[at].lv = (uint8_t)(90u + (lv * ((r >> 25) & 127u) >> 8));
    }
}
static uint32_t dust_live;               /* specks still glowing (the screen keeps moving till they are out) */
static void dust_draw(int32_t ytop)
{
    uint32_t i;
    dust_live = 0;
    for (i = 0; i < NDUST; i++) {
        uint32_t t, a;
        if (!dust[i].life) continue;
        dust_live++;
        dust[i].x += dust[i].vx; dust[i].y += dust[i].vy;
        dust[i].vx += (int32_t)(drnd() % 3u) - 1;              /* (a little air) */
        if (dust[i].y < ytop << 4) dust[i].life = 1;
        t = dust[i].life * 256u / dust[i].max;                 /* 256 .. 0 */
        a = (dust[i].lv * (t < 128u ? t * 2u : 256u - (t - 128u) / 2u)) >> 8;   /* (in quickly, out slowly) */
        glow(dust[i].x, dust[i].y, a);
        dust[i].life--;
    }
}

static void draw_cloud(int32_t y0, int32_t h)
{
    uint8_t gp[G_NG], gl[G_NG], gv[G_NG], note, pos, shown = 60;
    int n = g_grains(gp, gl, gv, G_NG), i;
    int32_t x0 = 10, w = 220, base = y0 + h - 10, x;
    uint32_t t = plat_ms();
    for (i = 0; i < G_NV; i++)           /* the outline of the note sounding (else C4's) */
        if (g_voice_pos(i, &note, &pos)) { shown = note; break; }
    for (x = 0; x < w; x++) {            /* the sample, hammer to tail: a soft silhouette mirrored on its floor */
        int e = g_env(shown, (int)(x * 255 / w)), y;
        int32_t up16 = e * (h - 30) * 16 / 255, up = up16 >> 4, fr = up16 & 15;
        for (y = 0; y < up; y++) {       /* (brighter towards its edge, as light on a crest) */
            uint32_t a = 18u + (uint32_t)y * 46u / (uint32_t)(up > 0 ? up : 1);
            light(x0 + x, base - 1 - y, a, a * 2u / 3u, a / 4u);
        }
        light(x0 + x, base - 1 - up, (uint32_t)fr * 6u, (uint32_t)fr * 4u, (uint32_t)fr);   /* (its edge, smooth) */
        for (y = 0; y < up / 4; y++) {   /* the reflection, fading */
            uint32_t a = 26u - (uint32_t)y * 26u / (uint32_t)(up / 4 > 0 ? up / 4 : 1);
            light(x0 + x, base + 1 + y, a, a * 2u / 3u, a / 4u);
        }
    }
    cv_rect(x0, base, w, 1, mix565(C_BLACK, COL_GOLD, 90));
    {                                    /* where POSITION points: a gold tick on the floor */
        int32_t px = x0 + (w - 1) * (int32_t)kv()[K_POS] / 100;
        cv_rect(px - 2, base + 2, 5, 2, COL_GOLD); cv_rect(px - 1, base + 4, 3, 1, COL_GOLD); cv_rect(px, base + 5, 1, 1, COL_GOLD);
    }
    for (i = 0; i < G_NV; i++)
        if (g_voice_pos(i, &note, &pos)) {   /* each note's reading point: a fading line */
            int32_t px = x0 + (w - 1) * pos / 255, yy;
            for (yy = y0 + 4; yy < base; yy++) {   /* (a faint beam, rising from the floor) */
                uint32_t a = (uint32_t)(yy - y0) * 34u / (uint32_t)(base - y0);
                light(px, yy, a * 2u / 3u, a / 2u, a);
            }
        }
    if (ui.page == PG_MAIN) {            /* the two macros: SELECT and ALGORITHM, their names and how far */
        int m;
        for (m = 0; m < 2; m++) {
            int32_t y = y0 + 2 + m * 13, bw = 56 * kv()[K_M1 + m] / 100;
            cv_text(10, y, &FONT_T, m ? "ALGO" : "SELECT", COL_LABEL);
            cv_text(48, y, &FONT_T, MACRO[set.preset % NPRESET][m].name, kv()[K_M1 + m] ? COL_GOLD : COL_LABEL);
            cv_rect(98, y + 5, 40, 1, COL_LINE);
            cv_rect(98, y + 5, bw * 40 / 56, 1, lerp565(COL_EMBER, COL_GOLD, bw, 56));
        }
    }
    for (i = 0; i < n; i++) {            /* the grains: soft lights between the pixels, each shedding dust */
        uint8_t nn = 60, pp;
        int32_t gx16, gy16;
        if (g_voice_pos(gv[i], &nn, &pp)) { (void)pp; }
        gx16 = (x0 << 4) + (int32_t)((uint32_t)(w - 1) * 16u * gp[i] / 255u);
        gy16 = ((y0 + 8) << 4) + (h - 34) * 16 * (84 - clamp(nn, 36, 84)) / 48;
        gy16 += (int32_t)(((gp[i] * 7u + (uint32_t)i * 13u) * 16u + (t >> 2)) % 240u) - 120;   /* (drifting) */
        {
            uint32_t lv = 120u + gl[i] / 2u, hl = lv / 3u;   /* (bright enough to be seen, its loudness on top) */
            glow(gx16, gy16, lv);
            glow(gx16 - 16, gy16, hl); glow(gx16 + 16, gy16, hl);   /* (a soft halo) */
            glow(gx16, gy16 - 16, hl); glow(gx16, gy16 + 16, hl);
            glow(gx16 - 24, gy16 - 24, hl / 3u); glow(gx16 + 24, gy16 - 24, hl / 3u);
            glow(gx16 - 24, gy16 + 24, hl / 3u); glow(gx16 + 24, gy16 + 24, hl / 3u);
            dust_shed(gx16, gy16, lv, 2 + (int)(gl[i] > 120u));
        }
    }
    dust_draw(y0);
    if (!piano_ok) {
        cv_text(10, y0 + 10, &FONT_XS, "THE PIANO'S SAMPLES ARE MISSING:", COL_EMBER);
        cv_text(10, y0 + 26, &FONT_XS, "INSTALL THE FULL PACKAGE AGAIN", COL_LABEL);
    }
}

static float w_loop;
/* the LOOP page: the loop's bars, where it is, its notes (gold) and knob moves (dusk, below), what REC does */
static void draw_loop(int32_t y0, int32_t h, uint32_t now)
{
    int pl, rc, bar, bars = g_seq_state(&pl, &rc, &w_loop, &bar), i, kind;
    int32_t x0 = 10, w = 220, b;
    float wh, pt;
    (void)now;
    cv_text(10, y0 + 2, &FONT_T, "REC", COL_LABEL);
    cv_text(34, y0 + 2, &FONT_T, rc == 2 ? "RECORDING" : rc == 1 ? "COUNT-IN" : "RECORDS (HELD 1 S: CLEARS)",
            rc ? COL_EMBER : COL_GOLD);
    cv_text(10, y0 + 15, &FONT_T, "PLAY", COL_LABEL);
    cv_text(34, y0 + 15, &FONT_T, pl ? "STOPS" : "PLAYS FROM THE TOP", COL_GOLD);
    for (b = 0; b <= bars; b++) {        /* the bars */
        int32_t x = x0 + w * b / bars;
        cv_rect(x, y0 + 32, 1, h - 36, b % 4 == 0 ? COL_LABEL : COL_LINE);
    }
    cv_rect(x0, y0 + h - 4, w, 1, COL_LINE);
    for (i = 0; g_seq_event(i, &wh, &kind, &pt); i++) {   /* a note: a dot at its start, high as its pitch */
        int32_t x = x0 + (int32_t)(wh * (float)w);
        if (kind == 2) cv_rect(x, y0 + h - 12, 1, 4, COL_DUSK);
        else if (kind == 0) {
            int32_t y = y0 + 34 + (int32_t)((float)(h - 50) * (1.0f - fm_clampf((pt - 36.0f) / 60.0f, 0.0f, 1.0f)));
            cv_rect(x - 1, y, 3, 3, COL_GOLD);
        }
    }
    if (pl && rc != 1) {                 /* where it is */
        int32_t x = x0 + (int32_t)(w_loop * (float)w);
        cv_rect(x, y0 + 30, 1, h - 30, rc ? COL_EMBER : COL_IVORY);
    }
}

/* a gauge: a ring of 270 degrees (from bottom left, clockwise), its pixels worked out once with their coverage
 * (smooth edges) and their place along the arc */
#define RING_R 16.5f
static struct { int8_t dx, dy; uint8_t a, f; } ring[600];
static int ring_n;
static float atan2_turns(float y, float x)   /* the angle of (x, y), 0..1 of a turn from +x towards +y */
{
    float ax = fm_fabsf(x), ay = fm_fabsf(y), a, r, s;
    if (ax < 1e-6f && ay < 1e-6f) return 0.0f;
    r = ax > ay ? ay / ax : ax / ay;
    s = r * r;
    a = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * r + r;   /* atan(r), 0..pi/4 */
    if (ay > ax) a = 1.57079637f - a;
    if (x < 0.0f) a = 3.14159274f - a;
    if (y < 0.0f) a = -a;
    a *= 0.159154943f;
    return a < 0.0f ? a + 1.0f : a;
}
static void ring_init(void)
{
    int dx, dy;
    ring_n = 0;
    for (dy = -20; dy <= 20; dy++)
        for (dx = -20; dx <= 20; dx++) {
            float d = fm_sqrtf((float)(dx * dx + dy * dy)), cov = 1.0f - fm_fabsf(d - RING_R) / 1.9f, rel;
            if (cov <= 0.0f || ring_n >= 600) continue;
            rel = atan2_turns((float)dy, (float)dx) - 0.375f;   /* from 135 degrees (bottom left), clockwise */
            if (rel < 0.0f) rel += 1.0f;
            if (rel > 0.75f) continue;   /* (the gap at the bottom) */
            ring[ring_n].dx = (int8_t)dx; ring[ring_n].dy = (int8_t)dy;
            ring[ring_n].a = (uint8_t)(fm_minf(1.0f, cov * 1.4f) * 255.0f);
            ring[ring_n].f = (uint8_t)(rel / 0.75f * 255.0f);
            ring_n++;
        }
}
static void draw_ring(int32_t cx, int32_t cy, int32_t v, int lit)   /* v 0..255 */
{
    int i;
    for (i = 0; i < ring_n; i++) {
        uint16_t c = ring[i].f <= v ? (lit ? COL_IVORY : lerp565(COL_EMBER, COL_GOLD, ring[i].f, 255)) : RGB(48, 40, 58);
        cv_pset(cx + ring[i].dx, cy + ring[i].dy, dim565(c, ring[i].a + 1u));
    }
    if (v > 0) {                         /* the pointer: a bright dot at the value's end */
        float a = (0.375f + 0.75f * (float)v / 255.0f) * 6.2831853f;
        int32_t px = cx + (int32_t)(fm_cosf(a) * RING_R + (fm_cosf(a) < 0.0f ? -0.5f : 0.5f));
        int32_t py = cy + (int32_t)(fm_sinf(a) * RING_R + (fm_sinf(a) < 0.0f ? -0.5f : 0.5f));
        cv_rect(px - 1, py - 1, 3, 3, COL_IVORY);
    }
}
static void draw_knobs(int32_t y0, uint32_t now)
{
    uint32_t i;
    if (!ring_n) ring_init();
    for (i = 0; i < 4u; i++) {
        int which = PAGE_K[ui.page][i];
        int32_t cx = 30 + (int32_t)i * 60, cy = y0 + 31, v;
        int lit = ui.turned_at[i] && now - ui.turned_at[i] < 500u;
        uint32_t nn;
        const char *name = knob_name(which), *big = 0, *below = 0;
        char t[24];
        value_text(which, t);
        if (which > K_PRESET) {          /* a page for every preset: names */
            uint32_t n2, v2 = *gk(which, &n2);
            v = n2 > 1u ? (int32_t)(v2 * 255u / (n2 - 1u)) : 255;
            if (which == K_TEMPO) big = gk_text(which, 1); else below = gk_text(which, 1);
        } else if (which == K_PRESET) {
            v = (int32_t)set.preset * 255 / (NPRESET - 1);
            below = PRESET_SHORT[set.preset];
        } else if (knob_names(which, &nn)) {
            v = nn > 1u ? (int32_t)kv()[which] * 255 / (int32_t)(nn - 1u) : 255;
            below = t;
        } else {
            v = (int32_t)kv()[which] * 255 / 100;
            big = t;
        }
        cv_text(cx - text_w(&FONT_T, name) / 2, y0, &FONT_T, name, lit ? COL_IVORY : COL_LABEL);
        draw_ring(cx, cy, v, lit);
        if (big) cv_text(cx - text_w(&FONT_B, big) / 2, cy - 8, &FONT_B, big, lit ? COL_IVORY : COL_GOLD);
        if (below) {                     /* a name: under the ring (the ring itself shows where in the list) */
            int32_t tw = text_w(&FONT_T, below);
            cv_text(cx - tw / 2, cy + 18, &FONT_T, below, lit ? COL_IVORY : COL_GOLD);
            cv_rect(cx - 2, cy - 2, 5, 5, lit ? COL_IVORY : mix565(C_BLACK, COL_GOLD, 160));
        }
    }
}

static void draw_keys(void)
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
    static const uint8_t AT[11] = {0, 1, 2, 4, 5, 7, 8, 9, 11, 12, 14};
    uint32_t i;
    char t[12];
    float w;
    cv_begin(240, 56, C_BLACK);
    bg_rows(56, C_BLACK, RGB(22, 16, 28));
    for (i = 0; i < 16u; i++) {          /* the white keys: ivory in the dark, a sheen at the top, lit when down */
        uint32_t k = WK[i];
        int32_t x = 5 + (int32_t)i * 14;
        int dn = (int)(ui.keys >> k & 1u);
        uint16_t body = dn ? COL_IVORY : key_left(k) ? RGB(70, 56, 96) : RGB(64, 58, 70);
        cv_rect(x, 4, 12, 26, body);
        cv_rect(x, 4, 12, 2, mix565(body, COL_IVORY, dn ? 0 : 60));
        cv_rect(x, 28, 12, 2, mix565(body, C_BLACK, 110));
        if (dn) cv_rect(x, 30, 12, 2, mix565(C_BLACK, COL_GOLD, 160));   /* (a glow under a key down) */
    }
    for (i = 0; i < 11u; i++) {          /* the black keys, over them */
        uint32_t k = BK[i];
        int32_t x = 5 + (int32_t)AT[i] * 14 + 9;
        int dn = (int)(ui.keys >> k & 1u);
        uint16_t body = dn ? COL_GOLD : key_left(k) ? RGB(36, 26, 52) : RGB(18, 15, 22);
        cv_rect(x - 1, 3, 10, 16, C_BLACK);
        cv_rect(x, 3, 8, 15, body);
        cv_rect(x + 1, 4, 6, 1, mix565(body, COL_IVORY, 40));
    }
    {                                    /* the loop, in a chip: stopped, playing (its bar), counting in, recording */
        int pl, rc, bar, bars = g_seq_state(&pl, &rc, &w, &bar);
        const char *st = rc == 2 ? "REC" : rc == 1 ? "COUNT" : pl ? "PLAY" : "STOP";
        uint16_t c = rc ? COL_EMBER : pl ? COL_GOLD : COL_LABEL;
        t[0] = 0; text_cat(t, st);
        if (pl && rc != 1) {
            char u[6];
            text_cat(t, " "); int_text(bar + 1, u); text_cat(t, u); text_cat(t, "/"); int_text(bars, u); text_cat(t, u);
        }
        frame_rect(5, 37, text_w(&FONT_T, t) + 16, 14, mix565(C_BLACK, c, 120));
        cv_rect(9, 42, 4, 4, c);
        cv_text(16, 38, &FONT_T, t, c);
    }
    t[0] = 0; text_cat(t, "CPU "); int_text((int32_t)plat_cpu_pct(), t + 4); text_cat(t, "%");
    cv_text(100, 38, &FONT_T, t, plat_cpu_pct() > 85u ? COL_EMBER : COL_LABEL);
    t[0] = 'O'; t[1] = 'C'; t[2] = 'T'; t[3] = ' ';
    t[4] = set.octave < 0 ? '-' : '+'; t[5] = (char)('0' + (set.octave < 0 ? -set.octave : set.octave)); t[6] = 0;
    cv_text(152, 38, &FONT_T, t, COL_LABEL);
    if (set_dirty) cv_text(196, 38, &FONT_T, "SAVE", COL_EMBER);
    cv_blit(0, 184);
}

static void draw(uint32_t now)
{
    draw_header(now);
    cv_begin(240, 144, C_BLACK);
    bg_rows(144, RGB(12, 9, 16), RGB(24, 17, 30));
    if (ui.page == PG_LOOP) draw_loop(0, 80, now);
    else draw_cloud(0, 84);
    draw_knobs(ui.page == PG_MAIN ? 88 : 84, now);
    cv_blit(0, 36);
    draw_keys();
}

/* ------------------------------------------------------------------ the splash --- */
/* GRAIN over PIANO in an 8-bit handheld's pixel type (Press Start 2P, tools/gen_title.py), shimmering gold on black, a
 * dark shadow under it: a highlight
 * sweeps across it, the gold breathes; then it breaks, left to right, into grains the wind takes to the right, and
 * they glow out. "by swtdr34mz" small under it. About 4.5 s at the start (a key or a button skips it). */
#include "title_data.h"
#define INTRO_SHIM 2200u                 /* ms: the shimmer, then the breaking, then the end */
#define INTRO_DIS 1800u
#define INTRO_END 500u
#define INTRO_Y 40                       /* the title's canvas, on the screen */
static uint16_t gold_lut[256];

static void gold_init(void)              /* dark bronze .. gold .. pale light gold */
{
    static const uint8_t G[5][3] = {{70, 44, 10}, {150, 100, 30}, {214, 160, 60}, {246, 206, 110}, {255, 240, 190}};
    uint32_t i;
    for (i = 0; i < 256u; i++) {
        uint32_t t = i * 4u, k = t / 256u, f = t % 256u, c[3], j;
        if (k > 3u) { k = 3u; f = 255u; }
        for (j = 0; j < 3u; j++) c[j] = (G[k][j] * (256u - f) + G[k + 1u][j] * f) / 256u;
        gold_lut[i] = RGB(c[0], c[1], c[2]);
    }
}
static uint16_t dim565(uint16_t c, uint32_t a)   /* c at a / 256 */
{
    uint32_t r = (c >> 11) * a >> 8, g = ((c >> 5) & 63u) * a >> 8, b = (c & 31u) * a >> 8;
    return (uint16_t)(r << 11 | g << 5 | b);
}

static void title_draw(uint32_t t)
{
    int32_t yt = (240 - TITLE_H) / 2 - 12 - INTRO_Y, x, y, band, front;
    int32_t fade = t >= 500u ? 256 : (int32_t)(t * 256u / 500u);
    int16_t breath[240];
    uint32_t i, by;
    cv_begin(240, 144, C_BLACK);
    band = -60 + (int32_t)(t * 360u / INTRO_SHIM);
    front = t > INTRO_SHIM ? (int32_t)((t - INTRO_SHIM) * 300u / INTRO_DIS) - 30 : -999;
    for (x = 0; x < 240; x++)
        breath[x] = (int16_t)(20.0f * fm_sinf((float)t * 0.003f + (float)x * 0.05f));
    if (t < INTRO_SHIM + INTRO_DIS) {
        for (y = 0; y < TITLE_H; y++) {
            int32_t base = 140 + 64 * (TITLE_H - y) / TITLE_H;
            for (x = front < 0 ? 0 : front; x < 240; x++) {
                uint32_t q = (uint32_t)(y * TITLE_W + x), a = (title_alpha[q >> 1] >> ((q & 1u) << 2)) & 15u;
                int32_t g, d;
                if (!a) continue;
                d = (x * 10 + y * 6) / 10 - band;
                if (d < 0) d = -d;
                g = base + breath[x] + (d < 22 ? (22 - d) * 140 / 22 : 0);
                g = g * fade >> 8;
                if (g > 255) g = 255;
                cv_pset(x, yt + y, dim565(gold_lut[g], a * 17u + 1u));
            }
        }
        if (t > INTRO_SHIM)             /* the grains, blown right as the front passes them */
            for (i = 0; i < TITLE_NG; i++) {
                uint32_t t0 = INTRO_SHIM + ((uint32_t)title_gx[i] + 30u) * INTRO_DIS / 300u;
                float u, sp, life, px, py;
                if (t < t0) continue;
                u = (float)(t - t0) * 0.001f;
                life = 1.0f - u / (0.9f + 0.8f * (float)title_gl[i] * (1.0f / 255.0f));
                if (life <= 0.0f) continue;
                sp = 0.6f + 1.8f * (float)title_gs[i] * (1.0f / 255.0f);
                px = (float)title_gx[i] + sp * 60.0f * u + 40.0f * u * u * sp;
                py = (float)title_gy[i] + (float)title_gd[i] * (0.35f / 128.0f) * 40.0f * u - 6.0f * u * u;
                {
                    uint16_t c = dim565(gold_lut[(uint32_t)(153.0f + 102.0f * life)], (uint32_t)(life * 256.0f));
                    int32_t gx = (int32_t)px, gy = yt + (int32_t)py;   /* a 2 x 2 grain: chunky, as the type */
                    cv_pset(gx, gy, c); cv_pset(gx + 1, gy, c); cv_pset(gx, gy + 1, c); cv_pset(gx + 1, gy + 1, c);
                }
            }
    }
    cv_blit(0, INTRO_Y);
    cv_begin(240, 28, C_BLACK);          /* the battery, here too */
    draw_battery(214, 11);
    cv_blit(0, 0);
    /* by swtdr34mz, fading in, and out at the end */
    by = t < 800u ? t * 256u / 800u : t < INTRO_SHIM + INTRO_DIS ? 256u :
         t < INTRO_SHIM + INTRO_DIS + INTRO_END ? 256u - (t - INTRO_SHIM - INTRO_DIS) * 256u / INTRO_END : 0u;
    cv_begin(240, 20, C_BLACK);
    {                                    /* in the title's pixel type */
        int32_t bx = (240 - BY_W) / 2;
        uint16_t c = dim565(gold_lut[130], by);
        for (y = 0; y < BY_H; y++)
            for (x = 0; x < BY_W; x++)
                if (title_by[y * BY_W + x]) cv_pset(bx + x, 6 + y, c);
    }
    cv_blit(0, 206);
}

void app_splash(void)                    /* at power-on, while the FM-1 starts: the title, still */
{
    gold_init();
    lcd_fill(0, 0, 240, 240, C_BLACK);
    title_draw(900u);
}

void app_ui_init(void)
{
    touch_on(1);
    gold_init();
    ui.page = PG_MAIN;
    ui.intro = 1;                        /* the splash runs first (app_frame) */
    ui.redraw = 1;
}

void app_frame(void)
{
    uint32_t now = plat_ms();
    uint32_t pkt;
    int port;
    input();
    while (plat_midi_in(&pkt, &port)) {  /* MIDI in plays the piano (any channel) */
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
        (void)port;
        if (st == 0xF8u) {               /* an outside clock: its tempo from each 24 ticks (a beat) */
            if (now - ui.clk_last > 500u) { ui.clk_n = 0; ui.clk_t0 = now; }
            ui.clk_last = now;
            if (++ui.clk_n >= 25u) {
                uint32_t dt = now - ui.clk_t0;
                if (dt > 200u && dt < 2000u) { g_tempo(60000.0f / (float)dt); ui.ext = 1; }
                ui.clk_n = 1; ui.clk_t0 = now;
            }
        }
        if (st == 0xFAu || st == 0xFBu) { g_seq(SEQ_PLAY); app_say("LOOP", "PLAY"); }   /* an outside start / stop */
        else if (st == 0xFCu) { g_seq(SEQ_STOP); app_say("LOOP", "STOP"); }
        if ((st & 0xF0u) == 0x90u && d2) g_note_on((int)d1, (float)d1, (float)d2 * (1.0f / 127.0f));
        else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u) g_note_off((int)d1);
    }
    if (ui.ext && now - ui.clk_last > 1000u) { ui.ext = 0; g_tempo(0.0f); ui.redraw = 1; }   /* the clock stopped */
    if (ui.mem && !g_memory_on() && now - ui.mem_t > 300u) { ui.mem = 0; ui.redraw = 1; }   /* (nothing played yet) */
    {                                    /* the loop's knob moves, played back: the knobs follow them */
        int kk, vv, any = 0;
        while (g_seq_knob_out(&kk, &vv))
            if (kk >= 0 && kk < NK) { kv()[kk] = (uint8_t)vv; any = 1; }
        if (any) { send_params(); ui.redraw = 1; }
    }
    ann_tick(now);
    if (ui.msg_until && now >= ui.msg_until) { ui.msg_until = 0; ui.redraw = 1; }
    if (ui.intro) {                      /* the splash: to its end, or until a key or a button */
        if (!ui.intro_t0) ui.intro_t0 = now ? now : 1u;
        if (now - ui.intro_t0 >= INTRO_SHIM + INTRO_DIS + INTRO_END || ui.keys || ui.btn) {
            ui.intro = 0;
            lcd_fill(0, 0, 240, 240, C_BLACK);
            ui.redraw = 1;
        } else {
            if (now - ui.t_draw >= 30u) { ui.t_draw = now; title_draw(now - ui.intro_t0); }
            return;
        }
    }
    /* the cloud moves while anything sounds: about 30 pictures a second; else only on a change */
    if (ui.redraw || ((g_level() > 0.0002f || hd.n || dust_live) && now - ui.t_draw >= 33u)) {
        ui.redraw = 0;
        ui.t_draw = now;
        draw(now);
    }
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "hold")) *got = set.hold;
    else if (!strcmp(what, "held")) *got = hd.n;
    else if (!strncmp(what, "held", 4) && what[4] >= '0' && what[4] <= '7')   /* held0..held7: the notes */
        *got = what[4] - '0' < hd.n ? (int)(hd.pitch[what[4] - '0'] + 0.5f) : -1;
    else if (!strncmp(what, "cents", 5) && what[5] >= '0' && what[5] <= '7')   /* cents0..7: a held note's pitch x 100 */
        *got = what[5] - '0' < hd.n ? (int)(hd.pitch[what[5] - '0'] * 100.0f + 0.5f) : -1;
    else if (!strcmp(what, "scale")) *got = SC;
    else if (!strcmp(what, "split")) *got = SPLIT_OCT ? (int)SPLIT_KEY : 0;
    else if (!strncmp(what, "ev", 2) && what[2] >= '0' && what[2] <= '9') {   /* evN: event N's place x 1000 */
        float w;
        int kd;
        float pt;
        *got = g_seq_event(atoi(what + 2), &w, &kd, &pt) ? (int)(w * 1000.0f + 0.5f) : -1;
    } else if (!strcmp(what, "lastcents")) *got = (int)(last_pitch * 100.0f + 0.5f);
    else if (!strcmp(what, "intro")) *got = ui.intro;
    else if (!strcmp(what, "seq_fired")) *got = (int)g_seq_fired();
    else if (!strcmp(what, "seq_play") || !strcmp(what, "seq_rec") || !strcmp(what, "seq_events") ||
             !strcmp(what, "seq_notes")) {
        int pl, rc, bar, kd, n = 0, m = 0;
        float w, pt;
        g_seq_state(&pl, &rc, &w, &bar);
        while (g_seq_event(n, &w, &kd, &pt)) { if (kd == 0) m++; n++; }
        *got = what[4] == 'p' ? pl : what[4] == 'r' ? rc : what[4] == 'e' ? n : m;
    }
    else if (!strcmp(what, "memory")) *got = g_memory_on();
    else if (!strcmp(what, "ext")) *got = ui.ext;
    else if (!strcmp(what, "page")) *got = ui.page;
    else if (!strcmp(what, "preset")) *got = set.preset;
    else if (!strcmp(what, "octave")) *got = set.octave;
    else if (!strcmp(what, "piano")) *got = piano_ok;
    else if (!strcmp(what, "dirty")) *got = set_dirty;
    else if (!strcmp(what, "grains")) { uint8_t a[G_NG], b[G_NG], c[G_NG]; *got = g_grains(a, b, c, G_NG); }
    else if (!strcmp(what, "level")) *got = (int)(g_level() * 1000.0f);
    else if (!strncmp(what, "k_", 2)) {  /* k_NAME: a knob's value in the preset */
        int i;
        for (i = 0; i < NK; i++)
            {
                char nm[16];
                uint32_t q;
                for (q = 0; K_NAME[i][q] && q < 15u; q++) nm[q] = K_NAME[i][q] == ' ' ? '_' : K_NAME[i][q];
                nm[q] = 0;
                if (!strcmp(what + 2, nm)) { *got = kv()[i]; return 1; }   /* (a space as _: k_MACRO_1) */
            }
        return 0;
    } else
        return 0;
    return 1;
}
#endif
