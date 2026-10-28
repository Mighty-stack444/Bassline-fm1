/* SPDX-License-Identifier: GPL-3.0-only */
/* BASSLINE: the instrument (omni.h). One compilation unit, -O2.
 *
 * The plucked basses are Karplus-Strong strings: a delay line one period long, fed back through a
 * lowpass, excited by a burst shaped per sound (a finger's soft noise, a pick's bright one, a
 * slap's). Gliding changes the delay's length while the string rings, so slides are the string's
 * own. The 808 is a sine with a fast punch and a slower dive; acid a sawtooth through a 4-pole
 * resonant lowpass with its envelope; sub a sine with its overtones; reese two detuned saws. */
#include "omni.h"
#include "fastmath.h"

#ifdef OM_HOST
#define OM_POOL
#else
#define OM_POOL __attribute__((section(".pool")))
#endif
#define BARRIER() __asm__ volatile("" ::: "memory")

/* ============================================================== theory === */
const char *const OM_NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const char *const OM_ROOT_NAME[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
const char *const OM_SCALE_NAME[NSCALES] = {"Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian",
                                            "Harmonic Min", "Melodic Min", "Pent. Major", "Pent. Minor", "Blues"};
static const uint8_t SCALE_LEN[NSCALES] = {7, 7, 7, 7, 7, 7, 7, 7, 5, 5, 6};
static const uint8_t SCALE[NSCALES][7] = {
    {0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 9, 10}, {0, 1, 3, 5, 7, 8, 10},
    {0, 2, 4, 6, 7, 9, 11}, {0, 2, 4, 5, 7, 9, 10}, {0, 2, 3, 5, 7, 8, 11}, {0, 2, 3, 5, 7, 9, 11},
    {0, 2, 4, 7, 9},        {0, 3, 5, 7, 10},       {0, 3, 5, 6, 7, 10}};
const char *const OM_SOUND_NAME[NSOUNDS] = {"808", "Finger", "Pick", "Slap", "Muted",
                                            "Fretless", "Upright", "Acid", "Sub", "Reese"};
enum { S_808, S_FINGER, S_PICK, S_SLAP, S_MUTED, S_FRETLESS, S_UPRIGHT, S_ACID, S_SUB, S_REESE };
const char *const OM_ARP_NAME[NARPPAT] = {"Off", "Root", "Octave", "Root-5th", "R-5-8-5",
                                          "R-3-5-3", "Walk", "Pump", "Random"};

int om_scale_len(int s) { return SCALE_LEN[(unsigned)s < NSCALES ? s : 0]; }
int om_scale_step(int s, int i)
{
    s = (unsigned)s < NSCALES ? s : 0;
    return SCALE[s][(unsigned)i < SCALE_LEN[s] ? i : 0];
}
static int clamp_note(int n) { return n < 0 ? 0 : n > 127 ? 127 : n; }
int om_scale_note(int s, int root, int oct, int i)
{
    int len = om_scale_len(s);
    return clamp_note(root + om_scale_step(s, i % len) + (oct + 1 + i / len) * 12);
}
int om_in_scale(int s, int root, int note)
{
    int pc = ((note - root) % 12 + 12) % 12, i;
    for (i = 0; i < om_scale_len(s); i++)
        if (om_scale_step(s, i) == pc)
            return 1;
    return 0;
}
int om_chord_notes(int s, int root, int oct, int deg, int n, uint8_t *out)
{
    int i;
    if (n > 6)
        n = 6;
    for (i = 0; i < n; i++)
        out[i] = (uint8_t)om_scale_note(s, root, oct, deg + 2 * i);
    return n;
}

static void scat(char *b, const char *s)
{
    while (*b)
        b++;
    while ((*b++ = *s++))
        ;
}

/* the chord's name from its intervals (CHORDS's, after the Chord Generator's cQ()) */
void om_chord_quality(const uint8_t *mn, int n, char *q)
{
    int has_v[32] = {0}, i, s = -1, im, iD, iA, i4, i2;
    q[0] = 0;
    if (n < 2)
        return;
    for (i = 1; i < n; i++) {
        int d = mn[i] - mn[0];
        if (d >= 0 && d < 32)
            has_v[d] = 1;
    }
    if (n < 3) {
        int iv = mn[1] - mn[0];
        scat(q, iv == 7 ? "5" : iv == 3 ? "m" : "");
        return;
    }
    im = has_v[3] && has_v[7];
    iD = has_v[3] && has_v[6];
    iA = has_v[4] && has_v[8];
    i4 = has_v[5] && has_v[7];
    i2 = has_v[2] && has_v[7];
    scat(q, iD ? "dim" : iA ? "aug" : im ? "m" : i4 ? "sus4" : i2 ? "sus2" : "");
    if (n >= 4) {
        for (i = 9; i <= 11; i++)
            if (has_v[i]) {
                s = i;
                break;
            }
        if (s == 11) {
            if (!iD)
                scat(q, "maj7");
        } else if (s == 10) {
            if (iD) {
                q[0] = 0;
                scat(q, "m7b5");
            } else {
                scat(q, "7");
            }
        } else if (s == 9 && iD) {
            q[0] = 0;
            scat(q, "dim7");
        }
    }
}

void om_roman(int deg, int minor, char *b)
{
    static const char *const ROM[7] = {"I", "II", "III", "IV", "V", "VI", "VII"};
    const char *r = ROM[(unsigned)deg < 7u ? deg : 0];
    while (*r) {
        *b++ = minor ? (char)(*r + ('a' - 'A')) : *r;
        r++;
    }
    *b = 0;
}

int om_note_chord(int s, int root, int note, int seventh, int8_t *t)
{
    int pc = ((note - root) % 12 + 12) % 12, i, len = om_scale_len(s), n = seventh ? 4 : 3;
    for (i = 0; i < len; i++)
        if (om_scale_step(s, i) == pc) {
            uint8_t c[4];
            int k;
            om_chord_notes(s, 0, 3, i, n, c);
            for (k = 0; k < n; k++)
                t[k] = (int8_t)(c[k] - c[0]);
            return n;
        }
    t[0] = 0;                                  /* outside the key: root, fifth, octave */
    t[1] = 7;
    t[2] = 12;
    return 3;
}

int om_arp_len(int pat)
{
    static const uint8_t LEN[NARPPAT] = {1, 1, 2, 2, 4, 4, 4, 4, 1};
    return LEN[(unsigned)pat < NARPPAT ? pat : 0];
}

int om_arp_note(int pat, int i, int root, const int8_t *t, int nt, int scale, int key, uint32_t *rng)
{
    int third = nt > 1 ? t[1] : 4, fifth = nt > 2 ? t[2] : 7, r = root;
    switch (pat) {
    case 2: return clamp_note(r + (i & 1) * 12);                   /* Octave */
    case 3: return clamp_note(r + (i & 1) * fifth);                /* Root-5th */
    case 4: {                                                      /* R-5-8-5 */
        static const int8_t K[4] = {0, 1, 2, 1};
        int k = K[i & 3];
        return clamp_note(r + (k == 0 ? 0 : k == 1 ? fifth : 12));
    }
    case 5: {                                                      /* R-3-5-3 */
        static const int8_t K[4] = {0, 1, 2, 1};
        int k = K[i & 3];
        return clamp_note(r + (k == 0 ? 0 : k == 1 ? third : fifth));
    }
    case 6: {                                                      /* Walk: up the scale, 1 2 3 5 */
        static const int8_t W[4] = {0, 1, 2, 4};
        int pc = ((r - key) % 12 + 12) % 12, d = -1, k, len = om_scale_len(scale);
        for (k = 0; k < len; k++)
            if (om_scale_step(scale, k) == pc)
                d = k;
        if (d < 0)
            return clamp_note(r + (i & 3) * 2);
        k = d + W[i & 3];
        return clamp_note(r - pc + om_scale_step(scale, k % len) + 12 * (k / len));
    }
    case 7: return clamp_note(r + ((i & 3) == 2 ? 12 : 0));       /* Pump */
    case 8: {                                                      /* Random chord tones and octave */
        uint32_t x = *rng = *rng * 1664525u + 1013904223u;
        int k = (int)((x >> 16) % (uint32_t)(nt + 1));
        return clamp_note(r + (k < nt ? t[k] : 12));
    }
    default: return clamp_note(r);                                 /* Root */
    }
}

/* ========================================================== parameters === */
static const om_param_t PARAMS[P_NPARAMS] = {
    {"Tone", 0, 100, 50},     {"Decay", 0, 100, 60},   {"Drive", 0, 100, 15},  {"Level", 0, 100, 80},
    {"Glide", 0, 100, 30},    {"Mode", 0, 2, 1},       {"Slide", 0, 100, 40},  {"Dive", 0, 24, 0},
    {"Bars", 1, 8, 2},        {"Tempo", 40, 220, 90},  {"Swing", 0, 100, 0},   {"Click", 0, 2, 1},
    {"Pattern", 0, NARPPAT - 1, 0}, {"Rate", 0, 3, 1}, {"Gate", 10, 100, 60},  {"Step", 0, 3, 0},
    {"Count-in", 0, 2, 1},    {"MIDI out", 0, 1, 1},   {"Tune", -50, 50, 0},   {"Snap", 0, 2, 0},
    {"Scale", 0, NSCALES - 1, 1}, {"Root", 0, 11, 0},  {"Octave", -3, 0, -2},  {"Sound", 0, NSOUNDS - 1, 1},
};
const om_param_t *om_param_info(int i) { return (i >= 0 && i < P_NPARAMS) ? &PARAMS[i] : &PARAMS[0]; }

static void itoa_s(int v, char *b)
{
    char t[8];
    int n = 0, neg = v < 0;
    if (neg)
        v = -v;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 6);
    if (neg)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}
static void copy_s(char *b, const char *s)
{
    int k = 0;
    while (s[k] && k < 11) {
        b[k] = s[k];
        k++;
    }
    b[k] = 0;
}

int om_arp_rate_steps(int r)
{
    static const uint8_t ST[4] = {4, 2, 0, 1};
    return ST[r & 3];
}

void om_param_text(int i, int v, char *b)
{
    static const char *const MODE[3] = {"Retrig", "Legato", "Glide all"};
    static const char *const RATE[4] = {"1/4", "1/8", "1/8T", "1/16"};
    static const char *const STEP[4] = {"1/16", "1/8", "1/4", "1/2"};
    static const char *const CLICK[3] = {"Off", "Rec", "On"};
    static const char *const CIN[3] = {"Off", "1 bar", "2 bars"};
    static const char *const SNAP[3] = {"Off", "Scale", "Chord"};
    static const char *const ONOFF[2] = {"Off", "On"};
    const char *s = 0;
    switch (i) {
    case P_MODE: s = MODE[v % 3]; break;
    case P_ARPRATE: s = RATE[v & 3]; break;
    case P_STEPLEN: s = STEP[v & 3]; break;
    case P_METRO: s = CLICK[v % 3]; break;
    case P_COUNTIN: s = CIN[v % 3]; break;
    case P_SNAP: s = SNAP[v % 3]; break;
    case P_MIDI: s = ONOFF[v ? 1 : 0]; break;
    case P_ARPPAT: s = OM_ARP_NAME[v % NARPPAT]; break;
    case P_SCALE: s = OM_SCALE_NAME[v % NSCALES]; break;
    case P_ROOT: s = OM_ROOT_NAME[v % 12]; break;
    case P_SOUND: s = OM_SOUND_NAME[v % NSOUNDS]; break;
    default: break;
    }
    if (s) {
        copy_s(b, s);
        return;
    }
    if (i == P_GLIDE || i == P_SLIDE) {          /* milliseconds */
        float f = (float)v * 0.01f;
        int ms = (int)((0.005f + f * f) * 1000.0f + 0.5f);
        itoa_s(ms, b);
        while (*b)
            b++;
        b[0] = 'm';
        b[1] = 's';
        b[2] = 0;
        return;
    }
    if (i == P_SWING) {                          /* 50 % (straight) .. 75 % */
        itoa_s(50 + v / 4, b);
        while (*b)
            b++;
        b[0] = '%';
        b[1] = 0;
        return;
    }
    if ((i == P_TUNE || i == P_OCTAVE) && v > 0)
        *b++ = '+';
    if (i == P_DIVE && v > 0)
        *b++ = '+';
    itoa_s(v, b);
    if (i == P_DIVE && v) {
        while (*b)
            b++;
        b[0] = ' ';
        b[1] = 's';
        b[2] = 't';
        b[3] = 0;
    }
    if (i == P_TONE || i == P_DECAY || i == P_DRIVE || i == P_LEVEL || i == P_ARPGATE) {
        while (*b)
            b++;
        b[0] = '%';
        b[1] = 0;
    }
}

/* ============================================================ commands === */
enum { C_SET, C_NOTE, C_PLAY, C_REC, C_BEND, C_PANIC };
#define QN 128u
static uint32_t q[QN];
static volatile uint32_t q_w, q_r;
static void post(uint32_t c, int a, int b)
{
    uint32_t w = q_w;
    if (w - q_r >= QN)
        return;
    q[w % QN] = c << 24 | ((uint32_t)a & 0xFFu) << 16 | ((uint32_t)b & 0xFFFFu);
    BARRIER();
    q_w = w + 1;
}
void om_set(int p, int v) { post(C_SET, p, v); }
void om_note(int n, int on, int src, int slide) { post(C_NOTE, n, (on ? 1 : 0) | (src & 1) << 1 | (slide ? 4 : 0)); }
void om_play(int on, int rec, int cin) { post(C_PLAY, on, (rec ? 1 : 0) | (cin & 3) << 1); }
void om_rec(int on) { post(C_REC, on, 0); }
void om_bend(int cents) { post(C_BEND, 0, cents); }
void om_panic(void) { post(C_PANIC, 0, 0); }

/* =============================================================== state === */
volatile uint32_t om_pat[OM_STEPS];
volatile uint8_t om_playing, om_recording, om_counting, om_step, om_note_on, om_arp_on;
volatile float om_step_frac, om_level, om_env;
volatile uint32_t om_rec_writes;

static int16_t par[P_NPARAMS];
static float tunefac = 1.0f, bend = 0.0f;

static void midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    if (par[P_MIDI])
        om_midi_out(st, d1, d2);
}

#define TBL 1024
static float sintab[TBL + 1];
static inline float tsin(float ph)
{
    float x = ph * (float)TBL;
    int i = (int)x;
    float f = x - (float)i;
    return sintab[i] + (sintab[i + 1] - sintab[i]) * f;
}
static inline float wrap1(float p) { return p >= 1.0f ? p - (float)(int)p : p; }
static inline float blep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}
static uint32_t nrng = 0x2468ACE1u;
static inline float noise(void)
{
    nrng = nrng * 1664525u + 1013904223u;
    return (float)(int32_t)nrng * (1.0f / 2147483648.0f);
}
static float t60_coef(float t)                 /* per-sample decay that falls 60 dB in t seconds */
{
    return fm_exp2f(-9.9658f / (fm_maxf(t, 0.001f) * OM_SR));
}
static float tc_coef(float t) { return 1.0f - fm_expf(-1.0f / (fm_maxf(t, 0.0005f) * OM_SR)); }

/* ================================================================ voice === */
#define KS_N 4096
#define KS_M (KS_N - 1)
static float ks[KS_N] OM_POOL;

enum { E_OFF, E_ATT, E_HOLD, E_REL };
static struct {
    uint8_t st, note;                          /* the note playing (or last played) */
    float pitch, target, gk;                   /* pitch in semitones; glide toward target */
    float env, att_k;                          /* amplitude */
    float punch, dive;                         /* 808 punch, dive (or scoop) envelopes, 1 -> 0 */
    float ph, ph2, ph3;
    float f1, f2, f3, f4, fenv, acc;           /* acid's filter and its envelope */
    float lp, ks_w_f;                          /* the string's loop lowpass */
    int ks_w;
    float pop, prev, mw;                       /* slap's pop; fretless's "mwah" */
    float sv1l, sv1b, sv2l, sv2b, svml, svmb;  /* upright's body, fretless's formant */
    float dt, len;                             /* per tick: phase increment, string length */
    float k_hold, k_rel, k_lp;                 /* per tick: the string's decay and brightness */
} vo;
static int sound;
static float k_punch, k_dive, k_fenv, k_pop, k_mw;

static float note_hz(float p) { return 440.0f * tunefac * fm_exp2f((p - 69.0f) * (1.0f / 12.0f)); }

static void voice_coefs(void)
{
    k_punch = tc_coef(0.012f);
    k_dive = tc_coef(sound == S_808 ? 0.16f : 0.05f);
    k_fenv = tc_coef(0.06f + 1.0f * (float)par[P_DECAY] * (float)par[P_DECAY] * 1e-4f);
    k_pop = tc_coef(0.012f);
    k_mw = tc_coef(0.22f);
}

/* the string's excitation: a burst one period long, shaped per sound, added to what the delay line
 * will play next. The old note is damped (a hand stopping the string). */
static void pluck(float amp)
{
    int L = (int)vo.len, i;
    float e = 0.0f, e2 = 0.0f, ec, tone = (float)par[P_TONE] * 0.01f;
    if (L < 2)
        L = 2;
    if (L > KS_N - 2)
        L = KS_N - 2;
    switch (sound) {
    case S_PICK: ec = 0.45f + 0.5f * tone; break;
    case S_SLAP: ec = 0.75f + 0.25f * tone; break;
    case S_MUTED: ec = 0.15f + 0.2f * tone; break;
    case S_UPRIGHT: ec = 0.08f + 0.15f * tone; break;
    case S_FRETLESS: ec = 0.15f + 0.3f * tone; break;
    default: ec = 0.18f + 0.35f * tone; break;   /* finger */
    }
    for (i = 0; i < L; i++) {
        int j = (vo.ks_w - L + i) & KS_M;
        float x = noise();
        float w = (float)i / (float)L, sh;
        e += ec * (x - e);                          /* the burst's brightness */
        e2 += ec * (e - e2);
        sh = sound == S_PICK || sound == S_SLAP ? e : e2;
        sh *= 1.0f - w * 0.6f;                      /* a little weight at the start */
        if (sound == S_FINGER || sound == S_UPRIGHT || sound == S_MUTED)
            sh += 0.6f * fm_sinf(FM_PI * w);        /* the finger's push: a soft half sine */
        ks[j] = ks[j] * 0.25f + amp * sh;
    }
    vo.lp = 0.0f;
}

static void tick_coefs(void);

/* the note starts: retrig restarts the sound, else it slides there (legato) */
static void voice_go(int note, int retrig, int glide, float gtime, int acc)
{
    float p = (float)note;
    if (vo.st != E_OFF && vo.st != E_REL && vo.note != note)
        midi(0x80, vo.note, 0);
    if (vo.st == E_OFF || !glide)
        vo.pitch = p;                               /* no glide: straight there */
    vo.target = p;
    vo.gk = glide ? 1.0f - fm_expf(-16.0f * 3.0f / (fm_maxf(gtime, 0.003f) * OM_SR)) : 1.0f;
    vo.note = (uint8_t)note;
    vo.acc = acc ? 1.0f : 0.0f;
    midi(0x90, (uint32_t)note, acc ? 120 : 96);
    if (retrig || vo.st == E_OFF || vo.st == E_REL) {
        vo.punch = 1.0f;
        vo.dive = 1.0f;
        vo.fenv = 1.0f;
        vo.pop = 1.0f;
        vo.mw = 1.0f;
        if (sound == S_808 || sound == S_SUB || sound == S_REESE || sound == S_ACID) {
            if (sound == S_808)
                vo.ph = 0.0f;                       /* the 808 restarts its cycle: the punch */
            vo.st = E_ATT;
        } else {
            vo.st = E_HOLD;
            vo.env = 1.0f;
            tick_coefs();
            pluck(acc ? 1.0f : 0.8f);
        }
    }
    om_note_on = (uint8_t)note;
}

static void voice_off(void)
{
    if (vo.st == E_OFF || vo.st == E_REL)
        return;
    vo.st = E_REL;
    midi(0x80, vo.note, 0);
    om_note_on = 0;
}

/* every 16 samples: the pitch glides, the string's length and decay follow */
static void tick_coefs(void)
{
    float decay = (float)par[P_DECAY] * 0.01f, tone = (float)par[P_TONE] * 0.01f, p, hz, t_hold, t_rel;
    vo.pitch += (vo.target - vo.pitch) * vo.gk;
    p = vo.pitch + bend;
    if (sound == S_808)
        p += 4.0f * vo.punch + (float)par[P_DIVE] * vo.dive;
    else if (par[P_DIVE])
        p -= (float)par[P_DIVE] * 0.5f * vo.dive;   /* a scoop up into the note */
    hz = note_hz(p);
    vo.dt = hz / OM_SR;
    vo.len = fm_clampf(OM_SR / hz, 2.0f, (float)(KS_N - 4));
    switch (sound) {
    case S_MUTED: t_hold = 0.25f + 0.6f * decay; t_rel = 0.03f; break;
    case S_UPRIGHT: t_hold = 1.0f + 6.0f * decay * decay; t_rel = 0.1f; break;
    case S_FRETLESS: t_hold = 2.0f + 12.0f * decay * decay; t_rel = 0.12f; break;
    case S_SLAP: t_hold = 1.2f + 8.0f * decay * decay; t_rel = 0.06f; break;
    default: t_hold = 1.5f + 12.0f * decay * decay; t_rel = 0.06f; break;
    }
    /* one trip round the loop loses what this many samples of decay would */
    vo.k_hold = fm_exp2f(-9.9658f * vo.len / (t_hold * OM_SR));
    vo.k_rel = fm_exp2f(-9.9658f * vo.len / (t_rel * OM_SR));
    switch (sound) {                                /* the loop's brightness */
    case S_MUTED: vo.k_lp = 0.25f + 0.25f * tone; break;
    case S_UPRIGHT: vo.k_lp = 0.35f + 0.3f * tone; break;
    case S_FRETLESS: vo.k_lp = 0.6f + 0.3f * tone; break;
    case S_SLAP: vo.k_lp = 0.75f + 0.22f * tone; break;
    case S_PICK: vo.k_lp = 0.6f + 0.35f * tone; break;
    default: vo.k_lp = 0.4f + 0.4f * tone; break;
    }
}

static inline float ks_read(float delay)
{
    float r = (float)vo.ks_w - delay;
    int i;
    float f;
    while (r < 0.0f)
        r += (float)KS_N;
    i = (int)r;
    f = r - (float)i;
    return ks[i & KS_M] + (ks[(i + 1) & KS_M] - ks[i & KS_M]) * f;
}

static inline float svf_bp(float x, float *l, float *b, float f, float q)
{
    float h = x - *l - q * *b;
    *b = fm_flush(*b + f * h);
    *l = fm_flush(*l + f * *b);
    return *b;
}

static float drive_g = 1.0f, drive_n = 1.0f, lvl = 1.0f, dc_x, dc_y;

#define BLK 256
static float mono[BLK];

static void render_voice(uint32_t n)
{
    uint32_t j;
    float decay = (float)par[P_DECAY] * 0.01f, tone = (float)par[P_TONE] * 0.01f;
    const float rel_osc = t60_coef(sound == S_808 ? 0.09f : 0.06f);
    float t808 = 0.15f + 4.0f * decay * decay, k808 = t60_coef(t808);
    float sus = sound == S_ACID ? 1.0f : 0.25f + 0.75f * decay, k_sus = tc_coef(0.4f);
    for (j = 0; j < n; j++)
        mono[j] = 0.0f;
    if (vo.st == E_OFF) {
        om_env = 0.0f;
        return;
    }
    for (j = 0; j < n; j++) {
        float o = 0.0f;
        if (!(j & 15u)) {
            tick_coefs();
            vo.punch -= vo.punch * k_punch * 16.0f;
            vo.dive -= vo.dive * fm_minf(k_dive * 16.0f, 1.0f);
            vo.fenv -= vo.fenv * fm_minf(k_fenv * 16.0f, 1.0f);
            vo.pop -= vo.pop * fm_minf(k_pop * 16.0f, 1.0f);
            vo.mw -= vo.mw * fm_minf(k_mw * 16.0f, 1.0f);
        }
        if (sound >= S_FINGER && sound <= S_UPRIGHT) {   /* ---- the strings */
            float s = ks_read(vo.len), y;
            vo.lp += vo.k_lp * (s - vo.lp);
            y = vo.lp * (vo.st == E_REL ? vo.k_rel : vo.k_hold);
            ks[vo.ks_w] = fm_flush(y);
            vo.ks_w = (vo.ks_w + 1) & KS_M;
            o = s;
            switch (sound) {
            case S_FINGER: case S_PICK: case S_SLAP: case S_MUTED:   /* the pickup near the bridge */
                o = s - 0.55f * ks_read(vo.len * 0.13f + 1.0f);
                if (sound == S_SLAP) {
                    o += vo.pop * 3.0f * (s - vo.prev); /* the thumb's snap: the string's edge */
                    vo.prev = s;
                }
                o *= 0.9f;
                break;
            case S_FRETLESS: {                      /* a formant that sweeps down: the "mwah" */
                float fc = fm_minf(vo.dt * (2.5f + 7.0f * vo.mw), 0.12f) * FM_PI * 2.0f;
                o = 0.55f * s + 1.1f * svf_bp(s, &vo.svml, &vo.svmb, fc, 0.6f);
                break;
            }
            case S_UPRIGHT: {                       /* the wooden body: two resonances */
                float b1 = svf_bp(s, &vo.sv1l, &vo.sv1b, 0.0142f, 0.35f);   /* ~100 Hz */
                float b2 = svf_bp(s, &vo.sv2l, &vo.sv2b, 0.0313f, 0.3f);    /* ~220 Hz */
                o = 0.7f * s + 0.35f * b1 + 0.3f * b2;
                break;
            }
            default: break;
            }
            if (vo.st == E_REL && fm_fabsf(s) < 1e-4f && fm_fabsf(vo.lp) < 1e-4f) {
                int k;
                vo.st = E_OFF;                      /* rung out: the line is cleared */
                for (k = 0; k < KS_N; k++)
                    ks[k] = 0.0f;
                break;
            }
            o *= sound == S_MUTED ? 3.0f : 2.2f;       /* the strings up to the others' level */
            om_env = fm_fabsf(vo.lp);
        } else {                                    /* ---- the oscillators: their envelope */
            if (vo.st == E_ATT) {
                vo.env += 1.0f / (0.002f * OM_SR);
                if (vo.env >= 1.0f) {
                    vo.env = 1.0f;
                    vo.st = E_HOLD;
                }
            } else if (vo.st == E_HOLD) {
                if (sound == S_808)
                    vo.env *= k808;                 /* the 808 rings down while held */
                else
                    vo.env += (sus - vo.env) * k_sus;
            } else {
                vo.env *= rel_osc;
                if (vo.env < 1e-4f) {
                    vo.st = E_OFF;
                    break;
                }
            }
            switch (sound) {
            case S_808: {
                float x = tsin(vo.ph), g = 1.0f + tone * 3.0f;
                vo.ph = wrap1(vo.ph + vo.dt);
                o = fm_tanhf(x * g) / fm_tanhf(g) * 0.95f;
                break;
            }
            case S_ACID: {
                float saw = 2.0f * vo.ph - 1.0f - blep(vo.ph, vo.dt), fc, g, k = 3.4f, x;
                vo.ph = wrap1(vo.ph + vo.dt);
                if (!(j & 15u)) {                   /* the cutoff: Tone, plus the envelope (accent: more) */
                    fc = 70.0f * fm_exp2f(tone * 6.5f + vo.fenv * (3.0f + 1.5f * vo.acc));
                    vo.ks_w_f = fm_minf(1.0f - fm_expf(-FM_TWO_PI * fc / OM_SR), 0.85f);
                }
                g = vo.ks_w_f;
                x = saw - k * fm_tanhf(vo.f4);
                vo.f1 += g * (x - vo.f1);
                vo.f2 += g * (vo.f1 - vo.f2);
                vo.f3 += g * (vo.f2 - vo.f3);
                vo.f4 = fm_flush(vo.f4 + g * (vo.f3 - vo.f4));
                o = vo.f4 * 1.9f * (1.0f + 0.3f * vo.acc);
                break;
            }
            case S_SUB: {
                float p1 = vo.ph, p2 = p1 + p1, p3 = p2 + p1;
                p2 -= (float)(int)p2;
                p3 -= (float)(int)p3;
                o = tsin(p1) + tone * (0.45f * tsin(p2) + 0.25f * tsin(p3));
                vo.ph = wrap1(p1 + vo.dt);
                o = fm_tanhf(o * 1.2f) * 0.9f;
                break;
            }
            default: {                              /* S_REESE */
                float dc = fm_exp2f((6.0f + 22.0f * tone) * (1.0f / 1200.0f)), d1 = vo.dt * dc, d2 = vo.dt / dc,
                      x, g;
                x = (2.0f * vo.ph2 - 1.0f - blep(vo.ph2, d1)) + (2.0f * vo.ph3 - 1.0f - blep(vo.ph3, d2));
                vo.ph2 = wrap1(vo.ph2 + d1);
                vo.ph3 = wrap1(vo.ph3 + d2);
                x = 0.4f * x + 0.5f * tsin(vo.ph);
                vo.ph = wrap1(vo.ph + vo.dt);
                g = fm_minf(1.0f - fm_expf(-FM_TWO_PI * (180.0f * fm_exp2f(tone * 4.5f)) / OM_SR), 0.9f);
                vo.f1 += g * (x - vo.f1);
                vo.f2 = fm_flush(vo.f2 + g * (vo.f1 - vo.f2));
                o = vo.f2 * 0.9f;
                break;
            }
            }
            o *= vo.env;
            om_env = vo.env;
        }
        mono[j] = o;
    }
}

/* ================================================================ notes === */
#define NSTACK 8
static uint8_t stack[NSTACK], nstack;           /* the keys (and MIDI notes) held, the last on top */
static uint8_t stack_slide;

static void stack_push(int n)
{
    int i, k = 0;
    for (i = 0; i < nstack; i++)
        if (stack[i] != n)
            stack[k++] = stack[i];
    nstack = (uint8_t)k;
    if (nstack == NSTACK) {
        for (i = 1; i < NSTACK; i++)
            stack[i - 1] = stack[i];
        nstack--;
    }
    stack[nstack++] = (uint8_t)n;
}
static int stack_pop(int n)                      /* 1: it was on top */
{
    int i, k = 0, top = nstack && stack[nstack - 1] == n;
    for (i = 0; i < nstack; i++)
        if (stack[i] != n)
            stack[k++] = stack[i];
    nstack = (uint8_t)k;
    return top;
}

static float glide_time(int slide)
{
    float v = (float)par[slide ? P_SLIDE : P_GLIDE] * 0.01f;
    return 0.005f + v * v;
}

/* how a note starts, by the mode: legato is a note played while another is held */
static void play_note(int note, int legato, int slide, int acc)
{
    int mode = par[P_MODE], retrig, glide;
    int sounding = vo.st == E_ATT || vo.st == E_HOLD;
    if (slide) {
        retrig = !sounding;
        glide = 1;
    } else if (mode == MODE_RETRIG) {
        retrig = 1;
        glide = 0;
    } else if (mode == MODE_LEGATO) {
        retrig = !(legato && sounding);
        glide = legato && sounding;
    } else {                                     /* glide all: every note slides from the last */
        retrig = !(legato && sounding);
        glide = vo.st != E_OFF;
    }
    voice_go(note, retrig, glide, glide_time(slide), acc);
}

/* ================================================================= loop === */
static int playing, recording, counting;
static int step, loop_len = 32;
static float step_pos, step_len;                 /* samples into the step, the step's length */
static int seq_left = 0, seq_on = 0;             /* steps left of the loop's note, a loop note sounding */
static float click_env, click_ph, click_dt;
static int count_samples, count_beat;

/* recording */
static int rec_open = -1;                         /* the step where the note being recorded starts */

static int pos_steps(void)                        /* the step nearest now */
{
    int s = step + (step_pos >= 0.5f * step_len ? 1 : 0);
    return s % loop_len;
}

/* mono overdub: a note from s, len steps, replaces what starts under it and cuts the note before */
static void rec_close(int end)
{
    int s = rec_open, len, k;
    uint32_t w;
    if (s < 0)
        return;
    len = ((end - s) % loop_len + loop_len) % loop_len;
    if (len == 0)
        len = 1;
    if (len >= loop_len)
        len = loop_len - 1;
    w = om_pat[s];
    om_pat[s] = (w & ~(0xFFu << 8)) | (uint32_t)len << 8;
    for (k = 1; k < len; k++)
        om_pat[(s + k) % loop_len] = 0;
    rec_open = -1;
    om_rec_writes++;
}

static void rec_start(int note, int glide)
{
    int s = pos_steps(), k;
    if (rec_open >= 0)
        rec_close(s);
    for (k = 1; k < loop_len; k++) {              /* the note before: it ends where this one starts */
        int b = ((s - k) % loop_len + loop_len) % loop_len;
        uint32_t w = om_pat[b];
        if (PAT_NOTE(w)) {
            if ((int)PAT_LEN(w) > k)
                om_pat[b] = (w & ~(0xFFu << 8)) | (uint32_t)k << 8;
            break;
        }
    }
    om_pat[s] = PAT_MAKE(note, 1, glide ? PAT_GLIDE : 0u);
    rec_open = s;
    om_rec_writes++;
}

/* ================================================================== arp === */
static int arp_i, arp_root = -1, arp_last = -1;
static float arp_left, arp_gate;
static uint32_t arp_rng = 0x9E3779B9u;
static int8_t arp_t[4];
static int arp_nt;

static float arp_step_samples(void)
{
    float beat = OM_SR * 60.0f / (float)par[P_TEMPO];
    static const float B[4] = {1.0f, 0.5f, 1.0f / 3.0f, 0.25f};
    return beat * B[par[P_ARPRATE] & 3];
}

static void arp_fire(void)
{
    int n = om_arp_note(par[P_ARPPAT], arp_i++, arp_root, arp_t, arp_nt, par[P_SCALE], par[P_ROOT], &arp_rng);
    int legato = par[P_ARPGATE] >= 100 && arp_last >= 0;
    play_note(n, legato, 0, 0);
    if (recording && playing)
        rec_start(n, legato);
    arp_last = n;
    arp_gate = arp_step_samples() * (float)par[P_ARPGATE] * 0.01f;
}

static void arp_update(void)                      /* the held keys changed */
{
    if (!par[P_ARPPAT] || !nstack) {
        if (om_arp_on) {
            om_arp_on = 0;
            arp_root = -1;
            if (nstack == 0) {
                voice_off();
                if (recording && playing)
                    rec_close(pos_steps());
            }
        }
        return;
    }
    if (arp_root != stack[nstack - 1]) {
        arp_root = stack[nstack - 1];
        arp_nt = om_note_chord(par[P_SCALE], par[P_ROOT], arp_root, 1, arp_t);
        if (!om_arp_on) {
            arp_i = 0;
            arp_left = 0.0f;                      /* the first note now */
            arp_last = -1;
        }
        om_arp_on = 1;
    }
}

/* ======================================================== the loop runs === */
static void seq_step(int s)
{
    uint32_t w = om_pat[s];
    int n = (int)PAT_NOTE(w), was_on = seq_on;
    if (par[P_METRO] == 2 || (par[P_METRO] == 1 && recording)) {
        if ((s & 3) == 0) {
            click_env = 1.0f;
            click_dt = (s % OM_BAR == 0 ? 2000.0f : 1400.0f) / OM_SR;
        }
    }
    if (seq_on && --seq_left <= 0)
        seq_on = 0;                               /* its last step is over */
    if (nstack) {                                 /* the keys play: the loop's notes stand aside */
        seq_on = 0;
        return;
    }
    if (n) {
        play_note(n, (w & PAT_GLIDE) != 0 && was_on, 0, (w & PAT_ACC) != 0);
        seq_on = 1;
        seq_left = (int)PAT_LEN(w);
        if (seq_left < 1)
            seq_left = 1;
    } else if (was_on && !seq_on) {
        voice_off();
    }
}

static void play_start(int rec, int cin)
{
    loop_len = par[P_BARS] * OM_BAR;
    step = 0;
    step_pos = 0.0f;
    seq_on = 0;
    rec_open = -1;
    if (cin) {
        counting = 1;
        count_beat = cin * 4;
        count_samples = 0;
        recording = rec;
        playing = 0;
    } else {
        playing = 1;
        recording = rec;
        seq_step(0);
    }
    om_playing = (uint8_t)playing;
    om_recording = (uint8_t)recording;
}

static void play_stop(void)
{
    if (recording && rec_open >= 0)
        rec_close(pos_steps());
    playing = recording = counting = 0;
    om_playing = om_recording = om_counting = 0;
    if (seq_on && !nstack)
        voice_off();
    seq_on = 0;
}

static void note_cmd(int n, int on, int src, int slide)
{
    (void)src;
    if (on) {
        int legato = nstack > 0;
        stack_push(n);
        stack_slide = (uint8_t)slide;
        if (par[P_ARPPAT]) {
            arp_update();
            return;
        }
        seq_on = 0;
        play_note(n, legato, slide, 0);
        if (recording && playing)
            rec_start(n, legato || slide);
    } else {
        int top = stack_pop(n);
        if (par[P_ARPPAT]) {
            arp_update();
            return;
        }
        if (!top)
            return;
        if (nstack) {                             /* back to the key still held: legato */
            play_note(stack[nstack - 1], 1, 0, 0);
            if (recording && playing)
                rec_start(stack[nstack - 1], 1);
        } else {
            voice_off();
            if (recording && playing)
                rec_close(pos_steps());
        }
    }
}

/* ============================================================ the rest === */
static void apply(int p, int v)
{
    const om_param_t *pi = &PARAMS[p];
    if (v < pi->lo)
        v = pi->lo;
    if (v > pi->hi)
        v = pi->hi;
    par[p] = (int16_t)v;
    switch (p) {
    case P_SOUND:
        if (vo.st != E_OFF) {
            voice_off();
            vo.st = E_OFF;
        }
        sound = v;
        {
            int k;
            for (k = 0; k < KS_N; k++)
                ks[k] = 0.0f;
        }
        vo.f1 = vo.f2 = vo.f3 = vo.f4 = 0.0f;
        voice_coefs();
        break;
    case P_DECAY: voice_coefs(); break;
    case P_DRIVE: {
        float d = (float)v * 0.01f;
        drive_g = 1.0f + 9.0f * d * d;
        drive_n = 1.0f / fm_tanhf(drive_g) * (1.0f - 0.3f * d);
        break;
    }
    case P_LEVEL: lvl = (float)v * (1.0f / 80.0f); break;
    case P_TEMPO:
        step_len = OM_SR * 60.0f / ((float)v * 4.0f);
        break;
    case P_BARS:
        if (!playing && !counting)
            loop_len = v * OM_BAR;
        else
            loop_len = v * OM_BAR;               /* while playing: from the next step */
        if (step >= loop_len)
            step %= loop_len;
        break;
    case P_ARPPAT:
        if (!v && om_arp_on) {
            om_arp_on = 0;
            arp_root = -1;
            if (nstack)
                play_note(stack[nstack - 1], 1, 0, 0);
            else
                voice_off();
        } else if (v) {
            arp_root = -1;
            arp_update();
        }
        break;
    case P_TUNE: tunefac = fm_exp2f((float)v * (1.0f / 1200.0f)); break;
    case P_MIDI:
        if (!v)
            om_midi_out(0xB0, 123, 0);
        break;
    default: break;
    }
}

static void quiet(void)
{
    nstack = 0;
    om_arp_on = 0;
    arp_root = -1;
    voice_off();
    vo.st = E_OFF;
    om_midi_out(0xB0, 123, 0);
}

void om_init(void)
{
    int i;
    for (i = 0; i <= TBL; i++)
        sintab[i] = fm_sinf((float)i * (FM_TWO_PI / (float)TBL));
    for (i = 0; i < KS_N; i++)
        ks[i] = 0.0f;
    q_r = q_w = 0;
    vo.st = E_OFF;
    for (i = 0; i < P_NPARAMS; i++)
        apply(i, PARAMS[i].def);
}

static void drain(void)
{
    while (q_r != q_w) {
        uint32_t c = q[q_r % QN];
        int a = (int)(c >> 16 & 0xFFu), b = (int)(int16_t)(c & 0xFFFFu);
        BARRIER();
        q_r++;
        switch (c >> 24) {
        case C_SET:
            if (a < P_NPARAMS)
                apply(a, b);
            break;
        case C_NOTE: note_cmd(a, b & 1, (b >> 1) & 1, (b >> 2) & 1); break;
        case C_PLAY:
            if (a)
                play_start(b & 1, (b >> 1) & 3);
            else
                play_stop();
            break;
        case C_REC:
            if (!a && recording && rec_open >= 0)
                rec_close(pos_steps());
            recording = a && playing;
            om_recording = (uint8_t)recording;
            break;
        case C_BEND: bend = (float)b * 0.01f; break;
        case C_PANIC: quiet(); break;
        }
    }
}

/* the clocks, every 16 samples: the count-in, the loop's steps (with swing), the arp */
static void clocks(void)
{
    if (counting) {
        float beat = step_len * 4.0f;
        if (count_samples <= 0) {
            if (count_beat == 0) {
                counting = 0;
                om_counting = 0;
                playing = 1;
                om_playing = 1;
                step = 0;
                step_pos = 0.0f;
                seq_step(0);
                return;
            }
            click_env = 1.0f;
            click_dt = ((count_beat & 3) == 0 ? 2000.0f : 1400.0f) / OM_SR;
            count_beat--;
            count_samples += (int)beat;
        }
        count_samples -= 16;
        om_counting = (uint8_t)(count_beat + 1);
    }
    if (playing) {
        float sw = (float)par[P_SWING] * 0.01f * 0.33f;
        float this_len = step_len * ((step & 1) ? 1.0f - sw : 1.0f + sw);
        step_pos += 16.0f;
        if (step_pos >= this_len) {
            step_pos -= this_len;
            step = (step + 1) % loop_len;
            seq_step(step);
        }
        om_step = (uint8_t)step;
        om_step_frac = step_pos / this_len;
    }
    if (om_arp_on) {
        arp_left -= 16.0f;
        if (arp_left <= 0.0f) {
            arp_fire();
            arp_left += arp_step_samples();
        }
        if (par[P_ARPGATE] < 100 && arp_gate > 0.0f) {
            arp_gate -= 16.0f;
            if (arp_gate <= 0.0f) {
                voice_off();
                if (recording && playing)
                    rec_close(pos_steps());
            }
        }
    }
}

void om_render(int32_t *out, uint32_t n, uint32_t gain_q12)
{
    const float gain = (float)gain_q12 * (1.0f / 4096.0f) * 1.3f;
    float pk = om_level * 0.9f;
    uint32_t j0 = 0;
    drain();
    while (j0 < n) {                                /* 16 samples at a time: the clocks, then the sound */
        uint32_t k = n - j0 > 16u ? 16u : n - j0, j;
        clocks();
        render_voice(k);
        for (j = 0; j < k; j++) {
            float x = mono[j], y;
            x = fm_tanhf(x * drive_g) * drive_n * lvl;
            y = x - dc_x + 0.9993f * dc_y;          /* DC out (~5 Hz) */
            dc_x = x;
            dc_y = fm_flush(y);
            if (click_env > 1e-3f) {                /* the metronome, not part of the bass */
                y += 0.25f * click_env * tsin(click_ph);
                click_ph = wrap1(click_ph + click_dt);
                click_env *= 0.9975f;
            }
            y *= gain;
            pk = fm_maxf(pk, fm_fabsf(y));
            out[2 * (j0 + j)] = out[2 * (j0 + j) + 1] = (int32_t)(fm_tanhf(y) * 8300000.0f);
        }
        j0 += k;
    }
    om_level = fm_minf(pk, 1.0f);
}
