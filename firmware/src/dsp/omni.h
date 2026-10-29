/* SPDX-License-Identifier: GPL-3.0-only */
/* BASSLINE: the instrument. A monophonic bass with glide and legato, a 1-8 bar loop recorder and a
 * bass arpeggiator, for writing bass lines against a chord progression (or for finding one).
 *
 *   VOICE   one note at a time. Ten sounds: an 808, electric basses (finger, pick, slap, muted),
 *           fretless and upright (plucked-string models), acid, sub and reese (oscillators).
 *           Glide between notes, legato (overlapping notes slide without restarting), a forced
 *           slide (ENV), and the 808's pitch dive (a scoop on the others).
 *   LOOP    1-8 bars of 16 steps. Each step holds one note: its pitch, its length in steps, and
 *           whether it slides in. The render plays it, and records into it (live, quantized to the
 *           step) when recording; step entry is written by the UI.
 *   ARP     bass patterns from the held note's chord in the key (root, octaves, fifths, walks...).
 *
 * Threads: om_render() runs in the audio interrupt. Everything else posts commands (om_*), which the
 * render drains at the start of a block. om_pat[] is shared: the render reads it while playing and
 * writes it while recording; the UI writes it only when the loop is stopped or not recording.
 * Float only, no libm: fastmath.h. */
#pragma once
#include <stdint.h>

#define OM_SR 44100.0f
#define OM_BAR 16                      /* steps in a bar */
#define OM_MAXBARS 8
#define OM_STEPS (OM_BAR * OM_MAXBARS)

/* ---- music theory (pure functions, shared with the UI) */
enum { NSCALES = 11, NSOUNDS = 10, NARPPAT = 9 };
extern const char *const OM_SCALE_NAME[NSCALES];
extern const char *const OM_SOUND_NAME[NSOUNDS];
extern const char *const OM_NOTE_NAME[12];
extern const char *const OM_ROOT_NAME[12];
extern const char *const OM_ARP_NAME[NARPPAT];
int om_scale_len(int scale);
int om_scale_step(int scale, int i);
int om_scale_note(int scale, int root, int oct, int i);
int om_in_scale(int scale, int root, int note);
/* the chord on scale degree deg: n notes (3 triad, 4 seventh) stacked in thirds of the scale, as MIDI
 * notes from octave oct (3: C4 is 60). Returns n. */
int om_chord_notes(int scale, int root, int oct, int deg, int n, uint8_t *out);
void om_chord_quality(const uint8_t *notes, int n, char *buf);   /* "m", "maj7", "m7b5"... (16 B) */
void om_roman(int deg, int minor, char *buf);
/* the chord tones (semitones over the root: 0, third, fifth[, seventh]) of the chord the key gives the
 * note `note`: its scale degree's chord, or a power chord when the note is outside the scale */
int om_note_chord(int scale, int root, int note, int seventh, int8_t *tones);
/* arp pattern pat (1..NARPPAT-1), its i-th note over root, from the chord tones; rng for Random */
int om_arp_note(int pat, int i, int root, const int8_t *tones, int ntones, int scale, int key, uint32_t *rng);
int om_arp_len(int pat);               /* notes in one cycle of the pattern */

/* ---- the loop: one 32-bit word per step, written whole */
#define PAT_NOTE(w) ((w) & 0x7Fu)                       /* 0: no note starts here */
#define PAT_LEN(w) (((w) >> 8) & 0xFFu)                 /* steps */
#define PAT_GLIDE (1u << 16)                            /* slides in from the note before */
#define PAT_ACC (1u << 17)                              /* accent */
#define PAT_MAKE(n, len, fl) ((uint32_t)(n) | (uint32_t)(len) << 8 | (fl))
extern volatile uint32_t om_pat[OM_STEPS];

/* ---- parameters: value ranges in om_param_info() */
enum {
    P_TONE, P_DECAY, P_DRIVE, P_LEVEL,                 /* SOUND */
    P_GLIDE, P_MODE, P_SLIDE, P_DIVE,                  /* GLIDE */
    P_BARS, P_TEMPO, P_SWING, P_METRO,                 /* LOOP */
    P_ARPPAT, P_ARPRATE, P_ARPGATE, P_STEPLEN,         /* ARP (+ step entry's note length) */
    P_COUNTIN, P_MIDI, P_TUNE, P_SNAP,                 /* SETUP (+ snap, on the Prog page) */
    P_SCALE, P_ROOT, P_OCTAVE, P_SOUND,                /* the key and the sound (encoders) */
    P_PUNCH, P_DIVETIME, P_CHAR, P_TAIL,               /* SHAPE (0.2; new ones go last: saved projects load) */
    P_RETURN, P_WOBBLE, P_WRATE,                       /* BEND (with the bend knob itself, live) */
    P_NPARAMS
};
enum { MODE_RETRIG, MODE_LEGATO, MODE_GLIDEALL };
enum { SNAP_OFF, SNAP_SCALE, SNAP_CHORD };
enum { CHAR_WARM, CHAR_HARD, CHAR_FOLD, CHAR_FUZZ };
#define OM_NPARAMS_V1 24                   /* the parameters 0.1 saved */
typedef struct {
    const char *name;
    int16_t lo, hi, def;
} om_param_t;
const om_param_t *om_param_info(int i);
void om_param_text(int i, int v, char *buf);   /* the value as the screen shows it (12 bytes) */
int om_arp_rate_steps(int rate);               /* the rate in steps (0 for the triplet) */

/* ---- commands (main loop -> render) */
enum { SRC_KEYS, SRC_MIDI };
void om_init(void);
void om_set(int param, int value);
void om_note(int note, int on, int src, int slide);   /* slide: glide into it whatever the mode (ENV) */
void om_play(int on, int rec, int countin_bars);      /* start (from the loop's start) / stop */
void om_rec(int on);                                   /* recording on / off while playing */
void om_bend(int cents);                               /* MIDI pitch bend */
void om_knob_bend(int cents);                          /* the Bend knob (adds to MIDI's) */
void om_panic(void);

/* the render (audio ISR): n stereo frames, 24-bit in int32; gain Q12 (the MASTER pot) */
void om_render(int32_t *out_lr, uint32_t n, uint32_t gain_q12);

/* MIDI out, from the render: the app sends it (USB); status, data 1, data 2 */
void om_midi_out(uint32_t st, uint32_t d1, uint32_t d2);

/* ---- state for the screen (UI) */
extern volatile uint8_t om_playing, om_recording, om_counting;   /* counting: beats of count-in left */
extern volatile uint8_t om_step;               /* the step playing */
extern volatile float om_step_frac;            /* how far into it, 0..1 */
extern volatile uint8_t om_note_on;            /* the note sounding (gate on), 0 none */
extern volatile float om_level, om_env;        /* output level; the voice's envelope */
extern volatile uint8_t om_arp_on;
extern volatile uint32_t om_rec_writes;        /* notes the render has recorded (the UI marks unsaved) */
