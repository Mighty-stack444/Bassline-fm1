/* SPDX-License-Identifier: GPL-3.0-only */
/* BASSLINE project: one flash object (OBJ_PROJ): the settings, the progression and the loop. A
 * project of another format, or none, gives the defaults. Part of the unity build, after app.h. */
project_t proj;

void project_defaults(void)
{
    int i;
    memset(&proj, 0, sizeof proj);
    proj.magic = PROJ_MAGIC;
    proj.format = PROJ_FORMAT;
    for (i = 0; i < P_NPARAMS; i++)
        proj.par[i] = om_param_info(i)->def;
    proj.arp_last = 1;
    proj.leds = 1;
}

static int project_valid(void)
{
    int i;
    if (proj.magic != PROJ_MAGIC || proj.format != PROJ_FORMAT)
        return 0;
    for (i = 0; i < P_NPARAMS; i++) {
        const om_param_t *p = om_param_info(i);
        if (proj.par[i] < p->lo || proj.par[i] > p->hi)
            return 0;
    }
    if (proj.leds > 2u || proj.arp_last < 1u || proj.arp_last >= NARPPAT)
        return 0;
    for (i = 0; i < OM_MAXBARS; i++)
        if ((proj.prog[i] & 0x7Fu) > 7u)
            return 0;
    return 1;
}

/* a 0.1 project (format 1): 24 parameters, then the same fields. Its parameters keep their values;
 * the new ones (Shape, Bend) start at their defaults. */
typedef struct {
    uint32_t magic, format;
    int16_t par[OM_NPARAMS_V1];
    uint8_t prog[OM_MAXBARS];
    uint8_t arp_last, leds, rsv[14];
    uint32_t pat[OM_STEPS];
} project_v1_t;

static int project_migrate(void)
{
    static project_v1_t v1;
    int i;
    memcpy(&v1, &proj, sizeof v1 <= sizeof proj ? sizeof v1 : sizeof proj);
    if (v1.magic != PROJ_MAGIC || v1.format != 1u)
        return 0;
    project_defaults();
    for (i = 0; i < OM_NPARAMS_V1; i++)
        proj.par[i] = v1.par[i];
    memcpy(proj.prog, v1.prog, sizeof proj.prog);
    proj.arp_last = v1.arp_last;
    proj.leds = v1.leds;
    memcpy(proj.pat, v1.pat, sizeof proj.pat);
    return project_valid();
}

int project_load(void)
{
    int n, i;
    memset(&proj, 0, sizeof proj);
    n = plat_store_load(OBJ_PROJ, &proj, sizeof proj);
    if (!(n >= 8 && (project_valid() || project_migrate())))
        project_defaults();
    for (i = 0; i < OM_STEPS; i++)
        om_pat[i] = proj.pat[i];
    return n >= 8 && proj.magic == PROJ_MAGIC ? 0 : -1;
}

int project_save(void)
{
    int i;
    for (i = 0; i < OM_STEPS; i++)
        proj.pat[i] = om_pat[i];
    return plat_store_save(OBJ_PROJ, &proj, sizeof proj);
}

void project_apply(void)
{
    int i;
    for (i = 0; i < P_NPARAMS; i++)
        om_set(i, proj.par[i]);
}
