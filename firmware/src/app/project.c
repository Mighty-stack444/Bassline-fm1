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

int project_load(void)
{
    int n, i;
    memset(&proj, 0, sizeof proj);
    n = plat_store_load(OBJ_PROJ, &proj, sizeof proj);
    if (!(n >= 8 && project_valid()))
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
