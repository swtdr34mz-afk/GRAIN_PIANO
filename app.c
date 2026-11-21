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
       K_TREACT, NK, K_PRESET = 100, K_SCALE, K_ROOT, K_KEYS, K_LIGHT };   /* (100..: not a preset's own) */
static const char *const K_NAME[NK] = {"TONE", "DYNAMICS", "MIX", "SIZE", "DENSITY", "POSITION", "SPRAY", "ATTACK",
    "RELEASE", "PEDAL", "FREEZE", "RATE", "DEPTH", "TARGET", "WOBBLE", "ROOM", "REVERB", "DELAY", "FELT", "REVERSE",
    "HALO", "HALO MIX", "WIDTH", "TEXTURE", "LEVEL", "CHAR", "REACT"};
static const char *const TARGET_NAME[NLFO_TARGET] = {"POSITION", "PITCH", "FILTER", "PAN"};
static const char *const HALO_NAME[NHALO] = {"OFF", "OCTAVE", "FIFTH", "OCT+5TH", "MAJOR", "MINOR", "SUS", "MAJ9"};
static const char *const TEX_NAME[NTEX] = {"OFF", "RAIN", "VINYL", "TAPE", "WIND", "SEA", "ROOM"};
/* a knob that picks from names: their list and count (0: a value 0..100) */
static const char *const *knob_names(int k, uint32_t *n)
{
    if (k == K_LTARGET) { *n = NLFO_TARGET; return TARGET_NAME; }
    if (k == K_HALO) { *n = NHALO; return HALO_NAME; }
    if (k == K_TEX) { *n = NTEX; return TEX_NAME; }
    *n = 0;
    return 0;
}

enum { PG_MAIN, PG_SEL, PG_EDIT, PG_ENV, PG_LFO, PG_FX, PG_MAGIC, PG_TEX, PG_SCALE, NPAGE };
static const char *const PAGE_NAME[NPAGE] = {"MAIN", "PIANO", "GRAINS", "ENVELOPE", "MOVEMENT", "ROOM", "MAGIC",
                                             "TEXTURE", "SCALE"};
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
 * REVERSE HALO HALOMIX WIDTH | TEXTURE LEVEL CHARACTER REACT (0..100, or a name's number) */
static const uint8_t PRESET_K[NPRESET][NK] = {
    {72, 70, 10, 50, 25, 15, 10, 0, 35, 45, 0, 30, 0, LFO_POS, 0, 45, 22, 0, 0, 0, HALO_OFF, 0, 50, TEX_OFF, 30, 50, 0},
    {45, 55, 22, 60, 30, 25, 10, 2, 50, 60, 30, 20, 0, LFO_POS, 15, 35, 30, 10, 75, 0, HALO_OFF, 0, 50, TEX_OFF, 30, 40, 0},
    {60, 40, 85, 80, 65, 35, 30, 45, 80, 80, 85, 15, 40, LFO_POS, 10, 85, 55, 25, 10, 0, HALO_OFF, 0, 60, TEX_OFF, 30, 50, 0},
    /* AURORA: the cloud, an octave and a fifth above it, wide, a huge room */
    {78, 40, 80, 75, 60, 40, 25, 50, 85, 80, 90, 12, 35, LFO_POS, 5, 95, 60, 20, 0, 10, HALO_OCT5, 55, 85, TEX_OFF, 20, 40, 0},
    /* MIDNIGHT RAIN: a felt piano in the rain, the filter breathing */
    {42, 50, 55, 70, 45, 30, 20, 20, 70, 75, 70, 10, 30, LFO_FILTER, 10, 70, 45, 15, 60, 15, HALO_OFF, 0, 60, TEX_RAIN, 50, 55, 30},
    /* DREAMCORE: grains backwards, a major halo, a tape that leans */
    {58, 40, 75, 65, 55, 45, 35, 35, 75, 70, 60, 20, 25, LFO_PITCH, 35, 75, 50, 30, 20, 60, HALO_MAJ, 35, 70, TEX_TAPE, 30, 40, 20},
    /* UNDERWATER: dark, slow waves, the filter rolling with the sea */
    {35, 40, 70, 85, 55, 40, 25, 40, 80, 80, 80, 8, 70, LFO_FILTER, 15, 80, 55, 10, 40, 20, HALO_OCT, 20, 60, TEX_SEA, 45, 60, 10},
    /* STARDUST: lo-fi maj9 dust on vinyl */
    {62, 50, 65, 45, 60, 50, 40, 15, 70, 70, 65, 25, 30, LFO_PAN, 10, 70, 50, 25, 30, 25, HALO_MAJ9, 45, 75, TEX_VINYL, 40, 45, 25},
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
    if (bad) settings_defaults();
    if (set.rsv2[0] >= 15u || set.rsv2[1] > 11u || set.rsv2[2] > 2u || set.rsv2[3] > 1u)   /* the scale page's (below) */
        set.rsv2[0] = set.rsv2[1] = set.rsv2[2] = set.rsv2[3] = 0;
}

static uint8_t *kv(void) { return set.k[set.preset]; }

static void send_params(void)
{
    const uint8_t *k = kv();
    gparams_t p;
#define F(i) ((float)k[i] * 0.01f)
    p.tone = F(K_TONE); p.dyn = F(K_DYN); p.mix = F(K_MIX);
    p.size = F(K_SIZE); p.dens = F(K_DENS); p.pos = F(K_POS); p.spray = F(K_SPRAY);
    p.atk = F(K_ATK); p.rel = F(K_REL); p.pedal = F(K_PEDAL); p.freeze = F(K_FREEZE);
    p.lrate = F(K_LRATE); p.ldepth = F(K_LDEPTH); p.wobble = F(K_WOBBLE); p.ltarget = k[K_LTARGET];
    p.rsize = F(K_RSIZE); p.rmix = F(K_RMIX); p.delay = F(K_DELAY); p.felt = F(K_FELT);
    p.rev = F(K_REV); p.halo = k[K_HALO]; p.hamt = F(K_HAMT); p.width = F(K_WIDTH);
    p.tex = k[K_TEX]; p.tlev = F(K_TLEV); p.tchar = F(K_TCHAR); p.treact = F(K_TREACT);
#undef F
    g_params(&p);
}

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
static int key_pitch(uint32_t k, float *pitch, int *root)
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS;
    const scale_t *sc = &SCALES[SC % NSCALE];
    float base = 48.0f + (float)(ROOT % 12u) + 12.0f * (float)set.octave;   /* the root, in the octave of C3 */
    int32_t d = 0, i;
    if (KEYMODE == KEYS_ALL || KEYMODE == KEYS_WHITE) {
        if (KEYMODE == KEYS_ALL) d = (int32_t)k;
        else {
            for (i = 0; i < 16 && WK[i] != k; i++) {}
            if (i == 16) return 0;
            d = i;
        }
        if (base < 53.0f - 6.0f) base += 12.0f;   /* (the first key near F3) */
        *pitch = degree_pitch(base, d);
        *root = (d % sc->n) == 0;
        return 1;
    }
    {                                    /* SNAP: the key's own pitch, to the nearest step at or below it */
        float c = (53.0f + (float)k + 12.0f * (float)set.octave - (base - 12.0f)) * 100.0f, best = -1e9f, x;
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
static uint8_t snd_midi[512];            /* the notes on, by id: the MIDI note sent out + 1 (0: none) */

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
    uint32_t home_t, play_t;             /* HOME / PLAY went down (0: not held, or the hold already acted) */
    uint32_t turned_at[4];               /* when K1..K4 (as shown) were last turned: lit for a moment */
    uint32_t t_draw;
    uint8_t redraw;
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
static uint8_t *gk(int which, uint32_t *n)
{
    static const uint8_t N[4] = {NSCALE, 12, NKEYMODE, 2};
    *n = N[(which - K_SCALE) & 3];
    return &set.rsv2[(which - K_SCALE) & 3];
}
static const char *gk_text(int which, int shrt)
{
    uint32_t n, v = *gk(which, &n) % (n ? n : 1u);
    if (which == K_SCALE) return shrt ? SCALES[v].shrt : SCALES[v].name;
    if (which == K_ROOT) return ROOT_NAME[v];
    if (which == K_KEYS) return KEYS_NAME[v];
    return ONOFF_NAME[v & 1u];
}

static void value_text(int which, char *t)   /* a knob's value as shown */
{
    t[0] = 0;
    if (which == K_PRESET) { text_cat(t, PRESET_NAME[set.preset]); return; }
    if (which > K_PRESET) { text_cat(t, gk_text(which, 0)); return; }
    {
        uint32_t nn;
        const char *const *nm = knob_names(which, &nn);
        if (nm) { text_cat(t, nm[kv()[which] % nn]); return; }
    }
    int_text(kv()[which], t);
}
static const char *knob_name(int which)
{
    static const char *const G[4] = {"SCALE", "ROOT", "KEYS", "LIGHT"};
    return which == K_PRESET ? "PRESET" : which > K_PRESET ? G[(which - K_SCALE) & 3] : K_NAME[which];
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
        v = clamp((int32_t)*g + (e > 0 ? 1 : -1), 0, (int32_t)nn - 1);
        if (v == *g) return;
        *g = (uint8_t)v;
        set_dirty = 1;
        app_say(knob_name(which), gk_text(which, 0));
        return;
    }
    if (knob_names(which, &nn)) v = clamp((int32_t)k[which] + (e > 0 ? 1 : -1), 0, (int32_t)nn - 1);
    else v = clamp((int32_t)k[which] + e * (e > 1 || e < -1 ? 3 : 2), 0, 100);   /* a quick turn goes further */
    if (v == k[which]) return;
    k[which] = (uint8_t)v;
    set_dirty = 1;
    send_params();
    value_text(which, t);
    app_say(knob_name(which), t);
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
                key_press(k, v, cur);
                cur |= bit;
                ui.redraw = 1;
            }
        }
    }
    ui.keys = keys;
    hold_tick(now);
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit)) continue;
        switch (k) {
        case B_FX: page_go(PG_FX); break;
        case B_SEL: page_go(PG_SEL); break;
        case B_ENV: page_go(PG_ENV); break;
        case B_LFO: page_go(PG_LFO); break;
        case B_EDIT: page_go(PG_EDIT); break;
        case B_GLO: page_go(PG_MAGIC); break;
        case B_ARP: page_go(PG_TEX); break;
        case B_SEQ: page_go(PG_SCALE); break;
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
    if (ui.play_t) {                     /* PLAY: a press, HOLD on / off; held 0.8 s, the held notes let go */
        if (btn >> B_PLAY & 1u) {
            if (now - ui.play_t >= 800u) {
                ui.play_t = 0;
                held_clear(-1);
                hd.pending = -1;
                app_say("HOLD", "ALL OFF");
            }
        } else {
            ui.play_t = 0;
            hold_set(!set.hold, keys);
            app_say("HOLD", set.hold ? "ON" : "OFF");
        }
    }
    ui.btn = btn;
    for (k = 0; k < 4u; k++)             /* K1..K4: the page's four */
        if ((e = plat_enc((int)(EN_K1 + k))) != 0) {
            knob_turn(PAGE_K[ui.page][k], e);
            ui.turned_at[k] = now ? now : 1u;
        }
    if ((e = plat_enc(EN_PRESET)) != 0) preset_step(e > 0 ? 1 : -1);
    if ((e = plat_enc(EN_SELECT)) != 0) knob_turn(K_POS, e);
    if ((e = plat_enc(EN_ALGO)) != 0) knob_turn(K_SPRAY, e);
    /* the lights: the page's button, PLAY while HOLD is on, SAVE when something changed; the keys down bright, the
     * held notes' keys dim */
    {
        static const uint8_t PB[NPAGE] = {B_HOME, B_SEL, B_EDIT, B_ENV, B_LFO, B_FX, B_GLO, B_ARP, B_SEQ};
        lit_btn = 1u << PB[ui.page];
        if (set.hold) lit_btn |= 1u << B_PLAY | LED_PLAY_GREEN;
        if (set_dirty) lit_btn |= 1u << B_SAVE;
    }
    lit_keys = keys;
    for (i = 0; i < hd.n; i++) {
        int32_t kk = held_key(i);
        if (kk >= 0) dim_keys |= 1u << kk;
    }
    plat_lights(lit_btn, lit_keys, 0, dim_keys, 1);
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

static void draw_header(uint32_t now)
{
    cv_begin(240, 32, C_BLACK);
    if (now < ui.msg_until) {
        int32_t x = cv_text(8, 7, &FONT_B, ui.msg[0], COL_IVORY);
        cv_text(x + 8, 7, &FONT_B, ui.msg[1], COL_GOLD);
    } else {
        const char *pn = PRESET_NAME[set.preset];
        cv_text(8, 7, &FONT_B, pn, COL_IVORY);
        cv_text(232 - text_w(&FONT_XS, PAGE_NAME[ui.page]), 9, &FONT_XS, PAGE_NAME[ui.page], COL_LABEL);
    }
    cv_rect(0, 30, 240, 1, COL_LINE);
    cv_blit(0, 0);
}

/* the cloud: the sample from its hammer (left) to its tail (right), each voice's position a line, each grain a dot,
 * brighter as it sounds louder, its height the voice's note */
static void draw_cloud(int32_t y0, int32_t h)
{
    uint8_t gp[G_NG], gl[G_NG], gv[G_NG], note, pos;
    int n = g_grains(gp, gl, gv, G_NG), i;
    int32_t x0 = 10, w = 220;
    uint32_t t = plat_ms();
    for (i = 0; i < 12; i++) {           /* a faint floor: the sample's length, marked every twelfth */
        int32_t x = x0 + (w * i) / 11;
        cv_rect(x, y0 + h - 4, 1, 3, COL_LINE);
    }
    cv_rect(x0, y0 + h - 2, w, 1, COL_LINE);
    {                                    /* where POSITION points (as set), gold */
        int32_t x = x0 + (w - 1) * (int32_t)kv()[K_POS] / 100;
        cv_rect(x - 2, y0 + h - 7, 5, 5, COL_GOLD);
    }
    for (i = 0; i < G_NV; i++)
        if (g_voice_pos(i, &note, &pos)) {
            int32_t x = x0 + (w - 1) * pos / 255, yy;
            int32_t ny = y0 + 6 + (int32_t)(h - 18) * (84 - clamp(note, 36, 84)) / 48;
            for (yy = y0 + 4; yy < y0 + h - 8; yy += 3) cv_rect(x, yy, 1, 1, COL_DUSK);
            cv_rect(x - 3, ny, 7, 1, COL_EMBER);
        }
    for (i = 0; i < n; i++) {
        int32_t x = x0 + (w - 1) * gp[i] / 255;
        uint8_t nn = 60, pp;
        int32_t y, s;
        if (g_voice_pos(gv[i], &nn, &pp)) { (void)pp; }
        y = y0 + 6 + (int32_t)(h - 18) * (84 - clamp(nn, 36, 84)) / 48;
        y += (int32_t)((gp[i] * 7u + i * 13u + (t >> 6)) % 15u) - 7;   /* a little drift: a cloud, not a grid */
        s = 1 + gl[i] / 90;
        cv_rect(x - s / 2, y - s / 2, s + 1, s + 1, mix565(COL_DUSK, COL_IVORY, gl[i]));
    }
    if (!piano_ok) {
        cv_text(10, y0 + 10, &FONT_XS, "THE PIANO'S SAMPLES ARE MISSING:", COL_EMBER);
        cv_text(10, y0 + 26, &FONT_XS, "INSTALL THE FULL PACKAGE AGAIN", COL_LABEL);
    }
}

static void draw_knobs(int32_t y0, uint32_t now)
{
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        int which = PAGE_K[ui.page][i];
        int32_t x = 4 + (int32_t)i * 59, wbar = 53, fill;
        int lit = ui.turned_at[i] && now - ui.turned_at[i] < 500u;
        uint32_t nn;
        char t[24];
        cv_text(x, y0, &FONT_XS, knob_name(which), lit ? COL_IVORY : COL_LABEL);
        value_text(which, t);
        if (which > K_PRESET) {          /* the scale page */
            uint32_t n2, v2 = *gk(which, &n2);
            cv_text(x, y0 + 16, &FONT_B, gk_text(which, 1), lit ? COL_IVORY : COL_GOLD);
            fill = wbar * ((int32_t)v2 + 1) / (int32_t)n2;
        } else if (which == K_PRESET) {  /* the short name: a cell is 59 pixels wide */
            cv_text(x, y0 + 16, &FONT_B, PRESET_SHORT[set.preset], lit ? COL_IVORY : COL_GOLD);
            fill = wbar * ((int32_t)set.preset + 1) / NPRESET;
        } else if (knob_names(which, &nn)) {
            cv_text(x, y0 + 16, &FONT_B, t, lit ? COL_IVORY : COL_GOLD);
            fill = wbar * ((int32_t)kv()[which] + 1) / (int32_t)nn;
        } else {
            cv_text(x, y0 + 14, &FONT_M, t, lit ? COL_IVORY : COL_GOLD);
            fill = wbar * (int32_t)kv()[which] / 100;
        }
        cv_rect(x, y0 + 42, wbar, 3, COL_LINE);
        cv_rect(x, y0 + 42, fill, 3, lit ? COL_IVORY : COL_EMBER);
    }
}

static void draw_keys(void)
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
    static const uint8_t AT[11] = {0, 1, 2, 4, 5, 7, 8, 9, 11, 12, 14};
    uint32_t held = 0, i;
    char t[12];
    int j;
    for (j = 0; j < hd.n; j++) {
        int32_t kk = held_key(j);
        if (kk >= 0) held |= 1u << kk;
    }
    cv_begin(240, 56, C_BLACK);
    for (i = 0; i < 16u; i++) {
        uint32_t k = WK[i];
        uint16_t c = (ui.keys >> k & 1u) ? COL_IVORY : (held >> k & 1u) ? COL_GOLD : COL_LINE;
        cv_rect(4 + (int32_t)i * 14, 14, 13, 16, c);
    }
    for (i = 0; i < 11u; i++) {
        uint32_t k = BK[i];
        uint16_t c = (ui.keys >> k & 1u) ? COL_IVORY : (held >> k & 1u) ? COL_EMBER : COL_NIGHT;
        cv_rect(4 + (int32_t)AT[i] * 14 + 8, 4, 11, 12, c);
    }
    cv_text(4, 36, &FONT_XS, "HOLD", set.hold ? COL_GOLD : COL_LINE);
    if (set.hold && hd.n) {
        t[0] = 0; int_text(hd.n, t); text_cat(t, hd.n == 1 ? " NOTE" : " NOTES");
        cv_text(40, 36, &FONT_XS, t, COL_IVORY);
    }
    cv_text(150, 36, &FONT_XS, "OCT", COL_LABEL);
    t[0] = set.octave < 0 ? '-' : '+'; t[1] = (char)('0' + (set.octave < 0 ? -set.octave : set.octave)); t[2] = 0;
    cv_text(176, 36, &FONT_XS, t, COL_IVORY);
    if (set_dirty) cv_text(204, 36, &FONT_XS, "SAVE", COL_EMBER);
    t[0] = 0; text_cat(t, "CPU "); int_text((int32_t)plat_cpu_pct(), t + 4); text_cat(t, "%");
    cv_text(96, 36, &FONT_XS, t, plat_cpu_pct() > 85u ? COL_EMBER : COL_LABEL);   /* the audio's load (for the tests) */
    cv_blit(0, 184);
}

static void draw(uint32_t now)
{
    draw_header(now);
    cv_begin(240, 144, C_BLACK);
    draw_cloud(0, ui.page == PG_MAIN ? 92 : 80);
    draw_knobs(ui.page == PG_MAIN ? 96 : 92, now);
    cv_blit(0, 36);
    draw_keys();
}

void app_ui_init(void)
{
    touch_on(1);
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.page = PG_MAIN;
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
        if ((st & 0xF0u) == 0x90u && d2) g_note_on((int)d1, (float)d1, (float)d2 * (1.0f / 127.0f));
        else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u) g_note_off((int)d1);
    }
    ann_tick(now);
    if (ui.msg_until && now >= ui.msg_until) { ui.msg_until = 0; ui.redraw = 1; }
    /* the cloud moves while anything sounds: about 30 pictures a second; else only on a change */
    if (ui.redraw || ((g_level() > 0.0002f || hd.n) && now - ui.t_draw >= 33u)) {
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
            if (!strcmp(what + 2, K_NAME[i])) { *got = kv()[i]; return 1; }
        return 0;
    } else
        return 0;
    return 1;
}
#endif
