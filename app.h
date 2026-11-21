/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 swtdr34mz */
/* GRAIN PIANO by swtdr34mz: a sampled grand piano broken into grains, for the M-VAVE FM-1 (on fm1-base).
 * This header is the app's half of the contract with the platform (device/unity.c, host/plat_host.c). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "GRAIN PIANO"
#define APP_SPLASH_RGB RGB(244, 214, 160)
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */
#define APP_OVERLOAD                     /* app_overload: the render over 90 % of its time */
#define APP_TOUCH                        /* velocity from the keys (lib/touch.h) */

/* the store: the app's settings and the panel calibration (the platform's), and their flash offsets (as CHORD RUN's) */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u}

/* the grand piano's samples (res/piano.bin) are in the image itself (tools/gen_piano_h.py), not in the resource region:
 * nothing to send apart, nothing that can be missing */

/* backup and restore over USB: the settings */
#define APP_BACKUP_OBJS {OBJ_SETTINGS}
#define APP_BACKUP_MAGIC 0x4E415247u            /* "GRAN" */
#define APP_BACKUP_VERSION 1u
#define APP_BACKUP_WHAT "replaces settings"
int app_backup_playing(void);
void app_backup_flush(void);
void app_backup_hold(int on);
void app_backup_restored(void);

void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);
void app_overload(void);

#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
