/*
 * The remaining exports of NavigationLib (previews), SpeechLib, ControlStripLib,
 * FoldersLib, ContextualMenu, NameRegistryLib and DriverServicesLib.
 *
 * Previews (NavCreatePreview and the NavPreview calls): a file's preview is
 * a 'PICT' in its resource fork that a 'pnot' resource (ID 0: modification
 * date, version 0, resource type, resource ID, as the Preview Components
 * keep them) points to.  NavCreatePreview(fileDesc, 'PICT', data, size)
 * writes both; a preview reference (NavCreatePreviewReference(fsspec,
 * *ref)) holds the picture found through the 'pnot', for the area given by
 * NavSetupPreview/NavSetPreviewArea(ref, rect), drawn by NavDrawPreview(ref)
 * into the current port; NavHandlePreviewEvent(ref, event) draws on an
 * update event and takes a mouse-down in the area.  The 'pnot' date is the
 * time it was written, and is not checked against the file's.
 *
 * SpeechLib: RegisterVoices/UnRegisterVoices(voiceSpecs, count) add and
 * remove voices GetIndVoice lists; Get/SetSpeechDefaults(record) are 5
 * longs: the VoiceSpec's creator and ID, then rate, pitch and volume
 * (Fixed) for the channels made after; AddSpeechChannelToList(channel)
 * checks the channel, every channel being on the list SpeechBusySystemWide
 * counts.
 */
#include <stdlib.h>
#include <string.h>

#include "carbon_events.h"
#include "ppc_bridge.h"
#include "tb_internal.h"
#include "os9rest.h"
#include "os9left.h"

enum { nsvErr = -35, rfNumErr = -51 };
#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (b) << 16 | (c) << 8 | (d))

/* ---- Navigation previews ---- */

enum { REF_NAVPREVIEW = 150 };

typedef struct {
    gaddr id;
    gaddr picture; /* a PICT handle the reference owns, or 0 */
    int16_t rect[4];
    bool active;
} preview;

static preview *preview_of(gaddr ref) { return ref_get(ref, REF_NAVPREVIEW); }

/* The FSSpec in an AEDesc of type 'fss '. */
static gaddr spec_of_desc(gaddr desc)
{
    if (!desc || rd32(desc) != FOURCC('f', 's', 's', ' ') || !rd32(desc + 4))
        return 0;
    return rd32(rd32(desc + 4));
}

int16_t tb_NavCreatePreview(gaddr theObject, int32_t previewDataType, gaddr previewData, int32_t previewDataSize)
{
    gaddr spec = spec_of_desc(theObject);
    if (!spec || !previewData || previewDataSize < 1)
        return paramErr;
    if ((uint32_t)previewDataType != FOURCC('P', 'I', 'C', 'T'))
        return paramErr;
    int16_t saved = tb_CurResFile();
    int16_t rn = tb_FSpOpenResFile(spec, 3);
    if (rn < 0) {
        tb_FSpCreateResFile(spec, FOURCC('?', '?', '?', '?'), FOURCC('?', '?', '?', '?'), 0);
        rn = tb_FSpOpenResFile(spec, 3);
    }
    if (rn < 0)
        return tb_ResError() ? tb_ResError() : rfNumErr;
    tb_UseResFile(rn);
    /* A preview already there goes. */
    gaddr old = tb_Get1Resource(FOURCC('p', 'n', 'o', 't'), 0);
    if (old && rd32(old) && tb_GetHandleSize(old) >= 12) {
        uint32_t type = rd32(rd32(old) + 6);
        int16_t id = (int16_t)rd16(rd32(old) + 10);
        gaddr pic = tb_Get1Resource((int32_t)type, id);
        if (pic)
            tb_RemoveResource(pic);
        tb_RemoveResource(old);
    }
    uint32_t pic = 0, note = 0;
    int16_t e = tb_PtrToHand(previewData, &pic, previewDataSize);
    if (!e) {
        note = tb_NewHandle(14, false, true);
        e = note ? noErr : memFullErr;
    }
    if (e) {
        tb_CloseResFile(rn);
        tb_UseResFile(saved);
        return e;
    }
    int16_t id = tb_Unique1ID(FOURCC('P', 'I', 'C', 'T'));
    tb_AddResource(pic, FOURCC('P', 'I', 'C', 'T'), id, 0);
    gaddr n = rd32(note);
    gaddr when = tb_NewPtr(4, false, true);
    tb_GetDateTime(when);
    wr32(n, rd32(when));
    tb_DisposePtr(when);
    wr16(n + 4, 0);
    wr32(n + 6, FOURCC('P', 'I', 'C', 'T'));
    wr16(n + 10, (uint16_t)id);
    wr16(n + 12, 0);
    tb_AddResource(note, FOURCC('p', 'n', 'o', 't'), 0, 0);
    tb_UpdateResFile(rn);
    tb_CloseResFile(rn);
    tb_UseResFile(saved);
    return noErr;
}

static void nav_NavCreatePreviewReference(ppc_cpu *c)
{
    if (!R(0) || !R(1))
        RETURN(paramErr);
    wr32(R(1), 0);
    int16_t saved = tb_CurResFile();
    int16_t rn = tb_FSpOpenResFile(R(0), 1);
    if (rn < 0)
        RETURN(tb_ResError() ? tb_ResError() : rfNumErr);
    tb_UseResFile(rn);
    preview *p = calloc(1, sizeof *p);
    if (!p) {
        tb_CloseResFile(rn);
        tb_UseResFile(saved);
        RETURN(memFullErr);
    }
    gaddr note = tb_Get1Resource(FOURCC('p', 'n', 'o', 't'), 0);
    if (note && rd32(note) && tb_GetHandleSize(note) >= 12) {
        uint32_t type = rd32(rd32(note) + 6);
        int16_t id = (int16_t)rd16(rd32(note) + 10);
        gaddr pic = tb_Get1Resource((int32_t)type, id);
        if (pic && type == FOURCC('P', 'I', 'C', 'T')) {
            tb_DetachResource(pic);
            p->picture = pic;
        }
    }
    tb_CloseResFile(rn);
    tb_UseResFile(saved);
    p->id = ref_new(REF_NAVPREVIEW, p);
    wr32(R(1), p->id);
    RETURN(noErr);
}

static void nav_NavDisposePreviewReference(ppc_cpu *c)
{
    preview *p = preview_of(R(0));
    if (!p)
        RETURN(paramErr);
    if (p->picture)
        tb_DisposeHandle(p->picture);
    ref_free(p->id);
    free(p);
    RETURN(noErr);
}

static void nav_NavHasPreview(ppc_cpu *c)
{
    preview *p = preview_of(R(0));
    RETURN(p && p->picture ? 1 : 0);
}

static void set_area(preview *p, gaddr r)
{
    for (int i = 0; i < 4; i++)
        p->rect[i] = (int16_t)rd16(r + 2u * (unsigned)i);
}

static void nav_NavSetPreviewArea(ppc_cpu *c)
{
    preview *p = preview_of(R(0));
    if (!p || !R(1))
        RETURN(paramErr);
    set_area(p, R(1));
    RETURN(noErr);
}

/* NavSetupPreview(ref, rect): the area, and the preview is on show. */
static void nav_NavSetupPreview(ppc_cpu *c)
{
    preview *p = preview_of(R(0));
    if (!p || !R(1))
        RETURN(paramErr);
    set_area(p, R(1));
    p->active = true;
    RETURN(noErr);
}

static void nav_NavGetPreviewArea(ppc_cpu *c)
{
    preview *p = preview_of(R(0));
    if (!p || !R(1))
        RETURN(paramErr);
    for (int i = 0; i < 4; i++)
        wr16(R(1) + 2u * (unsigned)i, (uint16_t)p->rect[i]);
    RETURN(noErr);
}

static int16_t draw(preview *p)
{
    gaddr r = tb_NewPtr(8, false, true);
    if (!r)
        return memFullErr;
    for (int i = 0; i < 4; i++)
        wr16(r + 2u * (unsigned)i, (uint16_t)p->rect[i]);
    if (p->picture)
        tb_DrawPicture(p->picture, r);
    tb_FrameRect(r);
    tb_DisposePtr(r);
    return noErr;
}

static void nav_NavDrawPreview(ppc_cpu *c)
{
    preview *p = preview_of(R(0));
    if (!p)
        RETURN(paramErr);
    RETURN(draw(p));
}

/* NavHandlePreviewEvent(ref, event): an update is answered by drawing,
 * a mouse-down in the area is the preview's. */
static void nav_NavHandlePreviewEvent(ppc_cpu *c)
{
    preview *p = preview_of(R(0));
    if (!p || !R(1) || !p->active)
        RETURN(0);
    uint16_t what = rd16(R(1));
    if (what == 6) { /* updateEvt */
        draw(p);
        RETURN(1);
    }
    if (what == 1) { /* mouseDown: where, in global coordinates, is the event's point */
        gaddr r = tb_NewPtr(8, false, true);
        if (!r)
            RETURN(0);
        for (int i = 0; i < 4; i++)
            wr16(r + 2u * (unsigned)i, (uint16_t)p->rect[i]);
        int8_t in = tb_PtInRect(rd32(R(1) + 10), r);
        tb_DisposePtr(r);
        RETURN(in != 0);
    }
    RETURN(0);
}

/* ---- SpeechLib ---- */

int16_t speech_register_voice(uint32_t creator, int32_t id);
int16_t speech_unregister_voice(uint32_t creator, int32_t id);
void speech_get_defaults(uint32_t *creator, int32_t *id, int32_t *rate, int32_t *pitch, int32_t *volume);
void speech_set_defaults(uint32_t creator, int32_t id, int32_t rate, int32_t pitch, int32_t volume);
int speech_channel_voice(gaddr chan);

static void sp_RegisterVoices(ppc_cpu *c)
{
    if (!R(0) || (int32_t)R(1) < 1)
        RETURN(paramErr);
    for (int32_t i = 0; i < (int32_t)R(1); i++) {
        int16_t e = speech_register_voice(rd32(R(0) + 8u * (uint32_t)i), (int32_t)rd32(R(0) + 8u * (uint32_t)i + 4));
        if (e)
            RETURN(e);
    }
    RETURN(noErr);
}

static void sp_UnRegisterVoices(ppc_cpu *c)
{
    if (!R(0) || (int32_t)R(1) < 1)
        RETURN(paramErr);
    int16_t err = noErr;
    for (int32_t i = 0; i < (int32_t)R(1); i++) {
        int16_t e = speech_unregister_voice(rd32(R(0) + 8u * (uint32_t)i), (int32_t)rd32(R(0) + 8u * (uint32_t)i + 4));
        if (e)
            err = e;
    }
    RETURN(err);
}

static void sp_GetSpeechDefaults(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    uint32_t creator;
    int32_t id, rate, pitch, volume;
    speech_get_defaults(&creator, &id, &rate, &pitch, &volume);
    wr32(R(0), creator);
    wr32(R(0) + 4, (uint32_t)id);
    wr32(R(0) + 8, (uint32_t)rate);
    wr32(R(0) + 12, (uint32_t)pitch);
    wr32(R(0) + 16, (uint32_t)volume);
    RETURN(noErr);
}

static void sp_SetSpeechDefaults(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    speech_set_defaults(rd32(R(0)), (int32_t)rd32(R(0) + 4), (int32_t)rd32(R(0) + 8), (int32_t)rd32(R(0) + 12),
                        (int32_t)rd32(R(0) + 16));
    RETURN(noErr);
}

static void sp_AddSpeechChannelToList(ppc_cpu *c) { RETURN(speech_channel_voice(R(0)) >= 0 ? noErr : paramErr); }

/* ---- ControlStripLib ---- */

/* SBCallProcInFrontProcess(proc, refCon): the front process is this one,
 * and the routine is called here. */
static void st_SBCallProcInFrontProcess(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    uint32_t arg = R(1);
    RETURN(ppc_call(R(0), &arg, 1));
}

/* SBGetPopupMenuCheckMark(): the character the strip's pop-up menus mark
 * the current item with: the check mark. */
static void st_SBGetPopupMenuCheckMark(ppc_cpu *c) { RETURN(0x12); }

/* SBControlStripSetup(): there is no Control Strip to set up; what Mac OS
 * answers when the extension isn't there. */
static void st_SBControlStripSetup(ppc_cpu *c) { RETURN((int16_t)-4); }

/* ---- Calls that are other calls' work ---- */

static uint32_t pass(ppc_cpu *c, const char *name, int n)
{
    uint32_t a[8];
    for (int i = 0; i < n; i++)
        a[i] = R(i);
    return os9rest_call(c, name, a, n);
}

/* FindFolderInternal: FindFolder's work under the name its glue calls. */
static void fo_FindFolderInternal(ppc_cpu *c) { RETURN(pass(c, "FindFolder", 5)); }

/* PopUpMenuSelectSticky(menu, top, left, item): the pop-up menu that stays
 * open after a click that let go at once (PopUpMenuSelect does so when a
 * person is at the mouse). */
static void cm_PopUpMenuSelectSticky(ppc_cpu *c) { RETURN(pass(c, "PopUpMenuSelect", 4)); }

/* RegistryDeviceSearch: RegistryEntrySearch among the devices. */
static void nr_RegistryDeviceSearch(ppc_cpu *c) { RETURN(pass(c, "RegistryEntrySearch", 7)); }

/* ---- DriverServicesLib ---- */

int32_t os9svc_lookup_members(uint32_t setID, int32_t first, int32_t count, gaddr out);

static void ds_LookupInterruptMembers(ppc_cpu *c) { RETURN(os9svc_lookup_members(R(0), (int32_t)R(1), (int32_t)R(2), R(3))); }

void os9left_misc_reset(void) {}

const ppc_export os9left_misc_exports[] = {
    { "AddSpeechChannelToList", sp_AddSpeechChannelToList },
    { "FindFolderInternal", fo_FindFolderInternal },
    { "GetSpeechDefaults", sp_GetSpeechDefaults },
    { "LookupInterruptMembers", ds_LookupInterruptMembers },
    { "NavCreatePreviewReference", nav_NavCreatePreviewReference },
    { "NavDisposePreviewReference", nav_NavDisposePreviewReference },
    { "NavDrawPreview", nav_NavDrawPreview },
    { "NavGetPreviewArea", nav_NavGetPreviewArea },
    { "NavHandlePreviewEvent", nav_NavHandlePreviewEvent },
    { "NavHasPreview", nav_NavHasPreview },
    { "NavSetPreviewArea", nav_NavSetPreviewArea },
    { "NavSetupPreview", nav_NavSetupPreview },
    { "PopUpMenuSelectSticky", cm_PopUpMenuSelectSticky },
    { "RegisterVoices", sp_RegisterVoices },
    { "RegistryDeviceSearch", nr_RegistryDeviceSearch },
    { "SBCallProcInFrontProcess", st_SBCallProcInFrontProcess },
    { "SBControlStripSetup", st_SBControlStripSetup },
    { "SBGetPopupMenuCheckMark", st_SBGetPopupMenuCheckMark },
    { "SetSpeechDefaults", sp_SetSpeechDefaults },
    { "UnRegisterVoices", sp_UnRegisterVoices },
};
const unsigned os9left_misc_export_count = sizeof os9left_misc_exports / sizeof os9left_misc_exports[0];
