/* SPDX-License-Identifier: GPL-3.0-only */
/* BASSLINE UI: the panel and the screen. Part of the unity build (after gfx.c and project.c); the host
 * simulator includes it too.
 *
 *   keys           the bass, chromatic (Snap can pull them into the scale or the bar's chord)
 *   ENV            held with a key: slide into that note
 *   PLAY           the loop on / off (from step entry: record live, after the count-in)
 *   REC            playing: recording on / off; stopped: step entry on / off
 *   LFO            undo (and redo) the last recording, step entry, fill or clear; held 1.5 s: clear
 *                  step entry: LFO a rest, ENV a tie, ENV held with a key a slide
 *   ARP            the bass arpeggiator on / off
 *   OCT- / OCT+    the octave          SELECT scale (step entry: the cursor)  ALGORITHM root  PRESETS sound
 *   HOME FX SEQ SEL GLO   pages SOUND, GLIDE, LOOP, PROG, SETUP; EDIT steps through all seven
 *   KNOB 1-4       the page's four values
 *   SAVE           saves (it also saves by itself when the loop is stopped and nothing is touched) */

enum { V_SOUND, V_GLIDE, V_LOOP, V_PROG, V_ARP, V_SUGGEST, V_SETUP, NVIEWS };
static const char *const VIEW_NAME[NVIEWS] = {"Sound", "Glide", "Loop", "Prog", "Arp", "Suggest", "Setup"};
enum { K_BAR = 100, K_CHORD, K_SEVENTH, K_FILL, K_SBAR, K_SPICK, K_APPLY, K_LEDS, K_NONE };
static const uint8_t VIEW_KNOB[NVIEWS][4] = {
    {P_TONE, P_DECAY, P_DRIVE, P_LEVEL},
    {P_GLIDE, P_MODE, P_SLIDE, P_DIVE},
    {P_BARS, P_TEMPO, P_SWING, P_METRO},
    {K_BAR, K_CHORD, K_SEVENTH, P_SNAP},
    {P_ARPPAT, P_ARPRATE, P_ARPGATE, K_FILL},
    {K_SBAR, K_SPICK, K_APPLY, K_NONE},
    {P_STEPLEN, P_COUNTIN, K_LEDS, P_MIDI},
};

#define NKEYS_ 27
#define KEY0 53                               /* F3: the first key, at Octave 0 */

/* colours: a dark ground, amber for the bass, a red for recording */
#define K_BG RGB(12, 12, 16)
#define K_PANEL RGB(26, 24, 30)
#define K_HDR RGB(18, 17, 22)
#define K_LINE RGB(56, 52, 62)
#define K_GRID RGB(34, 32, 40)
#define K_DIM RGB(130, 124, 140)
#define K_LABEL RGB(178, 172, 186)
#define K_TEXT RGB(244, 240, 236)
#define K_ACC RGB(255, 168, 64)
#define K_ACC_T RGB(70, 46, 20)
#define K_REC RGB(255, 84, 84)
#define C_CHD RGB(112, 196, 255)
#define C_CHD_T RGB(24, 44, 64)

static struct {
    uint8_t view;
    uint32_t btn, keys, btn_used;
    uint32_t enc_t[NE];
    uint8_t keynote[NKEYS_];                  /* the note each key started (after Snap) */
    uint8_t step_mode;
    int16_t cursor;                           /* step entry: where the next note goes */
    uint32_t undo[OM_STEPS];
    uint8_t undo_ok;
    uint8_t prog_bar, sug_bar, sug_pick;
    uint32_t lfo_t;                           /* LFO went down (held 1.5 s: clear) */
    uint8_t lfo_cleared;
    int8_t last_note;
    uint32_t rec_seen;
    uint8_t dirty;
    uint32_t act_t;
    char msg[2][24];
    uint32_t msg_until;
    int8_t touched;
    uint32_t touch_until;
    uint32_t frame;
    uint32_t sig[3];
    /* chord suggestions, per bar: 3 picks (degree, 7th) and their scores */
    uint8_t sug_deg[OM_MAXBARS][3], sug_7th[OM_MAXBARS][3], sug_n[OM_MAXBARS];
    uint32_t sug_sig;
} ui;

static void say(const char *a, const char *b)
{
    uint32_t i;
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1300u;
}
void ui_say(const char *a, const char *b) { say(a, b); }
int ui_dirty(void) { return ui.dirty != 0; }
static void mark_dirty(void) { ui.dirty = 1; }

static void scpy(char *b, const char *s)
{
    while ((*b++ = *s++))
        ;
}
static void scat_(char *b, const char *s)
{
    while (*b)
        b++;
    scpy(b, s);
}
static void itoa_u(uint32_t v, char *b)
{
    char t[12];
    int n = 0;
    do {
        t[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n)
        *b++ = t[--n];
    *b = 0;
}
static void note_name(int n, char *b)        /* "E1" */
{
    char o[4];
    scpy(b, OM_ROOT_NAME[n % 12]);
    itoa_u((uint32_t)(n / 12 - 1 < 0 ? 0 : n / 12 - 1), o);
    scat_(b, o);
}

/* --------------------------------------------------------- the music --- */
static int nbars(void) { return proj.par[P_BARS]; }
static int loop_steps(void) { return nbars() * OM_BAR; }
static int step_len(void) { return 1 << proj.par[P_STEPLEN]; }
static int key_note(int k) { return KEY0 + k + 12 * proj.par[P_OCTAVE]; }

/* the bar the music is at: playing, its step; step entry, the cursor's; else the Prog page's bar */
static int cur_bar(void)
{
    if (om_playing)
        return om_step / OM_BAR;
    if (ui.step_mode)
        return ui.cursor / OM_BAR;
    return ui.prog_bar < nbars() ? ui.prog_bar : 0;
}

/* a bar's chord: its notes (n of them), or 0 when the bar has none */
static int bar_chord(int b, uint8_t *notes)
{
    int v = proj.prog[b & 7] & 0x7F, n = proj.prog[b & 7] & 0x80 ? 4 : 3;
    if (!v)
        return 0;
    return om_chord_notes(proj.par[P_SCALE], proj.par[P_ROOT], 2, v - 1, n, notes);
}
/* the chord's name: "ii Dm" or just "ii" */
static void chord_label(int deg, int seventh, int with_name, char *b)
{
    uint8_t mn[4];
    char q[16];
    int n = om_chord_notes(proj.par[P_SCALE], proj.par[P_ROOT], 3, deg, seventh ? 4 : 3, mn);
    om_chord_quality(mn, n, q);
    om_roman(deg, (q[0] == 'm' && q[1] != 'a') || (q[0] == 'd' && q[1] == 'i'), b);
    if (with_name) {
        scat_(b, " ");
        scat_(b, OM_ROOT_NAME[mn[0] % 12]);
        scat_(b, q);
    }
}

/* Snap: the nearest note in the scale, or in the bar's chord (the scale when the bar has none) */
static int snap(int n)
{
    int mode = proj.par[P_SNAP], d, pcs = 0;
    uint8_t c[4];
    if (mode == SNAP_OFF)
        return n;
    if (mode == SNAP_CHORD) {
        int k, m = bar_chord(cur_bar(), c);
        for (k = 0; k < m; k++)
            pcs |= 1 << (c[k] % 12);
    }
    for (d = 0; d < 7; d++) {
        int cand[2] = {n - d, n + d}, i;
        for (i = 0; i < 2; i++) {
            int x = cand[i];
            if (x < 0 || x > 127)
                continue;
            if (pcs ? (pcs >> (x % 12) & 1) : om_in_scale(proj.par[P_SCALE], proj.par[P_ROOT], x))
                return x;
        }
    }
    return n;
}

/* ------------------------------------------------------------- the loop --- */
static void snapshot(void)
{
    int i;
    for (i = 0; i < OM_STEPS; i++)
        ui.undo[i] = om_pat[i];
    ui.undo_ok = 1;
}
static void undo_swap(void)                  /* undo, and pressed again, redo */
{
    int i;
    if (!ui.undo_ok) {
        say("NOTHING TO UNDO", 0);
        return;
    }
    for (i = 0; i < OM_STEPS; i++) {
        uint32_t t = om_pat[i];
        om_pat[i] = ui.undo[i];
        ui.undo[i] = t;
    }
    say("UNDO", "(LFO again: redo)");
    mark_dirty();
}

/* a note at s, len steps (mono): clears what starts under it, cuts the note before */
static void pat_write(int s, int len, uint32_t w)
{
    int L = loop_steps(), k;
    s %= L;
    for (k = 1; k < L; k++) {
        int b = ((s - k) % L + L) % L;
        uint32_t x = om_pat[b];
        if (PAT_NOTE(x)) {
            if ((int)PAT_LEN(x) > k)
                om_pat[b] = (x & ~(0xFFu << 8)) | (uint32_t)k << 8;
            break;
        }
    }
    for (k = 1; k < len && k < L; k++)
        om_pat[(s + k) % L] = 0;
    om_pat[s] = w;
}

static void step_advance(int n)
{
    ui.cursor = (int16_t)((ui.cursor + n) % loop_steps());
}

static void step_note(int note, int glide)
{
    int len = step_len();
    pat_write(ui.cursor, len, PAT_MAKE(note, len, glide ? PAT_GLIDE : 0u));
    step_advance(len);
    mark_dirty();
}
static void step_rest(void)
{
    int len = step_len(), L = loop_steps(), k;
    pat_write(ui.cursor, len, 0);
    for (k = 0; k < len; k++)
        om_pat[(ui.cursor + k) % L] = 0;
    step_advance(len);
    mark_dirty();
}
static void step_tie(void)                   /* the note before the cursor lasts a step length longer */
{
    int L = loop_steps(), k, len = step_len();
    for (k = 1; k < L; k++) {
        int b = ((ui.cursor - k) % L + L) % L;
        uint32_t x = om_pat[b];
        if (PAT_NOTE(x)) {
            int nl = (int)PAT_LEN(x) + len;
            if (nl > L - 1)
                nl = L - 1;
            pat_write(b, nl, (x & ~(0xFFu << 8)) | (uint32_t)nl << 8);
            break;
        }
    }
    step_advance(len);
    mark_dirty();
}

/* FILL: the loop written from the progression by the arp's pattern and rate. Bars without a chord
 * keep the chord before (the key's I at the start). The last note of a bar walks into the next
 * bar's root when the chord changes (a half step under it). */
static void fill_loop(void)
{
    int bars = nbars(), b, rate = om_arp_rate_steps(proj.par[P_ARPRATE]), pat = proj.par[P_ARPPAT];
    int deg = 0, sev = 0, gate = proj.par[P_ARPGATE], lo = 28 + 12 * (proj.par[P_OCTAVE] + 2);
    uint32_t rng = 0x1234567u;
    if (!rate)
        rate = 2;
    if (!pat)
        pat = proj.arp_last ? proj.arp_last : 1;
    snapshot();
    for (b = 0; b < OM_STEPS; b++)
        om_pat[b] = 0;
    for (b = 0; b < bars; b++) {
        int v = proj.prog[b] & 0x7F, i, nb = (b + 1) % bars, nv, root, next_root;
        int8_t t[4];
        uint8_t c[4];
        int nt;
        if (v) {
            deg = v - 1;
            sev = proj.prog[b] >> 7;
        }
        om_chord_notes(proj.par[P_SCALE], proj.par[P_ROOT], 0, deg, sev ? 4 : 3, c);
        root = lo + ((c[0] - lo) % 12 + 12) % 12;
        for (i = 0; i < (sev ? 4 : 3); i++)
            t[i] = (int8_t)(c[i] - c[0]);
        nt = sev ? 4 : 3;
        nv = proj.prog[nb] & 0x7F;
        next_root = -1;
        if (nv && nv - 1 != deg) {
            uint8_t c2[4];
            om_chord_notes(proj.par[P_SCALE], proj.par[P_ROOT], 0, nv - 1, 3, c2);
            next_root = lo + ((c2[0] - lo) % 12 + 12) % 12;
        }
        for (i = 0; i < OM_BAR / rate; i++) {
            int s = b * OM_BAR + i * rate, len = rate * gate / 100, n;
            n = om_arp_note(pat, i, root, t, nt, proj.par[P_SCALE], proj.par[P_ROOT], &rng);
            if (next_root >= 0 && i == OM_BAR / rate - 1 && pat == 6)
                n = next_root - 1;              /* Walk: a chromatic step into the next chord */
            if (len < 1)
                len = 1;
            om_pat[s] = PAT_MAKE(n, len, 0u);
        }
    }
    mark_dirty();
}

/* SUGGEST: for each bar, the chords of the key that fit its notes best. A note counts by its length,
 * twice on the bar's first step and half again on a beat; a chord tone adds, any other note takes
 * away a little; the bar's first note as the chord's root counts most. A 7th chord must earn it. */
static void suggest(void)
{
    int b, bars = nbars(), len = om_scale_len(proj.par[P_SCALE]);
    uint32_t sig = 2166136261u;
    for (b = 0; b < OM_STEPS; b++)
        sig = (sig ^ om_pat[b]) * 16777619u;
    sig = (sig ^ (uint32_t)(proj.par[P_SCALE] | proj.par[P_ROOT] << 4 | bars << 8)) * 16777619u;
    if (sig == ui.sug_sig)
        return;
    ui.sug_sig = sig;
    for (b = 0; b < bars; b++) {
        float w[12] = {0}, best[3] = {-1e9f, -1e9f, -1e9f}, total = 0.0f;
        int first = -1, s, d, sv, k;
        for (s = 0; s < OM_BAR; s++) {
            uint32_t x = om_pat[b * OM_BAR + s];
            if (PAT_NOTE(x)) {
                float wt = (float)PAT_LEN(x) * (s == 0 ? 2.0f : (s & 3) == 0 ? 1.5f : 1.0f);
                w[PAT_NOTE(x) % 12] += wt;
                total += wt;
                if (first < 0)
                    first = (int)(PAT_NOTE(x) % 12);
            }
        }
        if (first < 0) {                        /* a note held over from the bar before */
            for (k = 1; k <= b * OM_BAR; k++) {
                uint32_t x = om_pat[b * OM_BAR - k];
                if (PAT_NOTE(x)) {
                    if ((int)PAT_LEN(x) > k) {
                        first = (int)(PAT_NOTE(x) % 12);
                        w[first] += 2.0f;
                        total += 2.0f;
                    }
                    break;
                }
            }
        }
        ui.sug_n[b] = 0;
        if (total <= 0.0f)
            continue;
        for (d = 0; d < len; d++)
            for (sv = 0; sv < 2; sv++) {
                uint8_t c[4];
                int n = om_chord_notes(proj.par[P_SCALE], proj.par[P_ROOT], 3, d, sv ? 4 : 3, c), pcs = 0, pc;
                float sc = 0.0f;
                for (k = 0; k < n; k++)
                    pcs |= 1 << (c[k] % 12);
                for (pc = 0; pc < 12; pc++)
                    if (w[pc] > 0.0f)
                        sc += (pcs >> pc & 1) ? w[pc] : -0.6f * w[pc];
                if (first == c[0] % 12)
                    sc += 3.0f + 0.25f * total;
                if (sv)
                    sc -= w[c[3] % 12] > 0.0f ? 0.5f : 2.0f;
                for (k = 0; k < 3; k++)          /* keep the best three, best first */
                    if (sc > best[k]) {
                        int m;
                        for (m = 2; m > k; m--) {
                            best[m] = best[m - 1];
                            ui.sug_deg[b][m] = ui.sug_deg[b][m - 1];
                            ui.sug_7th[b][m] = ui.sug_7th[b][m - 1];
                        }
                        best[k] = sc;
                        ui.sug_deg[b][k] = (uint8_t)d;
                        ui.sug_7th[b][k] = (uint8_t)sv;
                        break;
                    }
            }
        for (k = 0; k < 3; k++)
            if (best[k] > -1e8f)
                ui.sug_n[b] = (uint8_t)(k + 1);
    }
}

/* ------------------------------------------------------------ values --- */
static int knob_get(int k)
{
    if (k < P_NPARAMS)
        return proj.par[k];
    switch (k) {
    case K_BAR: return ui.prog_bar;
    case K_CHORD: return proj.prog[ui.prog_bar] & 0x7F;
    case K_SEVENTH: return proj.prog[ui.prog_bar] >> 7;
    case K_SBAR: return ui.sug_bar;
    case K_SPICK: return ui.sug_pick;
    case K_LEDS: return proj.leds;
    default: return 0;
    }
}
static void knob_range(int k, int *lo, int *hi)
{
    if (k < P_NPARAMS) {
        *lo = om_param_info(k)->lo;
        *hi = om_param_info(k)->hi;
        return;
    }
    *lo = 0;
    switch (k) {
    case K_BAR: case K_SBAR: *hi = nbars() - 1; break;
    case K_CHORD: *hi = om_scale_len(proj.par[P_SCALE]); break;
    case K_SEVENTH: *hi = 1; break;
    case K_SPICK: *hi = 2; break;
    case K_LEDS: *hi = 2; break;
    default: *hi = 0; break;
    }
}
static const char *knob_name(int k)
{
    if (k < P_NPARAMS)
        return om_param_info(k)->name;
    switch (k) {
    case K_BAR: case K_SBAR: return "Bar";
    case K_CHORD: return "Chord";
    case K_SEVENTH: return "7th";
    case K_FILL: return "Fill loop";
    case K_SPICK: return "Pick";
    case K_APPLY: return "Use";
    case K_LEDS: return "Lights";
    default: return "";
    }
}
static void knob_text(int k, char *b)
{
    int v = knob_get(k);
    b[0] = 0;
    if (k < P_NPARAMS) {
        om_param_text(k, v, b);
        return;
    }
    switch (k) {
    case K_BAR: case K_SBAR: itoa_u((uint32_t)v + 1u, b); break;
    case K_CHORD:
        if (!v)
            scpy(b, "-");
        else
            chord_label(v - 1, proj.prog[ui.prog_bar] >> 7, 0, b);
        break;
    case K_SEVENTH: scpy(b, v ? "On" : "Off"); break;
    case K_FILL: scpy(b, "Turn"); break;
    case K_SPICK: itoa_u((uint32_t)v + 1u, b); break;
    case K_APPLY: scpy(b, "+bar -all"); break;
    case K_LEDS: scpy(b, v == 2 ? "Scale" : v ? "Chord" : "Off"); break;
    default: break;
    }
}

static void use_suggestion(int b, int pick)
{
    if (b >= nbars() || pick >= ui.sug_n[b])
        return;
    proj.prog[b] = (uint8_t)((ui.sug_deg[b][pick] + 1) | ui.sug_7th[b][pick] << 7);
}

static void knob_set(int k, int v)
{
    int lo, hi;
    knob_range(k, &lo, &hi);
    v = v < lo ? lo : v > hi ? hi : v;
    if (k == K_FILL || k == K_APPLY)
        return;
    if (v == knob_get(k))
        return;
    if (k < P_NPARAMS) {
        proj.par[k] = (int16_t)v;
        om_set(k, v);
        if (k == P_ARPPAT && v)
            proj.arp_last = (uint8_t)v;
        if (k == P_BARS) {
            if (ui.prog_bar >= v)
                ui.prog_bar = (uint8_t)(v - 1);
            if (ui.sug_bar >= v)
                ui.sug_bar = (uint8_t)(v - 1);
            if (ui.cursor >= v * OM_BAR)
                ui.cursor = 0;
        }
    } else {
        switch (k) {
        case K_BAR: ui.prog_bar = (uint8_t)v; return;
        case K_SBAR: ui.sug_bar = (uint8_t)v; ui.sug_pick = 0; return;
        case K_SPICK: ui.sug_pick = (uint8_t)v; return;
        case K_CHORD: proj.prog[ui.prog_bar] = (uint8_t)((proj.prog[ui.prog_bar] & 0x80) | v); break;
        case K_SEVENTH: proj.prog[ui.prog_bar] = (uint8_t)((proj.prog[ui.prog_bar] & 0x7F) | v << 7); break;
        case K_LEDS: proj.leds = (uint8_t)v; break;
        default: break;
        }
    }
    mark_dirty();
}

static void knob_wrap(int k, int32_t e)
{
    int lo, hi, v;
    knob_range(k, &lo, &hi);
    v = knob_get(k) + (e > 0 ? 1 : -1);
    if (v > hi)
        v = lo;
    if (v < lo)
        v = hi;
    knob_set(k, v);
}

static uint8_t accel_off;
static int32_t accel(int role, int32_t s, int range)
{
    uint32_t now = plat_ms(), dt = now - ui.enc_t[role], a = (uint32_t)(s < 0 ? -s : s), m;
    ui.enc_t[role] = now;
    if (range <= 24 || !a || accel_off)
        return s;
    if (a > 1)
        dt /= a;
    m = dt < 15u ? 6u : dt < 30u ? 4u : dt < 60u ? 2u : 1u;
    return s * (int32_t)m;
}

static void turn(int role, int k, int32_t e)
{
    int lo, hi;
    if (!e || k == K_NONE)
        return;
    if (k == K_FILL) {                       /* action knobs: a turn does it */
        if (e > 0) {
            fill_loop();
            say("LOOP FILLED", "(LFO: undo)");
        }
        return;
    }
    if (k == K_APPLY) {
        int b;
        suggest();
        if (e > 0) {
            use_suggestion(ui.sug_bar, ui.sug_pick);
            say("PROGRESSION", "BAR SET");
        } else {
            for (b = 0; b < nbars(); b++)
                use_suggestion(b, 0);
            say("PROGRESSION", "ALL BARS SET");
        }
        mark_dirty();
        return;
    }
    knob_range(k, &lo, &hi);
    knob_set(k, knob_get(k) + accel(role, e, hi - lo));
}

/* ------------------------------------------------------------ buttons --- */
static void set_view(int v)
{
    ui.view = (uint8_t)v;
    ui.touched = -1;
}

static void step_mode_set(int on)
{
    if (on && !ui.step_mode) {
        snapshot();
        ui.cursor = 0;
    }
    ui.step_mode = (uint8_t)on;
    say("STEP ENTRY", on ? "ON" : "OFF");
}

static void button(int b)
{
    char t[16];
    switch (b) {
    case B_HOME: set_view(V_SOUND); break;
    case B_FX: set_view(V_GLIDE); break;
    case B_SEQ: set_view(V_LOOP); break;
    case B_SEL: set_view(V_PROG); break;
    case B_GLO: set_view(V_SETUP); break;
    case B_EDIT: set_view((ui.view + 1) % NVIEWS); break;
    case B_PLAY:
        if (om_playing || om_counting) {
            om_play(0, 0, 0);
            say("STOP", 0);
        } else if (ui.step_mode) {           /* armed: record live, after the count-in */
            ui.step_mode = 0;
            snapshot();
            om_play(1, 1, proj.par[P_COUNTIN]);
            say("RECORDING", proj.par[P_COUNTIN] ? "AFTER THE COUNT-IN" : 0);
        } else {
            om_play(1, 0, 0);
        }
        break;
    case B_REC:
        if (om_playing || om_counting) {
            if (om_recording) {
                om_rec(0);
                say("RECORDING", "OFF");
            } else {
                snapshot();
                om_rec(1);
                say("RECORDING", "ON");
            }
        } else {
            step_mode_set(!ui.step_mode);
        }
        break;
    case B_ENV:                              /* tapped alone, in step entry: a tie */
        if (ui.step_mode)
            step_tie();
        break;
    case B_LFO:
        if (ui.lfo_cleared)
            break;
        if (ui.step_mode)
            step_rest();
        else if (!om_recording)
            undo_swap();
        break;
    case B_ARP:
        if (proj.par[P_ARPPAT]) {
            knob_set(P_ARPPAT, 0);
            say("ARP", "OFF");
        } else {
            knob_set(P_ARPPAT, proj.arp_last ? proj.arp_last : 1);
            om_param_text(P_ARPPAT, proj.par[P_ARPPAT], t);
            say("ARP", t);
        }
        break;
    case B_OCTDN: case B_OCTUP:
        knob_set(P_OCTAVE, proj.par[P_OCTAVE] + (b == B_OCTUP ? 1 : -1));
        note_name(key_note(0), t);
        say("LOWEST KEY", t);
        break;
    case B_SAVE:
        if (project_save() == 0) {
            ui.dirty = 0;
            say("SAVED", 0);
        } else {
            say("NOT SAVED", "FLASH ERROR");
        }
        break;
    default: break;
    }
}

static void key_event(int k, int down)
{
    if (down) {
        int n = snap(key_note(k)), slide = (int)(ui.btn >> B_ENV & 1u), others = (int)(ui.keys & ~(1u << k)) != 0;
        if (slide)
            ui.btn_used |= 1u << B_ENV;
        ui.keynote[k] = (uint8_t)n;
        om_note(n, 1, SRC_KEYS, slide);
        ui.last_note = (int8_t)n;
        if (ui.step_mode)
            step_note(n, slide || others);
    } else {
        om_note(ui.keynote[k], 0, SRC_KEYS, 0);
    }
}

/* MIDI in (USB and the TRS jack): notes on any channel play the bass (Snap applies), pitch bend
 * bends it (2 semitones) */
static void midi_in(void)
{
    uint32_t pkt;
    static uint8_t held_as[128];
    while (plat_midi_in(&pkt)) {
        uint32_t st = (pkt >> 8) & 0xF0u, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
        if (st == 0x90u && d2) {
            int n = snap((int)d1);
            held_as[d1] = (uint8_t)n;
            om_note(n, 1, SRC_MIDI, 0);
            ui.last_note = (int8_t)n;
        } else if (st == 0x80u || st == 0x90u) {
            om_note(held_as[d1] ? held_as[d1] : (int)d1, 0, SRC_MIDI, 0);
        } else if (st == 0xE0u) {
            int v = (int)(d1 | d2 << 7) - 8192;
            om_bend(v * 200 / 8192);
        } else if (st == 0xB0u && (d1 == 120u || d1 == 123u)) {
            om_panic();
        }
    }
}

static void input(void)
{
    uint32_t btn = plat_buttons(), keys = plat_keys(), ch, i, now = plat_ms();
    int32_t e;
    ch = btn ^ ui.btn;
    ui.btn = btn;
    for (i = 0; i < NB; i++) {
        uint32_t m = 1u << i;
        if (!(ch & m))
            continue;
        if (btn & m) {
            ui.btn_used &= ~m;
            if (i == B_PLAY)
                button((int)i);
            if (i == B_LFO) {
                ui.lfo_t = now;
                ui.lfo_cleared = 0;
            }
        } else if (!(ui.btn_used & m) && i != B_PLAY) {
            button((int)i);
        }
    }
    if ((btn >> B_LFO & 1u) && !ui.lfo_cleared && now - ui.lfo_t > 1500u && !om_recording) {
        int k;                               /* LFO held: the loop cleared */
        snapshot();
        for (k = 0; k < OM_STEPS; k++)
            om_pat[k] = 0;
        ui.lfo_cleared = 1;
        ui.btn_used |= 1u << B_LFO;
        say("LOOP CLEARED", "(LFO: undo)");
        mark_dirty();
    }
    ch = keys ^ ui.keys;
    ui.keys = keys;
    for (i = 0; i < NKEYS_; i++)
        if (ch >> i & 1u)
            key_event((int)i, (int)(keys >> i & 1u));
    if (btn || keys || ch)
        ui.act_t = now;
    midi_in();
    if ((e = plat_enc(EN_SELECT)) != 0) {
        if (ui.step_mode) {
            ui.cursor = (int16_t)(((ui.cursor + (e > 0 ? 1 : -1)) % loop_steps() + loop_steps()) % loop_steps());
        } else {
            knob_wrap(P_SCALE, e);
        }
    }
    if ((e = plat_enc(EN_ALGO)) != 0)
        knob_wrap(P_ROOT, e);
    if ((e = plat_enc(EN_PRESET)) != 0)
        knob_wrap(P_SOUND, e);
    for (i = 0; i < 4; i++)
        if ((e = plat_enc(EN_K1 + (int)i)) != 0) {
            turn(EN_K1 + (int)i, VIEW_KNOB[ui.view][i], e);
            ui.touched = (int8_t)i;
            ui.touch_until = now + 1200u;
            ui.act_t = now;
        }
    if (om_rec_writes != ui.rec_seen) {
        ui.rec_seen = om_rec_writes;
        mark_dirty();
    }
}

/* ------------------------------------------------------------- screen --- */
#define HDR_H 28
#define MAIN_Y 28
#define MAIN_H 144
#define KNB_Y 172
#define KNB_H 68

static uint32_t hash(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

static void cv_round(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c)
{
    static const uint8_t IN[7][6] = {{0}, {1}, {1, 0}, {2, 1, 0}, {2, 1, 0, 0}, {3, 2, 1, 0, 0}, {4, 2, 1, 1, 0, 0}};
    int32_t j;
    if (r > 6)
        r = 6;
    if (r > h / 2)
        r = h / 2;
    if (r > w / 2)
        r = w / 2;
    cv_rect(x, y + r, w, h - 2 * r, c);
    for (j = 0; j < r; j++) {
        int32_t d = IN[r][j];
        cv_rect(x + d, y + j, w - 2 * d, 1, c);
        cv_rect(x + d, y + h - 1 - j, w - 2 * d, 1, c);
    }
}
static void text_c(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}

static void draw_header(void)
{
    char t[24];
    int msg = plat_ms() < ui.msg_until;
    uint32_t h = hash(hash(hash(2166136261u, ui.dirty | (uint32_t)msg << 1),
                           (uint32_t)proj.par[P_SCALE] | (uint32_t)proj.par[P_ROOT] << 8 | (uint32_t)proj.par[P_SOUND] << 16),
                      (uint32_t)proj.par[P_TEMPO]);
    if (msg)
        h = hash(hash(h, (uint32_t)ui.msg[0][0] << 8 | ui.msg[0][1]), ui.msg_until);
    if (h == ui.sig[0])
        return;
    ui.sig[0] = h;
    cv_begin(240, HDR_H, K_HDR);
    cv_rect(0, HDR_H - 1, 240, 1, K_LINE);
    if (msg) {
        int32_t x = cv_text(10, 5, &FONT_B, ui.msg[0], K_TEXT);
        cv_text(x + 8, 5, &FONT_B, ui.msg[1], K_ACC);
    } else {
        int32_t x;
        scpy(t, OM_ROOT_NAME[proj.par[P_ROOT]]);
        scat_(t, " ");
        scat_(t, OM_SCALE_NAME[proj.par[P_SCALE]]);
        cv_text(10, 5, &FONT_B, t, K_ACC);
        itoa_u((uint32_t)proj.par[P_TEMPO], t);
        x = 232 - text_w(&FONT_S, t);
        cv_text(x, 5, &FONT_S, t, K_DIM);
        x -= 8 + text_w(&FONT_S, OM_SOUND_NAME[proj.par[P_SOUND]]);
        cv_text(x, 5, &FONT_S, OM_SOUND_NAME[proj.par[P_SOUND]], K_LABEL);
        if (ui.dirty)
            cv_round(x - 10, 11, 5, 5, 2, K_DIM);
    }
    cv_blit(0, 0);
}

/* the loop as a piano roll: steps across, pitch up; the bars' lines, the playhead or the cursor */
#define PR_X 8
#define PR_W 224
static void draw_roll(int32_t y0, int32_t h)
{
    int L = loop_steps(), s, lo = 127, hi = 0, range, prev_y = -1, prev_end = -1;
    for (s = 0; s < L; s++)
        if (PAT_NOTE(om_pat[s])) {
            int n = (int)PAT_NOTE(om_pat[s]);
            lo = n < lo ? n : lo;
            hi = n > hi ? n : hi;
        }
    if (lo > hi) {                           /* empty: the keys' range */
        lo = key_note(0);
        hi = lo + 12;
    }
    if (hi - lo < 12) {
        int c = (hi + lo) / 2;
        lo = c - 6;
        hi = c + 6;
    }
    range = hi - lo + 1;
    cv_rect(PR_X, y0, PR_W, h, K_PANEL);
    for (s = 0; s <= L; s += 4) {            /* beats, and bars stronger */
        int32_t x = PR_X + PR_W * s / L;
        if (s == L)
            x = PR_X + PR_W - 1;
        cv_rect(x, y0, 1, h, s % OM_BAR == 0 ? K_LINE : K_GRID);
    }
    for (s = 0; s < L; s++) {
        uint32_t w = om_pat[s];
        int n = (int)PAT_NOTE(w), len, cur;
        int32_t x, xe, y, nh;
        if (!n)
            continue;
        len = (int)PAT_LEN(w);
        x = PR_X + PR_W * s / L;
        xe = PR_X + PR_W * (s + len > L ? L : s + len) / L;
        nh = h / range;
        nh = nh < 2 ? 2 : nh > 5 ? 5 : nh;
        y = y0 + 2 + (h - 4 - nh) * (hi - n) / (hi - lo > 0 ? hi - lo : 1);
        cur = om_playing && om_step >= s && om_step < s + len;
        if ((w & PAT_GLIDE) && prev_y >= 0 && prev_end >= x - 1)   /* the slide: from the note before */
            cv_line(x - 2, prev_y + nh / 2, x + 2, y + nh / 2, K_ACC);
        cv_rect(x, y, xe - x > 1 ? xe - x - 1 : 1, nh, cur ? K_TEXT : (w & PAT_ACC) ? K_REC : K_ACC);
        prev_y = y;
        prev_end = xe;
    }
    if (om_playing) {
        int32_t x = PR_X + (int32_t)((float)PR_W * ((float)om_step + om_step_frac) / (float)L);
        cv_rect(x, y0, 1, h, om_recording ? K_REC : K_TEXT);
    } else if (ui.step_mode) {
        int32_t x = PR_X + PR_W * ui.cursor / L, w = PR_W * step_len() / L;
        cv_rect(x, y0, w > 2 ? w : 2, 2, K_REC);
        cv_rect(x, y0, 1, h, K_REC);
    }
}

/* the progression under the roll: a chip per bar */
static void draw_lane(int32_t y0, int sugg)
{
    int bars = nbars(), b, cb = cur_bar();
    int32_t w = PR_W / bars;
    for (b = 0; b < bars; b++) {
        char t[20];
        int32_t x = PR_X + b * w;
        int v = proj.prog[b] & 0x7F, sel, deg = -1, sev = 0;
        uint16_t bg, fg;
        t[0] = 0;
        if (sugg) {
            int pick = b == ui.sug_bar ? ui.sug_pick : 0;
            if (pick < ui.sug_n[b]) {
                deg = ui.sug_deg[b][pick];
                sev = ui.sug_7th[b][pick];
            }
            sel = ui.view == V_SUGGEST && b == ui.sug_bar;
        } else {
            if (v) {
                deg = v - 1;
                sev = proj.prog[b] >> 7;
            }
            sel = ui.view == V_PROG && b == ui.prog_bar;
        }
        if (deg >= 0)
            chord_label(deg, sev, w >= 54, t);
        else
            scpy(t, "-");
        bg = b == cb && (om_playing || ui.step_mode) ? C_CHD : C_CHD_T;
        fg = b == cb && (om_playing || ui.step_mode) ? K_BG : C_CHD;
        if (sel)
            cv_round(x, y0, w - 2, 30, 5, K_TEXT);
        cv_round(x + 1, y0 + 1, w - 4, 28, 4, bg);
        if (text_w(&FONT_XS, t) > w - 6) {      /* too long: the numeral only */
            int i = 0;
            while (t[i] && t[i] != ' ')
                i++;
            t[i] = 0;
        }
        text_c(x + w / 2 - 1, y0 + 8, &FONT_XS, t, fg);
    }
}

static void draw_main(void)
{
    char t[32], q[24];
    int s, bars = nbars();
    uint32_t h;
    if (ui.view == V_SUGGEST)
        suggest();
    h = hash(2166136261u, (uint32_t)ui.view | (uint32_t)ui.step_mode << 4 | (uint32_t)ui.cursor << 8);
    h = hash(h, ui.sug_sig);
    for (s = 0; s < loop_steps(); s++)
        h = hash(h, om_pat[s]);
    for (s = 0; s < OM_MAXBARS; s++)
        h = hash(h, proj.prog[s]);
    h = hash(hash(h, (uint32_t)om_playing | (uint32_t)om_recording << 1 | (uint32_t)om_counting << 2 |
                         (uint32_t)om_note_on << 8 | (uint32_t)(uint8_t)ui.last_note << 16),
             (uint32_t)(om_playing ? om_step * 4 + (int)(om_step_frac * 4.0f) : 0) | (uint32_t)bars << 12 |
                 (uint32_t)ui.prog_bar << 16 | (uint32_t)ui.sug_bar << 20 | (uint32_t)ui.sug_pick << 24 |
                 (uint32_t)proj.par[P_SCALE] << 26);
    h = hash(h, (uint32_t)proj.par[P_ROOT] | (uint32_t)proj.par[P_ARPPAT] << 4 | (uint32_t)om_arp_on << 8 |
                    (uint32_t)proj.par[P_SNAP] << 9);
    if (h == ui.sig[1])
        return;
    ui.sig[1] = h;
    cv_begin(240, MAIN_H, K_BG);
    {   /* the note, big; the bar's chord; the state */
        int n = om_note_on ? om_note_on : ui.last_note;
        int32_t x = 10;
        uint8_t c[4];
        if (n > 0) {
            note_name(n, t);
            x = cv_text(10, 0, &FONT_L, t, om_note_on ? K_TEXT : K_DIM) + 10;
        } else {
            x = cv_text(10, 8, &FONT_B, "Play a key", K_DIM) + 10;
        }
        if (bar_chord(cur_bar(), c)) {
            int v = proj.prog[cur_bar()] & 0x7F;
            chord_label(v - 1, proj.prog[cur_bar()] >> 7, 1, t);
            cv_text(x, 10, &FONT_S, t, C_CHD);
        }
        if (om_counting) {
            scpy(t, "COUNT ");
            itoa_u(om_counting, q);
            scat_(t, q);
        } else if (om_playing || ui.step_mode) {
            int st = om_playing ? om_step : ui.cursor;
            scpy(t, om_recording ? "REC " : ui.step_mode ? "STEP " : "");
            itoa_u((uint32_t)(st / OM_BAR + 1), q);
            scat_(t, q);
            scat_(t, ".");
            itoa_u((uint32_t)(st % OM_BAR / 4 + 1), q);
            scat_(t, q);
        } else {
            t[0] = 0;
        }
        cv_text(230 - text_w(&FONT_B, t), 8, &FONT_B, t, om_recording || ui.step_mode || om_counting ? K_REC : K_LABEL);
    }
    {   /* tags: arp, snap */
        int32_t x = 10, y = 30;
        if (proj.par[P_ARPPAT]) {
            om_param_text(P_ARPPAT, proj.par[P_ARPPAT], q);
            scpy(t, "ARP ");
            scat_(t, q);
            cv_round(x, y, text_w(&FONT_XS, t) + 10, 14, 4, om_arp_on ? K_ACC : K_ACC_T);
            x = cv_text(x + 5, y, &FONT_XS, t, om_arp_on ? K_BG : K_ACC) + 9;
        }
        if (proj.par[P_SNAP]) {
            scpy(t, proj.par[P_SNAP] == SNAP_CHORD ? "SNAP CHORD" : "SNAP SCALE");
            cv_round(x, y, text_w(&FONT_XS, t) + 10, 14, 4, C_CHD_T);
            cv_text(x + 5, y, &FONT_XS, t, C_CHD);
        }
        if (ui.view == V_SUGGEST) {               /* the bar's picks */
            int b = ui.sug_bar, k;
            int32_t xx = 230;
            for (k = ui.sug_n[b] - 1; k >= 0; k--) {
                chord_label(ui.sug_deg[b][k], ui.sug_7th[b][k], 1, t);
                xx -= text_w(&FONT_XS, t) + 10;
                if (k == ui.sug_pick)
                    cv_round(xx - 4, y, text_w(&FONT_XS, t) + 8, 14, 4, C_CHD);
                cv_text(xx, y, &FONT_XS, t, k == ui.sug_pick ? K_BG : C_CHD);
            }
        }
    }
    draw_roll(48, 58);
    draw_lane(110, ui.view == V_SUGGEST);
    cv_blit(0, MAIN_Y);
}

static void draw_knobs(void)
{
    uint32_t h = hash(2166136261u, ui.view | (uint32_t)ui.touched << 8), i;
    int now_touch = plat_ms() < ui.touch_until;
    for (i = 0; i < 4; i++)
        h = hash(h, (uint32_t)knob_get(VIEW_KNOB[ui.view][i]));
    h = hash(hash(h, (uint32_t)now_touch), (uint32_t)proj.prog[ui.prog_bar] | (uint32_t)proj.par[P_SCALE] << 8);
    if (h == ui.sig[2])
        return;
    ui.sig[2] = h;
    cv_begin(240, KNB_H, K_BG);
    cv_round(4, 2, 232, KNB_H - 6, 6, K_HDR);
    cv_text(10, 3, &FONT_XS, VIEW_NAME[ui.view], K_ACC);
    for (i = 0; i < 4; i++) {
        int k = VIEW_KNOB[ui.view][i], lo, hi, v;
        int32_t x = (int32_t)i * 60, cx = x + 30, bw;
        int hot = now_touch && ui.touched == (int)i;
        char t[16];
        if (k == K_NONE)
            continue;
        if (i)
            cv_rect(x, 18, 1, KNB_H - 32, K_LINE);
        knob_range(k, &lo, &hi);
        v = knob_get(k);
        text_c(cx, 16, &FONT_XS, knob_name(k), hot ? K_TEXT : K_LABEL);
        knob_text(k, t);
        {
            const char *qq = t;
            int lower = 0;
            for (; *qq; qq++)
                lower |= *qq >= 'a' && *qq <= 'z';
            if (lower || text_w(&FONT_M, t) > 56)
                text_c(cx, 32, text_w(&FONT_B, t) > 58 ? &FONT_XS : &FONT_B, t, hot ? K_ACC : K_TEXT);
            else
                text_c(cx, 29, &FONT_M, t, hot ? K_ACC : K_TEXT);
        }
        if (k == K_FILL || k == K_APPLY)
            continue;
        bw = hi > lo ? (int32_t)(44 * (v - lo) / (hi - lo)) : 0;
        cv_round(cx - 22, 54, 44, 5, 2, K_LINE);
        if (lo < 0 && hi > 0) {
            int32_t m = cx - 22 + 44 * (0 - lo) / (hi - lo), e = cx - 22 + bw;
            cv_round(e < m ? e : m, 54, (e < m ? m - e : e - m) + 5, 5, 2, K_ACC);
        } else if (bw) {
            cv_round(cx - 22, 54, bw < 5 ? 5 : bw, 5, 2, K_ACC);
        }
    }
    cv_blit(0, KNB_Y);
}

/* the lights: the page's button, PLAY (red while recording), REC, ARP; the keys of the bar's chord
 * (its root blinking) or of the scale, and the keys held */
static void leds(void)
{
    uint32_t b = 0, k = 0, i;
    static const int8_t VIEW_BTN[NVIEWS] = {B_HOME, B_FX, B_SEQ, B_SEL, -1, -1, B_GLO};
    int blink = (ui.frame / 10u) & 1u;
    b |= VIEW_BTN[ui.view] >= 0 ? 1u << VIEW_BTN[ui.view] : 1u << B_EDIT;
    if (om_counting)
        b |= blink ? 1u << B_PLAY : 0u;
    else if (om_playing)
        b |= (om_step & 3u) < 2u ? 1u << B_PLAY : 0u;
    if (om_recording || ui.step_mode)
        b |= 1u << B_REC;
    if (proj.par[P_ARPPAT])
        b |= 1u << B_ARP;
    if (proj.leds) {
        uint8_t c[4];
        int n = proj.leds == 1 ? bar_chord(cur_bar(), c) : 0, pcs = 0, root = -1;
        for (i = 0; i < (uint32_t)n; i++)
            pcs |= 1 << (c[i] % 12);
        if (n)
            root = c[0] % 12;
        for (i = 0; i < NKEYS_; i++) {
            int note = key_note((int)i), pc = note % 12;
            int lit = n ? (pcs >> pc & 1) : proj.leds == 2 && om_in_scale(proj.par[P_SCALE], proj.par[P_ROOT], note);
            if (lit && pc == root && !blink)
                lit = 0;                         /* the root blinks */
            if (lit)
                k |= 1u << i;
        }
    }
    k |= ui.keys;                                /* and the keys held */
    plat_glow(proj.leds != 0);
    plat_play_red(om_recording);
    plat_leds(b, k);
}

#define AUTOSAVE_QUIET 4000u
static void autosave(void)
{
    uint32_t now = plat_ms();
    if (!ui.dirty || om_playing || om_counting || ui.btn || ui.keys || now - ui.act_t < AUTOSAVE_QUIET ||
        om_note_on || om_env > 0.001f)
        return;
    if (project_save() == 0)
        ui.dirty = 0;
}

void ui_init(void)
{
    memset(&ui, 0, sizeof ui);
    ui.touched = -1;
    ui.last_note = -1;
    ui.sug_sig = 1;
    ui.rec_seen = om_rec_writes;
    lcd_fill(0, 0, 240, 240, K_BG);
}

void ui_frame(void)
{
    input();
    autosave();
    draw_header();
    draw_main();
    draw_knobs();
    leds();
    ui.frame++;
}

void ui_input_only(void) { input(); }
