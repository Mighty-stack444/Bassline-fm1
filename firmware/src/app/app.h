/* SPDX-License-Identifier: GPL-3.0-only */
/* BASSLINE app: the project (everything that is saved) and the app's entry points. Included by the unity
 * build after gfx.c (drawing helpers in scope) and by the host simulator. */
#pragma once
#include <stdint.h>
#include "plat.h"
#include "../dsp/omni.h"

#ifndef OM_VERSION                       /* a release build passes its own (tools/build.py --release) */
#define OM_VERSION "DEV"
#endif
#define PROJ_MAGIC 0x53534142u           /* "BASS" */
#define PROJ_FORMAT 1u

typedef struct {
    uint32_t magic, format;
    int16_t par[P_NPARAMS];              /* omni.h P_* */
    uint8_t prog[OM_MAXBARS];            /* the progression: per bar 0 none, else degree + 1; bit 7: a 7th */
    uint8_t arp_last;                    /* the pattern ARP turns back on */
    uint8_t leds;                        /* key lights: 0 off, 1 the bar's chord, 2 the scale */
    uint8_t rsv[14];
    uint32_t pat[OM_STEPS];              /* the loop (om_pat), copied in and out on load and save */
} project_t;

extern project_t proj;

void project_defaults(void);
int project_load(void);                  /* 0 = loaded; else defaults are in place */
int project_save(void);                  /* 0 = saved and verified */
void project_apply(void);                /* every value to the engine (boot, load) */

void ui_init(void);
void ui_frame(void);
void ui_input_only(void);
int ui_dirty(void);
void ui_say(const char *a, const char *b);
