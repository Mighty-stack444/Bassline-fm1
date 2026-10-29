#define OM_HOST 1
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "app/plat.h"
#include "app/app.h"
static uint8_t blob[4096]; static int blen;
int plat_store_load(uint32_t o, void *d, uint32_t m) { (void)o; memcpy(d, blob, blen < (int)m ? blen : m); return blen; }
void om_midi_out(uint32_t a, uint32_t b, uint32_t c) { (void)a; (void)b; (void)c; }
int plat_store_save(uint32_t o, const void *s, uint32_t l) { (void)o; (void)s; (void)l; return 0; }
#include "app/project.c"
int main(void) {
    struct { uint32_t magic, format; int16_t par[24]; uint8_t prog[8]; uint8_t al, leds, rsv[14]; uint32_t pat[128]; } v1;
    int i, bad = 0;
    om_init();
    memset(&v1, 0, sizeof v1);
    v1.magic = PROJ_MAGIC; v1.format = 1;
    for (i = 0; i < 24; i++) v1.par[i] = om_param_info(i)->def;
    v1.par[P_SOUND] = 0; v1.par[P_TEMPO] = 140; v1.prog[1] = 6; v1.al = 3; v1.leds = 2;
    v1.pat[5] = PAT_MAKE(40, 3, PAT_GLIDE);
    memcpy(blob, &v1, sizeof v1); blen = sizeof v1;
    project_load();
    bad |= proj.format != 2 || proj.par[P_SOUND] != 0 || proj.par[P_TEMPO] != 140 || proj.prog[1] != 6;
    bad |= proj.arp_last != 3 || proj.leds != 2 || om_pat[5] != PAT_MAKE(40, 3, PAT_GLIDE);
    bad |= proj.par[P_PUNCH] != 4 || proj.par[P_RETURN] != 20;
    printf(bad ? "FAIL migration\n" : "ok: a 0.1 project loads in 0.2 (loop, progression, settings; new ones at defaults)\n");
    return bad;
}
