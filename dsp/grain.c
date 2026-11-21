/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 swtdr34mz */
/* GRAIN PIANO's sound engine. See grain.h. Single precision only, no double, no libc.
 * The render works in chunks of 32 frames: envelopes, the LFO, the filters' coefficients and the grains' births are
 * updated per chunk, the voices, grains and effects per sample.
 *
 * The samples (tools/make_piano.py, the same res/piano.bin as CHORD RUN's: its sampler is the model of the dry voice)
 * are 4-bit IMA ADPCM, read in place from the flash. ADPCM is decoded from its start only, so a grain that starts in
 * the middle of a sample needs the decoder's state there: g_samples decodes every sample once and keeps the state
 * every 256 samples (a checkpoint: 4 bytes), with the sample's loudness there (a grain from the quiet tail is lifted,
 * so FREEZE and POSITION work the whole length). A grain then starts at the checkpoint below and decodes forward. */
#include "grain.h"
#include "fastmath.h"
#include "plat.h"

#define SR G_SR
#define CHUNK 32u
#define CK_SH 8u                        /* a checkpoint every 256 samples */
#define CK_MAX 2560u                    /* the nine notes (25 s at 22,050 Hz) take about 2,130 */
#define MS_MAX 512u                     /* blocks in one note at most (the longest: 3.4 s, 293) */
#define RV_MAXL 4600u                   /* the reverb's lines at their longest (room size 1) */
#define DL_N 24000u                     /* the ping-pong delay, per side: 0.54 s */
#define DL_L 15876u                     /* its left time (0.36 s) and right (0.54 s) */
#define DL_R 23800u

static void zero(void *d, uint32_t n) { uint8_t *p = (uint8_t *)d; while (n--) *p++ = 0; }
static void copy(void *d, const void *s, uint32_t n)
{
    uint8_t *p = (uint8_t *)d;
    const uint8_t *q = (const uint8_t *)s;
    while (n--) *p++ = *q++;
}

/* ------------------------------------------------------------------ big buffers (the pool on the device) --- */
static uint32_t ck[CK_MAX] PLAT_POOL;   /* the decoder's state: predictor (low 16 bits), step index (bits 16..23) */
static uint16_t ck_gain[CK_MAX] PLAT_POOL;   /* a grain's lift there, Q8 (256 = as it is) */
static float rv_line[4][RV_MAXL] PLAT_POOL;
static int16_t dl_buf[2][DL_N] PLAT_POOL;

/* ------------------------------------------------------------------ the samples --- */
typedef struct { uint8_t root, sidx, lidx, pad; int16_t spred, lpred; uint32_t off, len, loop; } smp_ent_t;
static const uint8_t *smp_data;
static smp_ent_t smp_e[16];
static uint16_t ck_off[16];
static int smp_n;
static float smp_ratio;                 /* the samples' rate / SR */

static const int16_t IMA_STEP[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55,
    60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598,
    658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};
static const int8_t IMA_INDEX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

typedef struct {                        /* a decoder on one sample */
    const uint8_t *d;
    uint32_t pos, len, loop;
    int32_t pred, idx, lpred, lidx;
} dec_t;

/* the next sample; at the sample's end it goes on from the loop's start (the state there is in the table) */
static inline int32_t dec_next(dec_t *s)
{
    uint32_t nib = (uint32_t)(s->d[s->pos >> 1] >> ((s->pos & 1u) << 2)) & 15u;
    int32_t step = IMA_STEP[s->idx], vp = step >> 3, out;
    if (nib & 4u) vp += step;
    if (nib & 2u) vp += step >> 1;
    if (nib & 1u) vp += step >> 2;
    s->pred += (nib & 8u) ? -vp : vp;
    if (s->pred > 32767) s->pred = 32767; else if (s->pred < -32768) s->pred = -32768;
    s->idx += IMA_INDEX[nib & 7u];
    if (s->idx < 0) s->idx = 0; else if (s->idx > 88) s->idx = 88;
    out = s->pred;
    if (++s->pos >= s->len) { s->pos = s->loop; s->pred = s->lpred; s->idx = s->lidx; }
    return out;
}

static void dec_start(dec_t *s, int e)
{
    const smp_ent_t *x = &smp_e[e];
    s->d = smp_data + x->off; s->len = x->len; s->loop = x->loop;
    s->pos = 0; s->pred = x->spred; s->idx = x->sidx; s->lpred = x->lpred; s->lidx = x->lidx;
}

/* the decoder at sample p of note e: from the checkpoint below, forward */
static void dec_seek(dec_t *s, int e, uint32_t p)
{
    uint32_t st;
    dec_start(s, e);
    if (p >= s->len) p = s->len - 1u;
    st = ck[ck_off[e] + (p >> CK_SH)];
    s->pos = p & ~((1u << CK_SH) - 1u);
    s->pred = (int16_t)(st & 0xFFFFu);
    s->idx = (int32_t)((st >> 16) & 0xFFu);
    while (s->pos < p) (void)dec_next(s);
}

int g_samples(const uint8_t *p, uint32_t len)
{
    uint32_t n, i, magic, rate, total = 0;
    smp_data = 0; smp_n = 0;
    if (!p || len < 16u) return 0;
    copy(&magic, p, 4); copy(&n, p + 4, 4); copy(&rate, p + 8, 4);
    if (magic != 0x314F4E50u || n == 0u || n > 16u || len < 16u + n * 20u || rate < 8000u || rate > 48000u) return 0;
    for (i = 0; i < n; i++) {
        smp_ent_t e;
        copy(&e, p + 16u + i * 20u, 20u);
        if (e.off > len || e.len < 1024u || e.loop >= e.len || e.off + (e.len + 1u) / 2u > len || e.sidx > 88u ||
            e.lidx > 88u)
            return 0;
        smp_e[i] = e;
        ck_off[i] = (uint16_t)total;
        total += (e.len + (1u << CK_SH) - 1u) >> CK_SH;
        if (total > CK_MAX) return 0;
    }
    smp_data = p;
    smp_n = (int)n;
    /* every note decoded once: the checkpoints, and each block's loudness (RMS over 1024 samples around it) */
    for (i = 0; i < n; i++) {
        static float ms[MS_MAX], rr[MS_MAX];   /* each block's mean square, then its RMS around it */
        dec_t d;
        uint32_t k, j, len = smp_e[i].len, nb = (len + (1u << CK_SH) - 1u) >> CK_SH;
        float top = 0.0f;
        if (nb > MS_MAX) return 0;
        dec_start(&d, (int)i);
        for (j = 0; j < len; j++) {     /* straight through (the decoder only wraps after the last sample) */
            float x;
            if (!(j & ((1u << CK_SH) - 1u))) {
                ck[ck_off[i] + (j >> CK_SH)] = ((uint32_t)(uint16_t)(int16_t)d.pred) | ((uint32_t)d.idx << 16);
                ms[j >> CK_SH] = 0.0f;
            }
            x = (float)dec_next(&d) * (1.0f / 32768.0f);
            ms[j >> CK_SH] += x * x * (1.0f / (float)(1u << CK_SH));
        }
        for (k = 0; k < nb; k++) {
            float a = 0.0f;
            int32_t q, c = 0;
            for (q = (int32_t)k - 1; q <= (int32_t)k + 2; q++)
                if (q >= 0 && q < (int32_t)nb) { a += ms[q]; c++; }
            rr[k] = fm_sqrtf(a / (float)c);
            if (rr[k] > top) top = rr[k];
        }
        for (k = 0; k < nb; k++) {      /* a grain from a quiet part is lifted most of the way, never more than 18 dB */
            float g = rr[k] > 1e-6f ? top / rr[k] : 12.0f;
            g = fm_powf(fm_clampf(g, 1.0f, 12.0f), 0.8f);
            ck_gain[ck_off[i] + k] = (uint16_t)(g * 256.0f);
        }
    }
    smp_ratio = (float)rate / SR;
    return 1;
}

static int smp_nearest(float m)
{
    int i, best = 0;
    float bd = 1e9f;
    for (i = 0; i < smp_n; i++) {
        float d = fm_fabsf(m - (float)smp_e[i].root);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* ------------------------------------------------------------------ the command queue (main loop -> render) --- */
enum { CMD_ON, CMD_OFF, CMD_ALLOFF, CMD_PANIC };
typedef struct { uint8_t t, note; float vel; } cmd_t;
#define QN 64u
static cmd_t cq[QN];
static volatile uint32_t cq_head, cq_tail;
static gparams_t pbuf[2];
static volatile uint32_t pcur;

static void push(uint8_t t, int note, float vel)
{
    uint32_t h = cq_head, nh = (h + 1u) % QN;
    if (nh == cq_tail) return;          /* full (the render is not running): dropped */
    cq[h].t = t; cq[h].note = (uint8_t)(note & 127); cq[h].vel = vel;
    __asm__ volatile("" ::: "memory");
    cq_head = nh;
}
void g_note_on(int note, float vel) { push(CMD_ON, note, vel); }
void g_note_off(int note) { push(CMD_OFF, note, 0.0f); }
void g_all_off(void) { push(CMD_ALLOFF, 0, 0.0f); }
void g_panic(void) { push(CMD_PANIC, 0, 0.0f); }
void g_params(const gparams_t *p)       /* into the buffer the render does not read, then swapped */
{
    uint32_t nx = pcur ^ 1u;
    pbuf[nx] = *p;
    __asm__ volatile("" ::: "memory");
    pcur = nx;
}

/* ------------------------------------------------------------------ state (render side) --- */
typedef struct {
    uint8_t on, held, note, e, dry_on, ng;
    float vel, amp, pan, rate;          /* rate: the step through its sample for its pitch (per chunk, with the wobble) */
    dec_t dry;
    float d0, d1, dfrac, dg;            /* the dry voice: two decoded samples, where between, its own fade (loop passes) */
    float denv, aenv, genv;             /* the damper (1 until released), the attack, the grains' envelope */
    float vlp, va;                      /* the velocity's brightness on the dry voice: its low-pass and coefficient */
    float scan;                         /* how far the grains' position has moved since the note began (samples) */
    float gnext;                        /* samples to the next grain */
    uint32_t age;
} voice_t;

typedef struct {
    uint8_t on, v;
    dec_t d;
    float s0, s1, frac, rmul, step, ph, inc, amp, pl, pr, gl, gr;   /* gl, gr: its gain this chunk, each side */
} grain_t;

static struct {
    gparams_t p;
    voice_t v[G_NV];
    grain_t g[G_NG];
    uint32_t rng, age;
    float lfo_ph, wob1, wob2;
    float tlpl, tlpr, ta;               /* the tone filter */
    float level;
    uint32_t dl_w, rv_w;
    float dl_lpl, dl_lpr;
    float rv_d[4], rv_g[4], rv_lp[4], rv_mph;
    float rv_hpl, rv_hpr;
    float gain_s;
    uint32_t glimit, gcalm;             /* grains allowed now (g_shed lowers it, a calm second raises it again) */
} E;
static const float RV_BASE[4] = {1559.0f, 1907.0f, 2341.0f, 2789.0f};

/* for the screen */
static volatile uint8_t disp_pos[G_NG], disp_lvl[G_NG], disp_v[G_NG], disp_n;

static inline float rnd(void)           /* -1 .. 1 */
{
    E.rng ^= E.rng << 13; E.rng ^= E.rng >> 17; E.rng ^= E.rng << 5;
    return (float)(int32_t)E.rng * (1.0f / 2147483648.0f);
}
static inline float rndu(void) { return 0.5f + 0.5f * rnd(); }
static inline float chunk_coef(float secs) { return fm_expf(-(float)CHUNK / (fm_maxf(secs, 0.001f) * SR)); }
static inline float exp_range(float x, float lo, float hi) { return lo * fm_powf(hi / lo, x); }

void g_init(void)
{
    gparams_t d;
    zero(&E, sizeof E);
    zero(rv_line, sizeof rv_line);
    zero(dl_buf, sizeof dl_buf);
    zero(&d, sizeof d);
    d.tone = 0.7f; d.dyn = 0.6f; d.mix = 0.3f; d.size = 0.5f; d.dens = 0.3f; d.pos = 0.2f; d.rel = 0.4f; d.pedal = 0.4f;
    d.lrate = 0.3f; d.rsize = 0.5f; d.rmix = 0.3f;
    pbuf[0] = pbuf[1] = d;
    E.p = d;
    E.rng = 0x2545F491u;
    E.ta = 0.9f;
    E.glimit = G_NG;
}

void g_shed(void)
{
    if (E.glimit > 10u) E.glimit -= 2u;
    E.gcalm = 0;
}

/* ------------------------------------------------------------------ notes --- */
static void voice_strike(voice_t *v, int note, float vel)
{
    const gparams_t *p = &E.p;
    float bright;
    v->note = (uint8_t)note; v->vel = vel; v->held = 1; v->on = 1; v->age = ++E.age;
    v->e = (uint8_t)smp_nearest((float)note);
    /* velocity: louder and brighter, as much as DYNAMICS says */
    v->amp = (1.0f - p->dyn) * 0.75f + p->dyn * (0.15f + 0.85f * vel * vel);
    bright = 1.0f - p->dyn * (1.0f - vel);
    v->va = 1.0f - fm_expf(-6.2831853f * exp_range(bright, 900.0f, 20000.0f) / SR);
    v->pan = fm_clampf(((float)note - 66.0f) * (1.0f / 40.0f), -0.6f, 0.6f);
    dec_start(&v->dry, v->e);
    v->d0 = 0.0f; v->d1 = (float)dec_next(&v->dry); v->dfrac = 0.0f; v->dg = 1.0f; v->dry_on = 1;
    v->denv = 1.0f; v->genv = 1.0f;
    v->scan = 0.0f;
    v->gnext = 0.0f;
}

static void note_on(int note, float vel)
{
    int i, best = -1;
    uint32_t oldest = 0xFFFFFFFFu;
    if (!smp_n) return;
    for (i = 0; i < G_NV; i++)          /* the same note: struck again */
        if (E.v[i].on && E.v[i].note == note) { voice_strike(&E.v[i], note, vel); return; }
    for (i = 0; i < G_NV; i++)
        if (!E.v[i].on) { E.v[i].aenv = 0.0f; voice_strike(&E.v[i], note, vel); return; }
    for (i = 0; i < G_NV; i++)          /* none free: the oldest released one, else the oldest */
        if (!E.v[i].held && E.v[i].age < oldest) { oldest = E.v[i].age; best = i; }
    if (best < 0)
        for (i = 0; i < G_NV; i++)
            if (E.v[i].age < oldest) { oldest = E.v[i].age; best = i; }
    for (i = 0; i < G_NG; i++)          /* its grains go with it */
        if (E.g[i].on && E.g[i].v == best) E.g[i].on = 0;
    E.v[best].ng = 0;
    E.v[best].aenv = 0.0f;
    voice_strike(&E.v[best], note, vel);
}

static void run_commands(void)
{
    int i;
    while (cq_tail != cq_head) {
        cmd_t c = cq[cq_tail];
        cq_tail = (cq_tail + 1u) % QN;
        switch (c.t) {
        case CMD_ON: note_on(c.note, c.vel); break;
        case CMD_OFF:
            for (i = 0; i < G_NV; i++)
                if (E.v[i].on && E.v[i].note == c.note) E.v[i].held = 0;
            break;
        case CMD_ALLOFF:
            for (i = 0; i < G_NV; i++) E.v[i].held = 0;
            break;
        case CMD_PANIC:
            for (i = 0; i < G_NV; i++) { E.v[i].on = 0; E.v[i].ng = 0; }
            for (i = 0; i < G_NG; i++) E.g[i].on = 0;
            zero(rv_line, sizeof rv_line);
            zero(dl_buf, sizeof dl_buf);
            break;
        }
    }
}

/* a new grain from voice vi */
static void spawn(int vi, float lfo, float size_s, float gnorm)
{
    const gparams_t *p = &E.p;
    voice_t *v = &E.v[vi];
    const smp_ent_t *e = &smp_e[v->e];
    grain_t *g = 0;
    float len = (float)e->len, loop = (float)e->loop, at, semis, pan, sp2 = p->spray * p->spray;
    uint32_t ip, busy = 0;
    int i;
    for (i = 0; i < G_NG; i++) {
        if (E.g[i].on) busy++;
        else if (!g) g = &E.g[i];
    }
    if (busy >= E.glimit) return;       /* (fewer, after g_shed) */
    if (!g) return;                     /* all busy: this one is skipped */
    at = p->pos * (len - 1.0f) + v->scan;
    if (p->ltarget == LFO_POS) at += lfo * p->ldepth * 0.35f * len;
    at += rnd() * (0.004f + 0.5f * sp2) * len;
    if (at < 0.0f) at = -at;
    while (at >= len) at -= (len - loop);   /* past the end: into the loop at the tail */
    ip = (uint32_t)at;
    dec_seek(&g->d, v->e, ip);
    g->s0 = (float)dec_next(&g->d);
    g->s1 = (float)dec_next(&g->d);
    g->frac = 0.0f;
    semis = rnd() * sp2 * 0.6f;         /* scattered in pitch a little; past the middle, octaves and fifths now and then */
    if (p->spray > 0.55f && rndu() < (p->spray - 0.55f) * 1.4f) {
        float r = rndu();
        semis += r < 0.6f ? 12.0f : r < 0.85f ? 7.0f : r < 0.95f ? 19.0f : -12.0f;
    }
    g->rmul = fm_exp2f(semis * (1.0f / 12.0f));
    g->ph = 0.0f;
    g->inc = 1.0f / (size_s * SR * (1.0f + 0.35f * p->spray * rnd()));
    pan = fm_clampf(v->pan * 0.6f + rnd() * (0.2f + 0.7f * p->spray), -1.0f, 1.0f);
    g->pl = fm_sqrtf(0.5f * (1.0f - pan));
    g->pr = fm_sqrtf(0.5f * (1.0f + pan));
    g->amp = gnorm * (0.75f + 0.25f * rndu()) * (float)ck_gain[ck_off[v->e] + (ip >> CK_SH)] * (1.0f / 256.0f);
    g->v = (uint8_t)vi;
    g->on = 1;
    v->ng++;
}

static inline float window(float ph)    /* 0 .. 1 .. 0, smooth at both ends */
{
    float x = 4.0f * ph * (1.0f - ph);
    return x * x * (3.0f - 2.0f * x);
}

static inline float rd_line(const float *b, uint32_t n, uint32_t w, float d)   /* fractional read d behind w */
{
    float rp = (float)w - d, fr;
    int32_t i0, i1;
    if (rp < 0.0f) rp += (float)n;
    if (rp < 0.0f) rp = 0.0f;
    i0 = (int32_t)rp;
    fr = rp - (float)i0;
    if (i0 >= (int32_t)n) i0 -= (int32_t)n;
    i1 = i0 + 1; if (i1 >= (int32_t)n) i1 = 0;
    return b[i0] + (b[i1] - b[i0]) * fr;
}

/* ------------------------------------------------------------------ the render --- */
void g_render(int32_t *lr, uint32_t n, float gain)
{
    uint32_t c0, s;
    int i;
    run_commands();
    E.p = pbuf[pcur];
    if (E.glimit < G_NG && ++E.gcalm >= 344u) { E.glimit++; E.gcalm = 0; }   /* two calm seconds: one more */
    for (c0 = 0; c0 < n; c0 += CHUNK) {
        const gparams_t *p = &E.p;
        uint32_t cn = n - c0 < CHUNK ? n - c0 : CHUNK;
        float lfo, pmul, wob, size_s, dens_hz, gnorm, interval, dryg, grg, fc, atk_step, d_rel, g_rel, scan_k;
        float felt_atk, rsc, t60, lpan = 0.0f, send_d, fb_d, wet, rv_dd[4];
        uint8_t live[G_NG];
        int ng_live = 0, j;
        /* ---- per chunk: the LFO, the wobble, the filters, the envelopes, the grains' births */
        E.lfo_ph += (float)cn * exp_range(p->lrate, 0.05f, 8.0f) / SR;
        if (E.lfo_ph >= 1.0f) E.lfo_ph -= 1.0f;
        lfo = fm_sin_turns(E.lfo_ph);
        E.wob1 += (float)cn * 0.53f / SR; if (E.wob1 >= 1.0f) E.wob1 -= 1.0f;
        E.wob2 += (float)cn * 1.37f / SR; if (E.wob2 >= 1.0f) E.wob2 -= 1.0f;
        wob = p->wobble * (24.0f * fm_sin_turns(E.wob1) + 9.0f * fm_sin_turns(E.wob2));   /* cents */
        pmul = wob;
        if (p->ltarget == LFO_PITCH) pmul += lfo * p->ldepth * 50.0f;   /* half a semitone at most */
        pmul = fm_exp2f(pmul * (1.0f / 1200.0f));
        if (p->ltarget == LFO_PAN) lpan = lfo * p->ldepth;
        size_s = exp_range(p->size, 0.015f, 0.6f);
        dens_hz = p->dens * p->dens * 70.0f;
        gnorm = 1.5f / fm_sqrtf(1.0f + 0.7f * dens_hz * size_s);
        interval = dens_hz > 0.05f ? SR / dens_hz : 1e9f;
        dryg = fm_minf(1.0f, 2.0f * (1.0f - p->mix));
        grg = fm_minf(1.0f, 2.0f * p->mix) * (dens_hz > 0.05f ? 1.0f : 0.0f);
        fc = exp_range(p->tone, 450.0f, 18000.0f) * (1.0f - 0.82f * p->felt);
        if (p->ltarget == LFO_FILTER) fc *= fm_exp2f(lfo * p->ldepth * 2.0f);
        fc = fm_clampf(fc, 60.0f, 19000.0f);
        E.ta = 1.0f - fm_expf(-6.2831853f * fc / SR);
        felt_atk = p->felt * 0.035f;    /* felt: the hammer softened */
        atk_step = (float)cn / (fm_maxf(exp_range(p->atk, 0.001f, 4.0f), felt_atk + 0.001f) * SR);
        d_rel = chunk_coef(0.12f + p->pedal * p->pedal * 8.0f);
        g_rel = chunk_coef(exp_range(p->rel, 0.05f, 8.0f));
        scan_k = (1.0f - p->freeze) * (1.0f - p->freeze);
        for (i = 0; i < G_NV; i++) {
            voice_t *v = &E.v[i];
            if (!v->on) continue;
            v->aenv = fm_minf(1.0f, v->aenv + atk_step);
            if (!v->held) { v->denv *= d_rel; v->genv *= g_rel; }
            v->scan += (float)cn * smp_ratio * scan_k;
            {
                const smp_ent_t *e = &smp_e[v->e];
                float len = (float)e->len, span = len - (float)e->loop;
                while (p->pos * (len - 1.0f) + v->scan >= len) v->scan -= span;
            }
            if (v->dry_on && (v->dg < 0.002f || v->denv < 0.0005f)) v->dry_on = 0;
            if (grg > 0.0f && v->genv > 0.002f) {
                v->gnext -= (float)cn;
                while (v->gnext <= 0.0f) {
                    spawn(i, lfo, size_s, gnorm);
                    v->gnext += interval * (1.0f + (0.15f + 0.8f * p->spray) * rnd());
                    if (v->gnext < 8.0f) v->gnext = 8.0f;
                }
            }
            /* done: let go, the piano silent, no grain left and none to come. A held voice stays (silent, it
             * may be), so its grains come back when DENSITY or MIX are turned up again */
            if (!v->held && !v->dry_on && v->ng == 0 && (v->genv <= 0.002f || grg <= 0.0f)) v->on = 0;
            v->rate = fm_exp2f(((float)v->note - (float)smp_e[v->e].root) * (1.0f / 12.0f)) * smp_ratio * pmul;
        }
        ng_live = 0;
        for (i = 0; i < G_NG; i++)      /* the grains sounding: their step and gain for this chunk, in a short list */
            if (E.g[i].on) {
                grain_t *g = &E.g[i];
                const voice_t *v = &E.v[g->v];
                float k = g->amp * v->genv * v->aenv * v->amp * grg * (1.0f / 32768.0f);
                g->step = v->rate * g->rmul;
                g->gl = k * g->pl;
                g->gr = k * g->pr;
                live[ng_live++] = (uint8_t)i;
            }
        rsc = 0.35f + 1.25f * p->rsize;
        t60 = 0.6f + fm_powf(p->rsize, 1.5f) * 14.0f;
        for (i = 0; i < 4; i++) {
            float target = RV_BASE[i] * rsc;
            E.rv_d[i] += (target - E.rv_d[i]) * 0.02f;   /* the size glides (turning it bends the room a little) */
            if (E.rv_d[i] < 100.0f) E.rv_d[i] = target;
            E.rv_g[i] = fm_exp2f(-9.965784f * E.rv_d[i] / (SR * t60));
        }
        E.rv_mph += (float)cn * 0.6f / SR; if (E.rv_mph >= 1.0f) E.rv_mph -= 1.0f;
        for (i = 0; i < 4; i++)         /* the lines' slow wander (no metallic ring), per chunk */
            rv_dd[i] = E.rv_d[i] + (i & 1 ? 3.0f : -3.0f) * fm_sin_turns(E.rv_mph + 0.25f * (float)i) + 4.0f;
        send_d = p->delay * 0.8f;
        fb_d = 0.2f + 0.55f * p->delay;
        wet = p->rmix;
        /* ---- per sample */
        for (s = 0; s < cn; s++) {
            float L = 0.0f, R = 0.0f;
            for (i = 0; i < G_NV; i++) {   /* the dry piano */
                voice_t *v = &E.v[i];
                float o, gv;
                if (!v->on || !v->dry_on) continue;
                v->dfrac += v->rate;
                while (v->dfrac >= 1.0f) {
                    uint32_t was = v->dry.pos;
                    v->d0 = v->d1; v->d1 = (float)dec_next(&v->dry); v->dfrac -= 1.0f;
                    if (v->dry.pos < was) v->dg *= 0.7f;   /* back at the loop: it fades on, as a piano does */
                }
                o = (v->d0 + (v->d1 - v->d0) * v->dfrac) * (1.0f / 32768.0f);
                v->vlp += v->va * (o - v->vlp);
                gv = v->vlp * v->dg * v->denv * v->aenv * v->amp * dryg;
                L += gv * (0.7f - 0.4f * v->pan);
                R += gv * (0.7f + 0.4f * v->pan);
            }
            for (j = 0; j < ng_live; j++) {   /* the grains */
                grain_t *g = &E.g[live[j]];
                float o;
                if (!g->on) continue;
                g->ph += g->inc;
                if (g->ph >= 1.0f) { voice_t *v = &E.v[g->v]; g->on = 0; if (v->ng) v->ng--; continue; }
                g->frac += g->step;
                while (g->frac >= 1.0f) { g->s0 = g->s1; g->s1 = (float)dec_next(&g->d); g->frac -= 1.0f; }
                o = (g->s0 + (g->s1 - g->s0) * g->frac) * window(g->ph);
                L += o * g->gl;
                R += o * g->gr;
            }
            if (lpan != 0.0f) { float l = L, r = R; L = l * (1.0f - lpan * 0.8f); R = r * (1.0f + lpan * 0.8f); }
            /* the tone */
            E.tlpl += E.ta * (L - E.tlpl);
            E.tlpr += E.ta * (R - E.tlpr);
            L = E.tlpl; R = E.tlpr;
            if (p->felt > 0.0f) {       /* felt: a little warmth (soft saturation) */
                float k = 1.0f + 2.0f * p->felt;
                L += p->felt * (fm_tanhf(L * k) / k - L) * 0.6f;
                R += p->felt * (fm_tanhf(R * k) / k - R) * 0.6f;
            }
            /* the ping-pong delay */
            {
                float dlo, dro, in = (L + R) * 0.5f * send_d;
                uint32_t rl = E.dl_w + DL_N - DL_L, rr = E.dl_w + DL_N - DL_R;
                if (rl >= DL_N) rl -= DL_N;
                if (rr >= DL_N) rr -= DL_N;
                dlo = (float)dl_buf[0][rl] * (1.0f / 32767.0f);
                dro = (float)dl_buf[1][rr] * (1.0f / 32767.0f);
                E.dl_lpl += 0.35f * (dro - E.dl_lpl);   /* each repeat darker */
                E.dl_lpr += 0.35f * (dlo - E.dl_lpr);
                dl_buf[0][E.dl_w] = (int16_t)(fm_clampf(in + fb_d * E.dl_lpl, -1.0f, 1.0f) * 32767.0f);
                dl_buf[1][E.dl_w] = (int16_t)(fm_clampf(fb_d * E.dl_lpr, -1.0f, 1.0f) * 32767.0f);
                if (++E.dl_w >= DL_N) E.dl_w = 0u;
                L += dlo * 0.85f;
                R += dro * 0.85f;
            }
            /* the reverb: four lines, a Hadamard mix, damped, its size and decay from ROOM */
            if (wet > 0.001f) {
                float y[4], a, b, c, d, h[4], x = (L + R) * 0.18f, ol, orr;
                int k;
                for (k = 0; k < 4; k++) {
                    y[k] = rd_line(rv_line[k], RV_MAXL, E.rv_w, rv_dd[k]);
                    E.rv_lp[k] += 0.5f * (y[k] - E.rv_lp[k]);
                }
                a = E.rv_lp[0] + E.rv_lp[1]; b = E.rv_lp[0] - E.rv_lp[1];
                c = E.rv_lp[2] + E.rv_lp[3]; d = E.rv_lp[2] - E.rv_lp[3];
                h[0] = 0.5f * (a + c); h[1] = 0.5f * (b + d); h[2] = 0.5f * (a - c); h[3] = 0.5f * (b - d);
                for (k = 0; k < 4; k++)
                    rv_line[k][E.rv_w] = h[k] * E.rv_g[k] + (k & 1 ? -x : x);
                if (++E.rv_w >= RV_MAXL) E.rv_w = 0u;
                ol = E.rv_lp[0] + 0.6f * E.rv_lp[2];
                orr = E.rv_lp[1] + 0.6f * E.rv_lp[3];
                E.rv_hpl += 0.004f * (ol - E.rv_hpl); ol -= E.rv_hpl;   /* no rumble */
                E.rv_hpr += 0.004f * (orr - E.rv_hpr); orr -= E.rv_hpr;
                L = L * (1.0f - 0.45f * wet) + ol * wet * 1.6f;
                R = R * (1.0f - 0.45f * wet) + orr * wet * 1.6f;
            }
            /* out: a soft limit, then MASTER */
            L = fm_tanhf(L * 1.1f);
            R = fm_tanhf(R * 1.1f);
            {
                float lv = fm_fabsf(L) + fm_fabsf(R);
                E.level += (lv * 0.5f - E.level) * 0.0005f;
            }
            E.gain_s += (gain - E.gain_s) * 0.004f;
            lr[2u * (c0 + s)] = (int32_t)(L * E.gain_s);
            lr[2u * (c0 + s) + 1u] = (int32_t)(R * E.gain_s);
        }
        E.tlpl = fm_flush(E.tlpl); E.tlpr = fm_flush(E.tlpr);
        E.dl_lpl = fm_flush(E.dl_lpl); E.dl_lpr = fm_flush(E.dl_lpr);
        for (i = 0; i < 4; i++) E.rv_lp[i] = fm_flush(E.rv_lp[i]);
        for (i = 0; i < G_NV; i++) E.v[i].vlp = fm_flush(E.v[i].vlp);
    }
    {                                   /* for the screen */
        uint8_t m = 0;
        for (i = 0; i < G_NG; i++) {
            const grain_t *g = &E.g[i];
            float lv;
            if (!g->on) continue;
            disp_pos[m] = (uint8_t)((uint32_t)g->d.pos * 255u / (g->d.len ? g->d.len : 1u));
            lv = window(g->ph) * fm_minf(1.0f, g->amp * E.v[g->v].genv * E.v[g->v].amp);
            disp_lvl[m] = (uint8_t)(fm_clampf(lv, 0.0f, 1.0f) * 255.0f);
            disp_v[m] = g->v;
            m++;
        }
        disp_n = m;
    }
}

int g_grains(uint8_t *pos, uint8_t *lvl, uint8_t *voice, int max)
{
    int i, m = disp_n;
    if (m > max) m = max;
    for (i = 0; i < m; i++) { pos[i] = disp_pos[i]; lvl[i] = disp_lvl[i]; voice[i] = disp_v[i]; }
    return m;
}

int g_voice_pos(int v, uint8_t *note, uint8_t *pos)
{
    const voice_t *x;
    float len, at;
    if (v < 0 || v >= G_NV || !E.v[v].on || !smp_n) return 0;
    x = &E.v[v];
    len = (float)smp_e[x->e].len;
    at = E.p.pos * (len - 1.0f) + x->scan;
    if (at < 0.0f) at = 0.0f;
    if (at >= len) at = len - 1.0f;
    *note = x->note;
    *pos = (uint8_t)(at * 255.0f / len);
    return 1;
}

float g_level(void) { return E.level; }
