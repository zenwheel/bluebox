/*
 * The Control Strip's services to the programs that load its modules'
 * resources and preferences.  Blue Box draws no strip and has no module
 * files, so a module's resource file is never found; preferences are kept
 * for the run, as the Control Strip kept them in its preferences file, and
 * detached strings come from a handle the program already has.
 */
#include <string.h>

#include "tb_internal.h"

#define MAX_STRIP_PREFS 32

typedef struct {
    uint8_t name[32]; /* a Pascal string, cut to fit */
    gaddr h;          /* the Control Strip's own copy */
} strip_pref;

static strip_pref strip_prefs[MAX_STRIP_PREFS];

static void pname(gaddr p, uint8_t out[32])
{
    uint8_t n = p ? rd8(p) : 0;
    if (n > 31)
        n = 31;
    out[0] = n;
    for (int i = 0; i < n; i++)
        out[1 + i] = rd8(p + 1 + i);
}

static strip_pref *pref_named(const uint8_t name[32], bool make)
{
    strip_pref *free_slot = NULL;
    for (int i = 0; i < MAX_STRIP_PREFS; i++) {
        strip_pref *s = &strip_prefs[i];
        if (s->h && s->name[0] == name[0] && !memcmp(s->name, name, 1u + name[0]))
            return s;
        if (!s->h && !free_slot)
            free_slot = s;
    }
    if (make && free_slot) {
        memset(free_slot, 0, sizeof *free_slot);
        memcpy(free_slot->name, name, 1u + name[0]);
    }
    return make ? free_slot : NULL;
}

/* A new handle holding a copy of h's contents. */
static gaddr copy_handle(gaddr h, bool sys)
{
    int32_t n = tb_GetHandleSize(h);
    gaddr c = tb_NewHandle(n < 0 ? 0 : n, sys, false);
    if (c && n > 0)
        tb_BlockMove(rd32(h), rd32(c), n, false);
    return c;
}

/* No module's resource file is open: the -1 OpenResFile gives. */
int16_t tb_SBOpenModuleResourceFile(int32_t fileCreator)
{
    (void)fileCreator;
    return -1;
}

int16_t tb_SBLoadPreferences(gaddr prefsResourceName, gaddr preferences)
{
    uint8_t name[32];
    pname(prefsResourceName, name);
    strip_pref *s = pref_named(name, false);
    gaddr h = s ? copy_handle(s->h, false) : 0;
    if (preferences)
        wr32(preferences, h);
    if (!s)
        return resNotFound;
    return h ? noErr : memFullErr;
}

int16_t tb_SBSavePreferences(gaddr prefsResourceName, gaddr preferences)
{
    uint8_t name[32];
    pname(prefsResourceName, name);
    if (!preferences || !rd32(preferences))
        return nilHandleErr;
    strip_pref *s = pref_named(name, true);
    if (!s)
        return memFullErr;
    gaddr h = copy_handle(preferences, true);
    if (!h)
        return memFullErr;
    if (s->h)
        tb_DisposeHandle(s->h);
    s->h = h;
    return noErr;
}

/* The nth (from 1) string of a STR# resource's data, in a string the
 * program gave; an empty one when the list has no such string. */
void tb_SBGetDetachedIndString(gaddr theString, gaddr stringList, int16_t whichString)
{
    if (!theString)
        return;
    wr8(theString, 0);
    if (!stringList || !rd32(stringList) || tb_GetHandleSize(stringList) < 2)
        return;
    int32_t size = tb_GetHandleSize(stringList);
    gaddr p = rd32(stringList);
    int32_t count = rd16(p);
    if (whichString < 1 || whichString > count)
        return;
    int32_t off = 2;
    for (int i = 1; i <= whichString; i++) {
        if (off >= size)
            return;
        int32_t n = rd8(p + off);
        if (i == whichString) {
            if (off + 1 + n > size)
                n = size - off - 1;
            wr8(theString, (uint8_t)n);
            for (int k = 0; k < n; k++)
                wr8(theString + 1 + k, rd8(p + off + 1 + k));
            return;
        }
        off += 1 + n;
    }
}
