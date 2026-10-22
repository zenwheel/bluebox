/*
 * Drag Manager: drags within the program's own windows.
 *
 * A drag holds items, each with flavors whose data is copied in (or
 * promised and fetched through the drag's send procedure when a receiver
 * asks).  TrackDrag follows the mouse with the drag region's gray outline,
 * as DragGrayRgn draws it, sends the tracking handlers of the window under
 * the mouse their enter, in and leave messages, and on release offers the
 * drag to that window's receive handlers.  Files and text dragged from
 * the Finder and other host programs come in the same way (machine_host_drag),
 * and a drag that leaves the program's windows goes on as the host's drag
 * (machine_drag_out_*), its flavors converted for other programs.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "forks.h"

#include "qd_internal.h"
#include "tb_internal.h"

enum {
    badDragRefErr = -1850, badDragItemErr = -1851, badDragFlavorErr = -1852, duplicateFlavorErr = -1853,
    cantGetFlavorErr = -1854, duplicateHandlerErr = -1855, handlerNotFoundErr = -1856,
    dragNotAcceptedErr = -1857,
};
enum { typeNull = 0x6E756C6C, typeAlias = 0x616C6973 }; /* 'null', 'alis' */
enum { kDragTrackingEnterHandler = 1, kDragTrackingEnterWindow, kDragTrackingInWindow, kDragTrackingLeaveWindow,
       kDragTrackingLeaveHandler };
enum { kDragHasLeftSenderWindow = 1, kDragInsideSenderApplication = 2, kDragInsideSenderWindow = 4 };
enum { patXor = 10 };

static gaddr wmgr_port(void) { return rd32(LM_WMgrPort); }

typedef struct {
    uint32_t type, flags;
    uint8_t *data;
    int32_t size;
    bool promised;              /* no data yet: the send procedure has it */
} flavor;

typedef struct {
    uint32_t ref;
    qrect bounds;
    int nflavors;
    flavor *flavors;
} item;

enum { MAX_DRAGS = 8, DRAG_TAG = 0x44520000 }; /* 'DR' and the slot */

typedef struct {
    bool used;
    int nitems;
    item *items;
    gaddr send_proc, send_refcon;
    uint32_t origin, mouse, pinned;
    uint16_t mods, down_mods, up_mods;
    uint32_t attributes;
    /* ShowDragHilite's frame, in its port's coordinates */
    bool hilite;
    gaddr hilite_port;
    hrgn hilite_rgn;
    char *drop_folder;          /* dropped on the host, in this folder */
} drag;

static drag drags[MAX_DRAGS];
static void drag_out_reset(void);
static uint32_t host_ref;  /* a drag from the host being tracked (below) */
static gaddr host_window;

typedef struct {
    gaddr proc, window, refcon;
    bool entered;               /* tracking: sent kDragTrackingEnterHandler */
} handler;

enum { MAX_HANDLERS = 32 };
static handler tracking[MAX_HANDLERS], receiving[MAX_HANDLERS];
static int ntracking, nreceiving;

static drag *get_drag(uint32_t ref)
{
    uint32_t i = ref - DRAG_TAG - 1;
    if ((ref & 0xFFFF0000u) != DRAG_TAG || i >= MAX_DRAGS || !drags[i].used)
        return NULL;
    return &drags[i];
}

static item *get_item(drag *d, uint32_t ref)
{
    for (int i = 0; i < d->nitems; i++)
        if (d->items[i].ref == ref)
            return &d->items[i];
    return NULL;
}

static flavor *get_flavor(item *it, uint32_t type)
{
    for (int i = 0; i < it->nflavors; i++)
        if (it->flavors[i].type == type)
            return &it->flavors[i];
    return NULL;
}

static void free_drag(drag *d)
{
    for (int i = 0; i < d->nitems; i++) {
        for (int j = 0; j < d->items[i].nflavors; j++)
            free(d->items[i].flavors[j].data);
        free(d->items[i].flavors);
    }
    free(d->items);
    if (d->hilite)
        hrgn_free(&d->hilite_rgn);
    free(d->drop_folder);
    memset(d, 0, sizeof *d);
}

void drag_reset(void)
{
    for (int i = 0; i < MAX_DRAGS; i++)
        if (drags[i].used)
            free_drag(&drags[i]);
    ntracking = nreceiving = 0;
    host_ref = 0;
    host_window = 0;
    drag_out_reset();
}

/* ---------------------------------------------------------------------- */
/* Handlers */

static int16_t install(handler *list, int *n, gaddr proc, gaddr window, gaddr refcon)
{
    for (int i = 0; i < *n; i++)
        if (list[i].proc == proc && list[i].window == window)
            return duplicateHandlerErr;
    if (*n == MAX_HANDLERS)
        return memFullErr;
    list[(*n)++] = (handler){ proc, window, refcon, false };
    return noErr;
}

static int16_t remove_handler(handler *list, int *n, gaddr proc, gaddr window)
{
    for (int i = 0; i < *n; i++)
        if (list[i].proc == proc && list[i].window == window) {
            memmove(&list[i], &list[i + 1], (size_t)(*n - i - 1) * sizeof *list);
            (*n)--;
            return noErr;
        }
    return handlerNotFoundErr;
}

int16_t tb_InstallTrackingHandler(gaddr trackingHandler, gaddr theWindow, gaddr handlerRefCon)
{
    return install(tracking, &ntracking, trackingHandler, theWindow, handlerRefCon);
}

int16_t tb_InstallReceiveHandler(gaddr receiveHandler, gaddr theWindow, gaddr handlerRefCon)
{
    return install(receiving, &nreceiving, receiveHandler, theWindow, handlerRefCon);
}

int16_t tb_RemoveTrackingHandler(gaddr trackingHandler, gaddr theWindow)
{
    return remove_handler(tracking, &ntracking, trackingHandler, theWindow);
}

int16_t tb_RemoveReceiveHandler(gaddr receiveHandler, gaddr theWindow)
{
    return remove_handler(receiving, &nreceiving, receiveHandler, theWindow);
}

/* A handler serves its own window, or (window nil) every window. */
static bool serves(const handler *h, gaddr w) { return w && (!h->window || h->window == w); }

/* pascal OSErr TrackingHandler(message, theWindow, refCon, theDrag) */
static int16_t call_tracking(const handler *h, int16_t message, gaddr w, uint32_t ref)
{
    cpu_push16(g_cpu, 0);
    cpu_push16(g_cpu, (uint16_t)message);
    cpu_push32(g_cpu, w);
    cpu_push32(g_cpu, h->refcon);
    cpu_push32(g_cpu, ref);
    guest_call(h->proc);
    return (int16_t)cpu_pop16(g_cpu);
}

/* pascal OSErr ReceiveHandler(theWindow, refCon, theDrag) */
static int16_t call_receive(const handler *h, gaddr w, uint32_t ref)
{
    cpu_push16(g_cpu, 0);
    cpu_push32(g_cpu, w);
    cpu_push32(g_cpu, h->refcon);
    cpu_push32(g_cpu, ref);
    guest_call(h->proc);
    return (int16_t)cpu_pop16(g_cpu);
}

/* Each handler serving w gets the message (handlers may remove themselves). */
static void tell(int16_t message, gaddr w, uint32_t ref)
{
    for (int i = 0; i < ntracking; i++)
        if (serves(&tracking[i], w)) {
            handler h = tracking[i];
            call_tracking(&h, message, w, ref);
        }
}

/* The drag moves from window from to window to (either may be nil). */
static void change_window(gaddr from, gaddr to, uint32_t ref)
{
    if (from) {
        tell(kDragTrackingLeaveWindow, from, ref);
        for (int i = 0; i < ntracking; i++)
            if (tracking[i].entered && serves(&tracking[i], from) && !serves(&tracking[i], to)) {
                tracking[i].entered = false;
                handler h = tracking[i];
                call_tracking(&h, kDragTrackingLeaveHandler, from, ref);
            }
    }
    if (to) {
        for (int i = 0; i < ntracking; i++)
            if (!tracking[i].entered && serves(&tracking[i], to)) {
                tracking[i].entered = true;
                handler h = tracking[i];
                call_tracking(&h, kDragTrackingEnterHandler, to, ref);
            }
        tell(kDragTrackingEnterWindow, to, ref);
    }
}

/* ---------------------------------------------------------------------- */
/* Drags and their items */

int16_t tb_NewDrag(gaddr theDrag)
{
    for (int i = 0; i < MAX_DRAGS; i++)
        if (!drags[i].used) {
            memset(&drags[i], 0, sizeof drags[i]);
            drags[i].used = true;
            wr32(theDrag, DRAG_TAG + (uint32_t)i + 1);
            return noErr;
        }
    wr32(theDrag, 0);
    return memFullErr;
}

int16_t tb_DisposeDrag(uint32_t theDrag)
{
    drag *d = get_drag(theDrag);
    if (!d)
        return badDragRefErr;
    free_drag(d);
    return noErr;
}

static int16_t put_data(flavor *f, gaddr dataPtr, int32_t dataSize, uint32_t offset)
{
    if (dataSize < 0)
        return paramErr;
    uint32_t end = offset + (uint32_t)dataSize;
    if (end > (uint32_t)f->size) {
        uint8_t *p = realloc(f->data, end ? end : 1);
        if (!p)
            return memFullErr;
        memset(p + f->size, 0, end - (uint32_t)f->size);
        f->data = p;
        f->size = (int32_t)end;
    }
    if (dataPtr && dataSize)
        gget(f->data + offset, dataPtr, (uint32_t)dataSize);
    f->promised = false;
    return noErr;
}

int16_t tb_AddDragItemFlavor(uint32_t theDrag, uint32_t theItemRef, int32_t theType, gaddr dataPtr,
                             int32_t dataSize, uint32_t theFlags)
{
    drag *d = get_drag(theDrag);
    if (!d)
        return badDragRefErr;
    item *it = get_item(d, theItemRef);
    if (!it) {
        item *grown = realloc(d->items, (size_t)(d->nitems + 1) * sizeof *grown);
        if (!grown)
            return memFullErr;
        d->items = grown;
        it = &d->items[d->nitems++];
        memset(it, 0, sizeof *it);
        it->ref = theItemRef;
    }
    if (get_flavor(it, theType))
        return duplicateFlavorErr;
    flavor *grown = realloc(it->flavors, (size_t)(it->nflavors + 1) * sizeof *grown);
    if (!grown)
        return memFullErr;
    it->flavors = grown;
    flavor *f = &it->flavors[it->nflavors++];
    memset(f, 0, sizeof *f);
    f->type = theType;
    f->flags = theFlags;
    if (!dataPtr) { /* promised: the send procedure provides it */
        f->promised = true;
        return noErr;
    }
    return put_data(f, dataPtr, dataSize, 0);
}

int16_t tb_SetDragItemFlavorData(uint32_t theDrag, uint32_t theItemRef, int32_t theType, gaddr dataPtr,
                                 int32_t dataSize, uint32_t dataOffset)
{
    drag *d = get_drag(theDrag);
    if (!d)
        return badDragRefErr;
    item *it = get_item(d, theItemRef);
    if (!it)
        return badDragItemErr;
    flavor *f = get_flavor(it, theType);
    if (!f)
        return badDragFlavorErr;
    return put_data(f, dataPtr, dataSize, dataOffset);
}

int16_t tb_SetDragSendProc(uint32_t theDrag, gaddr sendProc, gaddr dragSendRefCon)
{
    drag *d = get_drag(theDrag);
    if (!d)
        return badDragRefErr;
    d->send_proc = sendProc;
    d->send_refcon = dragSendRefCon;
    return noErr;
}

int16_t tb_SetDragInputProc(uint32_t theDrag, gaddr inputProc, gaddr dragInputRefCon)
{
    return get_drag(theDrag) ? noErr : badDragRefErr;
}

int16_t tb_SetDragDrawingProc(uint32_t theDrag, gaddr drawingProc, gaddr dragDrawingRefCon)
{
    return get_drag(theDrag) ? noErr : badDragRefErr;
}

int16_t tb_CountDragItems(uint32_t theDrag, gaddr numItems)
{
    drag *d = get_drag(theDrag);
    if (!d)
        return badDragRefErr;
    wr16(numItems, (uint16_t)d->nitems);
    return noErr;
}

int16_t tb_GetDragItemReferenceNumber(uint32_t theDrag, uint16_t index, gaddr theItemRef)
{
    drag *d = get_drag(theDrag);
    if (!d)
        return badDragRefErr;
    if (index < 1 || index > d->nitems)
        return badDragItemErr;
    wr32(theItemRef, d->items[index - 1].ref);
    return noErr;
}

int16_t tb_CountDragItemFlavors(uint32_t theDrag, uint32_t theItemRef, gaddr numFlavors)
{
    drag *d = get_drag(theDrag);
    if (!d)
        return badDragRefErr;
    item *it = get_item(d, theItemRef);
    if (!it)
        return badDragItemErr;
    wr16(numFlavors, (uint16_t)it->nflavors);
    return noErr;
}

int16_t tb_GetFlavorType(uint32_t theDrag, uint32_t theItemRef, uint16_t index, gaddr theType)
{
    drag *d = get_drag(theDrag);
    if (!d)
        return badDragRefErr;
    item *it = get_item(d, theItemRef);
    if (!it)
        return badDragItemErr;
    if (index < 1 || index > it->nflavors)
        return badDragFlavorErr;
    wr32(theType, it->flavors[index - 1].type);
    return noErr;
}

int16_t tb_GetFlavorFlags(uint32_t theDrag, uint32_t theItemRef, int32_t theType, gaddr theFlags)
{
    drag *d = get_drag(theDrag);
    if (!d)
        return badDragRefErr;
    item *it = get_item(d, theItemRef);
    if (!it)
        return badDragItemErr;
    flavor *f = get_flavor(it, theType);
    if (!f)
        return badDragFlavorErr;
    wr32(theFlags, f->flags);
    return noErr;
}

/* A flavor's data, asking the sender for promised data. */
static flavor *fetch(uint32_t theDrag, uint32_t theItemRef, uint32_t theType, int16_t *err)
{
    drag *d = get_drag(theDrag);
    if (!d) {
        *err = badDragRefErr;
        return NULL;
    }
    item *it = get_item(d, theItemRef);
    if (!it) {
        *err = badDragItemErr;
        return NULL;
    }
    flavor *f = get_flavor(it, theType);
    if (!f) {
        *err = badDragFlavorErr;
        return NULL;
    }
    if (f->promised && d->send_proc) {
        /* pascal OSErr SendDataProc(theType, refCon, theItemRef, theDrag) */
        gaddr proc = d->send_proc, refcon = d->send_refcon;
        cpu_push16(g_cpu, 0);
        cpu_push32(g_cpu, theType);
        cpu_push32(g_cpu, refcon);
        cpu_push32(g_cpu, theItemRef);
        cpu_push32(g_cpu, theDrag);
        guest_call(proc);
        int16_t e = (int16_t)cpu_pop16(g_cpu);
        /* the drag's items may have moved while the sender added data */
        if (!(d = get_drag(theDrag)) || !(it = get_item(d, theItemRef)) || !(f = get_flavor(it, theType))) {
            *err = cantGetFlavorErr;
            return NULL;
        }
        if (e) {
            *err = e;
            return NULL;
        }
    }
    if (f->promised) {
        *err = cantGetFlavorErr;
        return NULL;
    }
    *err = noErr;
    return f;
}

int16_t tb_GetFlavorDataSize(uint32_t theDrag, uint32_t theItemRef, int32_t theType, gaddr dataSize)
{
    int16_t err;
    flavor *f = fetch(theDrag, theItemRef, theType, &err);
    if (f)
        wr32(dataSize, (uint32_t)f->size);
    return err;
}

int16_t tb_GetFlavorData(uint32_t theDrag, uint32_t theItemRef, int32_t theType, gaddr dataPtr, gaddr dataSize,
                         uint32_t dataOffset)
{
    int16_t err;
    flavor *f = fetch(theDrag, theItemRef, theType, &err);
    if (!f)
        return err;
    int32_t want = (int32_t)rd32(dataSize);
    int32_t have = dataOffset < (uint32_t)f->size ? f->size - (int32_t)dataOffset : 0;
    int32_t n = want < have ? want : have;
    if (n > 0)
        gput(dataPtr, f->data + dataOffset, (uint32_t)n);
    wr32(dataSize, (uint32_t)(n > 0 ? n : 0));
    return noErr;
}

int16_t tb_GetDragItemBounds(uint32_t theDrag, uint32_t theItemRef, gaddr itemBounds)
{
    drag *d = get_drag(theDrag);
    if (!d)
        return badDragRefErr;
    item *it = get_item(d, theItemRef);
    if (!it)
        return badDragItemErr;
    qrect_put(itemBounds, it->bounds);
    return noErr;
}

int16_t tb_SetDragItemBounds(uint32_t theDrag, uint32_t theItemRef, gaddr itemBounds)
{
    drag *d = get_drag(theDrag);
    if (!d)
        return badDragRefErr;
    item *it = get_item(d, theItemRef);
    if (!it)
        return badDragItemErr;
    it->bounds = qrect_get(itemBounds);
    return noErr;
}

/* A drop in the program's own windows, or on the host somewhere other
 * than a folder, has no location (typeNull); one in a host folder has an
 * alias to the folder, the caller's to dispose of. */
int16_t tb_GetDropLocation(uint32_t theDrag, gaddr dropLocation)
{
    drag *d = get_drag(theDrag);
    if (!d)
        return badDragRefErr;
    wr32(dropLocation, typeNull);
    wr32(dropLocation + 4, 0);
    gaddr h = tb_NewPtr(4, false, true);
    if (d->drop_folder && alias_new_for_host(d->drop_folder, h) == noErr && rd32(h)) {
        wr32(dropLocation, typeAlias);
        wr32(dropLocation + 4, rd32(h));
    }
    tb_DisposePtr(h);
    return noErr;
}

int16_t tb_SetDropLocation(uint32_t theDrag, gaddr dropLocation)
{
    return get_drag(theDrag) ? noErr : badDragRefErr;
}

int16_t tb_GetDragAttributes(uint32_t theDrag, gaddr flags)
{
    drag *d = get_drag(theDrag);
    if (!d)
        return badDragRefErr;
    wr32(flags, d->attributes);
    return noErr;
}

int16_t tb_GetDragMouse(uint32_t theDrag, gaddr mouse, gaddr globalPinnedMouse)
{
    drag *d = get_drag(theDrag);
    if (!d)
        return badDragRefErr;
    if (mouse)
        wr32(mouse, d->mouse);
    if (globalPinnedMouse)
        wr32(globalPinnedMouse, d->pinned);
    return noErr;
}

int16_t tb_SetDragMouse(uint32_t theDrag, uint32_t globalPinnedMouse)
{
    drag *d = get_drag(theDrag);
    if (!d)
        return badDragRefErr;
    d->pinned = globalPinnedMouse;
    return noErr;
}

int16_t tb_GetDragOrigin(uint32_t theDrag, gaddr initialMouse)
{
    drag *d = get_drag(theDrag);
    if (!d)
        return badDragRefErr;
    wr32(initialMouse, d->origin);
    return noErr;
}

int16_t tb_GetDragModifiers(uint32_t theDrag, gaddr modifiers, gaddr mouseDownModifiers, gaddr mouseUpModifiers)
{
    drag *d = get_drag(theDrag);
    if (!d)
        return badDragRefErr;
    if (modifiers)
        wr16(modifiers, d->mods);
    if (mouseDownModifiers)
        wr16(mouseDownModifiers, d->down_mods);
    if (mouseUpModifiers)
        wr16(mouseUpModifiers, d->up_mods);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Highlighting */

static void draw_hilite(drag *d)
{
    static const uint8_t black[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    qd_fill_region(d->hilite_port, &d->hilite_rgn, black, patXor);
}

/* A two-pixel frame just inside or outside the region, in the current port. */
int16_t tb_ShowDragHilite(uint32_t theDrag, gaddr hiliteFrame, int8_t inside)
{
    drag *d = get_drag(theDrag);
    if (!d)
        return badDragRefErr;
    if (d->hilite)
        return noErr;
    hrgn outer, inner;
    hrgn_init(&outer);
    hrgn_init(&inner);
    rgn_read(hiliteFrame, &outer);
    if (!inside)
        hrgn_inset(&outer, -2, -2);
    hrgn_copy(&inner, &outer);
    hrgn_inset(&inner, 2, 2);
    hrgn_init(&d->hilite_rgn);
    hrgn_op(&d->hilite_rgn, &outer, &inner, RGN_DIFF);
    hrgn_free(&outer);
    hrgn_free(&inner);
    d->hilite_port = qd_port();
    d->hilite = true;
    draw_hilite(d);
    return noErr;
}

int16_t tb_HideDragHilite(uint32_t theDrag)
{
    drag *d = get_drag(theDrag);
    if (!d)
        return badDragRefErr;
    if (d->hilite) {
        draw_hilite(d);
        hrgn_free(&d->hilite_rgn);
        d->hilite = false;
    }
    return noErr;
}

/* Scrolling under the highlight: off before, back on after. */
static bool hidden_for_scroll;

int16_t tb_DragPreScroll(uint32_t theDrag, int16_t dH, int16_t dV)
{
    drag *d = get_drag(theDrag);
    if (!d)
        return badDragRefErr;
    hidden_for_scroll = d->hilite;
    if (d->hilite)
        draw_hilite(d);
    return noErr;
}

int16_t tb_DragPostScroll(uint32_t theDrag)
{
    drag *d = get_drag(theDrag);
    if (!d)
        return badDragRefErr;
    if (d->hilite && hidden_for_scroll)
        draw_hilite(d);
    hidden_for_scroll = false;
    return noErr;
}

int16_t tb_UpdateDragHilite(uint32_t theDrag, gaddr updateRgn)
{
    return get_drag(theDrag) ? noErr : badDragRefErr;
}

int16_t tb_GetDragHiliteColor(gaddr window, gaddr color)
{
    gmemcpy(color, LM_HiliteRGB, 6);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Tracking */

int8_t tb_WaitMouseMoved(uint32_t initialMouse)
{
    while (ev_still_down()) {
        uint32_t m = ev_mouse_global();
        if (abs(pt_h(m) - pt_h(initialMouse)) > 2 || abs(pt_v(m) - pt_v(initialMouse)) > 2)
            return 1;
        ev_wait_tick();
    }
    return 0;
}

static int16_t drop(uint32_t ref, gaddr w);
static bool out_offer(uint32_t ref);
static int16_t out_follow(uint32_t ref, gaddr sender);
static void out_set_tracking(bool on);

int16_t tb_TrackDrag(uint32_t theDrag, gaddr theEvent, gaddr theRegion)
{
    drag *d = get_drag(theDrag);
    if (!d)
        return badDragRefErr;
    d->origin = d->mouse = d->pinned = rd32(theEvent + 10); /* EventRecord: where 10, modifiers 14 */
    d->mods = d->down_mods = rd16(theEvent + 14);
    gaddr sender = wm_window_at(d->origin);
    d->attributes = kDragInsideSenderApplication | (sender ? kDragInsideSenderWindow : 0);

    /* The outline, as DragGrayRgn draws it: XOR, one pixel wide. */
    hrgn shape, moved, inner;
    hrgn_init(&shape);
    hrgn_init(&moved);
    hrgn_init(&inner);
    if (theRegion)
        rgn_read(theRegion, &shape);
    hrgn_copy(&inner, &shape);
    hrgn_inset(&inner, 1, 1);
    hrgn_op(&shape, &shape, &inner, RGN_DIFF);
    uint8_t pat[8];
    gget(pat, LM_DragPattern, 8);
    gaddr saved = qd_port();
    bool shown = false, handed = false;
    int dh = 0, dv = 0;

    for (int i = 0; i < ntracking; i++)
        tracking[i].entered = false;
    gaddr current = 0;
    bool first = true;
    out_set_tracking(true);
    do {
        uint32_t m = ev_mouse_global();
        d->mouse = d->pinned = m;
        d->mods = ev_modifiers();
        /* Out of the machine's windows: the host carries it on. */
        if (out_offer(theDrag)) {
            handed = true;
            break;
        }
        int ndh = pt_h(m) - pt_h(d->origin), ndv = pt_v(m) - pt_v(d->origin);
        bool move = first || ndh != dh || ndv != dv;
        gaddr w = wm_window_at(m);
        if (w != sender) {
            d->attributes |= kDragHasLeftSenderWindow;
            d->attributes &= ~(uint32_t)kDragInsideSenderWindow;
        } else
            d->attributes |= kDragInsideSenderWindow;
        if (move && shown) /* off while the handlers draw */
            qd_fill_region(wmgr_port(), &moved, pat, patXor);
        if (move)
            shown = false;
        if (w != current || first) {
            change_window(current, w, theDrag);
            current = w;
        } else if (w)
            tell(kDragTrackingInWindow, w, theDrag);
        tb_SetPort(wmgr_port());
        if (!shown) {
            hrgn_copy(&moved, &shape);
            hrgn_offset(&moved, ndh, ndv);
            qd_fill_region(wmgr_port(), &moved, pat, patXor);
            shown = true;
            dh = ndh;
            dv = ndv;
        }
        tb_SetPort(saved);
        first = false;
        ev_wait_tick();
    } while (ev_still_down());
    out_set_tracking(false);
    if (shown)
        qd_fill_region(wmgr_port(), &moved, pat, patXor);
    tb_SetPort(saved);
    hrgn_free(&shape);
    hrgn_free(&moved);
    hrgn_free(&inner);

    int16_t result;
    if (handed) {
        change_window(current, 0, theDrag);
        result = out_follow(theDrag, sender);
    } else {
        d->up_mods = ev_modifiers();
        d->mouse = d->pinned = ev_mouse_global();
        result = drop(theDrag, current);
    }
    tb_SetPort(saved);
    return result;
}

/* The drag ends over window w (or none): its receive handlers are offered
 * it, and the tracking handlers told it has left. */
static int16_t drop(uint32_t ref, gaddr w)
{
    int16_t result = dragNotAcceptedErr;
    if (w) {
        tell(kDragTrackingLeaveWindow, w, ref);
        for (int i = 0; i < nreceiving && result != noErr; i++)
            if (serves(&receiving[i], w)) {
                handler h = receiving[i];
                result = call_receive(&h, w, ref) ? dragNotAcceptedErr : noErr;
            }
    }
    for (int i = 0; i < ntracking; i++)
        if (tracking[i].entered) {
            tracking[i].entered = false;
            handler h = tracking[i];
            call_tracking(&h, kDragTrackingLeaveHandler, w, ref);
        }
    return result;
}

/* ---------------------------------------------------------------------- */
/* Drags from the host: files and text from the Finder and other programs.
 * The host's calls are queued; at the program's next event call they are
 * tracked through its handlers as its own drags are, and a drop goes to
 * the receive handlers of the window under it.  Files a window takes no
 * drags for open as an Open Documents event, as dropping them on the
 * program's icon did. */

enum { MAX_HOST_MSGS = 64 };
static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int kind, h, v; } host_msgs[MAX_HOST_MSGS];
static int nhost_msgs;
static char **pending_files, *pending_text; /* the next drag's items */
static int npending_files;
static char **host_files, *host_text;        /* the drag being tracked */
static int nhost_files;
static void free_items(char ***files, int *n, char **text)
{
    for (int i = 0; i < *n; i++)
        free((*files)[i]);
    free(*files);
    free(*text);
    *files = NULL;
    *n = 0;
    *text = NULL;
}

void machine_host_drag(int kind, int x, int y, const char *const *files, int nfiles, const char *text)
{
    pthread_mutex_lock(&host_lock);
    if (kind == MACHINE_DRAG_ENTER) {
        free_items(&pending_files, &npending_files, &pending_text);
        pending_files = nfiles ? calloc((size_t)nfiles, sizeof *pending_files) : NULL;
        for (int i = 0; i < nfiles; i++)
            pending_files[i] = strdup(files[i]);
        npending_files = nfiles;
        pending_text = text ? strdup(text) : NULL;
    }
    if (nhost_msgs < MAX_HOST_MSGS) {
        host_msgs[nhost_msgs].kind = kind;
        host_msgs[nhost_msgs].h = x;
        host_msgs[nhost_msgs++].v = y;
    }
    pthread_mutex_unlock(&host_lock);
}

static void add_flavor(drag *d, uint32_t item_ref, uint32_t type, const uint8_t *data, int32_t size)
{
    item *it = get_item(d, item_ref);
    if (!it) {
        d->items = realloc(d->items, sizeof *d->items * (size_t)(d->nitems + 1));
        it = &d->items[d->nitems++];
        memset(it, 0, sizeof *it);
        it->ref = item_ref;
    }
    it->flavors = realloc(it->flavors, sizeof *it->flavors * (size_t)(it->nflavors + 1));
    flavor *f = &it->flavors[it->nflavors++];
    memset(f, 0, sizeof *f);
    f->type = type;
    f->size = size;
    f->data = malloc(size ? (size_t)size : 1);
    memcpy(f->data, data, (size_t)size);
}

/* A drag of the host's items: an HFSFlavor ('hfs ': type, creator, Finder
 * flags, FSSpec) for each file, and the text as 'TEXT'. */
static uint32_t host_drag_new(void)
{
    gaddr refp = tb_NewPtr(4, false, true);
    int16_t err = tb_NewDrag(refp);
    uint32_t ref = rd32(refp);
    tb_DisposePtr(refp);
    drag *d = err ? NULL : get_drag(ref);
    if (!d)
        return 0;
    gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true), info = tb_NewPtr(16, false, true);
    for (int i = 0; i < nhost_files; i++) {
        uint8_t hfs[10 + FSSpec_SIZEOF];
        memset(hfs, 0, sizeof hfs);
        fm_spec_for_path(host_files[i], spec);
        if (tb_FSpGetFInfo(spec, info) == noErr) {
            gget(hfs, info, 8);             /* fileType, fileCreator */
            gget(hfs + 8, info + 8, 2);     /* fdFlags */
        }
        gget(hfs + 10, spec, FSSpec_SIZEOF);
        add_flavor(d, (uint32_t)i + 1, fourcc("hfs "), hfs, (int32_t)sizeof hfs);
    }
    tb_DisposePtr(spec);
    tb_DisposePtr(info);
    if (host_text) {
        size_t n = strlen(host_text);
        uint8_t *mac = malloc(n + 1);
        size_t m = utf8_to_macroman(host_text, mac, n + 1);
        for (size_t k = 0; k < m; k++)
            if (mac[k] == '\n')
                mac[k] = '\r';
        add_flavor(d, (uint32_t)nhost_files + 1, fourcc("TEXT"), mac, (int32_t)m);
        free(mac);
    }
    return ref;
}

/* At the program's event calls: the host's drag messages, in order. */
void drag_host_pump(void)
{
    for (;;) {
        pthread_mutex_lock(&host_lock);
        if (!nhost_msgs) {
            pthread_mutex_unlock(&host_lock);
            return;
        }
        int kind = host_msgs[0].kind;
        uint32_t pt = make_pt((int16_t)host_msgs[0].v, (int16_t)host_msgs[0].h);
        memmove(host_msgs, host_msgs + 1, sizeof host_msgs[0] * (size_t)--nhost_msgs);
        if (kind == MACHINE_DRAG_ENTER) {
            free_items(&host_files, &nhost_files, &host_text);
            host_files = pending_files;
            nhost_files = npending_files;
            host_text = pending_text;
            pending_files = NULL;
            npending_files = 0;
            pending_text = NULL;
        }
        pthread_mutex_unlock(&host_lock);

        if (kind == MACHINE_DRAG_ENTER) {
            if (host_ref)
                tb_DisposeDrag(host_ref);
            host_ref = host_drag_new();
            host_window = 0;
            for (int i = 0; i < ntracking; i++)
                tracking[i].entered = false;
        }
        drag *d = get_drag(host_ref);
        if (!d)
            continue;
        d->mouse = d->pinned = pt;
        d->attributes = 0; /* from another application */
        gaddr saved = qd_port();
        gaddr w = kind == MACHINE_DRAG_LEAVE ? 0 : wm_window_at(pt);
        if (kind == MACHINE_DRAG_DROP) {
            if (w != host_window)
                change_window(host_window, w, host_ref);
            bool taken = drop(host_ref, w) == noErr;
            if (!taken && nhost_files)
                machine_open_documents((const char *const *)host_files, nhost_files, false);
        } else if (w != host_window) {
            change_window(host_window, w, host_ref);
        } else if (w) {
            tell(kDragTrackingInWindow, w, host_ref);
        }
        host_window = w;
        tb_SetPort(saved);
        if (kind == MACHINE_DRAG_DROP || kind == MACHINE_DRAG_LEAVE) {
            tb_DisposeDrag(host_ref);
            host_ref = 0;
            host_window = 0;
            free_items(&host_files, &nhost_files, &host_text);
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Drags out to the host.  TrackDrag offers its drag to the host once the
 * host says the pointer has left the machine's windows; from then on the
 * host's own drag session carries it, and TrackDrag waits for the host's
 * word: data a receiver asks for (fetched here, on the emulator thread,
 * so the sender's send procedure can run), the pointer back over the
 * machine's windows (tracked through the handlers as before), and the end.
 * The flavors other programs understand go converted: text to UTF-8 (and
 * with its styles to RTF), files to their host paths, pictures to pixels,
 * and promised files ('phfs') written by the sender where the host says. */

enum { userCanceledErr = -128 };
enum { OUT_OVER, OUT_END };

void ev_wake(void);

static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t out_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t out_request_lock = PTHREAD_MUTEX_INITIALIZER; /* one request at a time */
static machine_drag_out_fn out_fn;
static void *out_ctx;
static bool out_tracking, out_outside, out_refused;
static uint32_t out_ref;    /* the drag the host has, or 0 */
typedef struct {
    int kind, how, h, v;
    bool inside;
    char *folder;
} out_msg;
static out_msg out_msgs[MAX_HOST_MSGS];
static int nout_msgs;
static struct {
    bool pending, taken, done, ok;
    int item, kind;
    char *folder;
    machine_drag_data result;
} out_req;

void machine_set_drag_out_handler(machine_drag_out_fn fn, void *ctx)
{
    out_fn = fn;
    out_ctx = ctx;
}

bool machine_drag_out_tracking(void)
{
    pthread_mutex_lock(&out_lock);
    bool on = out_tracking;
    pthread_mutex_unlock(&out_lock);
    return on;
}

void machine_drag_out_pointer(bool outside)
{
    pthread_mutex_lock(&out_lock);
    out_outside = outside;
    pthread_mutex_unlock(&out_lock);
}

static void drag_out_reset(void)
{
    pthread_mutex_lock(&out_lock);
    out_tracking = out_outside = out_refused = false;
    out_ref = 0;
    for (int i = 0; i < nout_msgs; i++)
        free(out_msgs[i].folder);
    nout_msgs = 0;
    pthread_mutex_unlock(&out_lock);
}

static void out_set_tracking(bool on)
{
    pthread_mutex_lock(&out_lock);
    out_tracking = on;
    if (on)
        out_refused = false;
    else
        out_outside = false;
    pthread_mutex_unlock(&out_lock);
}

static void out_post(int kind, int how, bool inside, int x, int y, const char *folder)
{
    pthread_mutex_lock(&out_lock);
    if (out_ref && nout_msgs < MAX_HOST_MSGS)
        out_msgs[nout_msgs++] = (out_msg){ kind, how, x, y, inside, folder ? strdup(folder) : NULL };
    pthread_mutex_unlock(&out_lock);
    ev_wake();
}

void machine_drag_out_over(bool inside, int x, int y) { out_post(OUT_OVER, 0, inside, x, y, NULL); }

void machine_drag_out_end(int how, int x, int y, const char *folder) { out_post(OUT_END, how, false, x, y, folder); }

/* A growing buffer of text. */
typedef struct {
    char *p;
    size_t n, cap;
} sbuf;

static void sb_put(sbuf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void sb_printf(sbuf *b, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    sb_put(b, tmp, n < 0 ? 0 : (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

/* MacRoman text in RTF (\mac): returns as paragraph breaks. */
static void sb_rtf_text(sbuf *b, const uint8_t *t, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = t[i];
        if (c == '\\' || c == '{' || c == '}')
            sb_printf(b, "\\%c", c);
        else if (c == '\r' || c == '\n')
            sb_put(b, "\\par\n", 5);
        else if (c == '\t')
            sb_put(b, "\\tab ", 5);
        else if (c >= 0x80 || c < 0x20)
            sb_printf(b, "\\'%02x", c);
        else
            sb_put(b, (const char *)&c, 1);
    }
}

static uint16_t be16_at(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32_at(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* 'TEXT' with its 'styl' (a StScrpRec: a count, then for each run its
 * start, height, ascent, font, face, size and colour) as RTF. */
static char *text_rtf(const uint8_t *text, size_t n, const uint8_t *styl, size_t sn, size_t *len)
{
    enum { RUN = 20, MAX_FONTS = 32, MAX_COLORS = 32 };
    int nruns = sn >= 2 ? (int16_t)be16_at(styl) : 0;
    if (nruns < 0 || 2 + (size_t)nruns * RUN > sn)
        nruns = 0;
    int16_t fonts[MAX_FONTS];
    uint16_t colors[MAX_COLORS][3];
    int nfonts = 0, ncolors = 0;
    for (int i = 0; i < nruns; i++) {
        const uint8_t *r = styl + 2 + i * RUN;
        int16_t font = (int16_t)be16_at(r + 8);
        int k = 0;
        while (k < nfonts && fonts[k] != font)
            k++;
        if (k == nfonts && nfonts < MAX_FONTS)
            fonts[nfonts++] = font;
        uint16_t c[3] = { be16_at(r + 14), be16_at(r + 16), be16_at(r + 18) };
        for (k = 0; k < ncolors && memcmp(colors[k], c, sizeof c); k++)
            ;
        if (k == ncolors && ncolors < MAX_COLORS)
            memcpy(colors[ncolors++], c, sizeof c);
    }
    sbuf b = { 0 };
    sb_printf(&b, "{\\rtf1\\mac\\ansicpg10000\\deff0{\\fonttbl");
    gaddr name = tb_NewPtr(256, false, true);
    for (int k = 0; k < nfonts; k++) {
        wr8(name, 0);
        tb_GetFontName(fonts[k], name);
        uint8_t pname[256];
        gget(pname, name, 256);
        sb_printf(&b, "{\\f%d\\fnil ", k);
        if (pname[0])
            sb_rtf_text(&b, pname + 1, pname[0]);
        else
            sb_printf(&b, "Geneva");
        sb_put(&b, ";}", 2);
    }
    tb_DisposePtr(name);
    if (!nfonts)
        sb_printf(&b, "{\\f0\\fnil Geneva;}");
    sb_printf(&b, "}{\\colortbl;");
    for (int k = 0; k < ncolors; k++)
        sb_printf(&b, "\\red%d\\green%d\\blue%d;", colors[k][0] >> 8, colors[k][1] >> 8, colors[k][2] >> 8);
    sb_printf(&b, "}\n");
    if (!nruns) {
        sb_printf(&b, "\\f0\\fs24 ");
        sb_rtf_text(&b, text, n);
    }
    for (int i = 0; i < nruns; i++) {
        const uint8_t *r = styl + 2 + i * RUN;
        size_t start = be32_at(r), end = i + 1 < nruns ? be32_at(r + RUN) : n;
        if (i == 0)
            start = 0;
        if (end > n)
            end = n;
        if (start >= end)
            continue;
        int16_t font = (int16_t)be16_at(r + 8);
        uint8_t face = r[10];
        int size = (int16_t)be16_at(r + 12);
        int f = 0, c = 0;
        while (f < nfonts - 1 && fonts[f] != font)
            f++;
        uint16_t rgb[3] = { be16_at(r + 14), be16_at(r + 16), be16_at(r + 18) };
        while (c < ncolors - 1 && memcmp(colors[c], rgb, sizeof rgb))
            c++;
        sb_printf(&b, "{\\f%d\\fs%d\\cf%d", f, (size > 0 ? size : 12) * 2, c + 1);
        if (face & 1)
            sb_printf(&b, "\\b");
        if (face & 2)
            sb_printf(&b, "\\i");
        if (face & 4)
            sb_printf(&b, "\\ul");
        if (face & 8)
            sb_printf(&b, "\\outl");
        if (face & 16)
            sb_printf(&b, "\\shad");
        sb_put(&b, " ", 1);
        sb_rtf_text(&b, text + start, end - start);
        sb_put(&b, "}", 1);
    }
    sb_put(&b, "}", 1);
    *len = b.n;
    return b.p;
}

/* MacRoman text as UTF-8, with the host's line ends. */
static char *text_utf8(const uint8_t *text, size_t n, size_t *len)
{
    char *out = malloc(n * 3 + 1);
    size_t m = macroman_to_utf8(text, n, out, n * 3 + 1);
    for (size_t i = 0; i < m; i++)
        if (out[i] == '\r')
            out[i] = '\n';
    *len = m;
    return out;
}

/* The host path of an HFSFlavor's (or a bare FSSpec's) file. */
static char *spec_path(const uint8_t *spec, size_t *len)
{
    gaddr name = tb_NewPtr(64, false, true);
    gput(name, spec + 6, 64);
    char path[4096];
    bool ok = fm_host_path((int16_t)be16_at(spec), (int32_t)be32_at(spec + 2), name, true, path, sizeof path);
    tb_DisposePtr(name);
    if (!ok)
        return NULL;
    *len = strlen(path);
    return strdup(path);
}

static flavor *find(drag *d, int i, uint32_t type)
{
    return i >= 0 && i < d->nitems ? get_flavor(&d->items[i], type) : NULL;
}

/* A flavor's data from the sender (promised data fetched); NULL if none. */
static flavor *fetch_at(uint32_t ref, int i, uint32_t type)
{
    drag *d = get_drag(ref);
    if (!d || i < 0 || i >= d->nitems)
        return NULL;
    int16_t err;
    return fetch(ref, d->items[i].ref, type, &err);
}

/* One representation of item i, made now (asking the sender if need be). */
static bool convert(uint32_t ref, int i, int kind, const char *folder, machine_drag_data *out)
{
    memset(out, 0, sizeof *out);
    flavor *f;
    switch (kind) {
    case MACHINE_FLAVOR_TEXT:
        if (!(f = fetch_at(ref, i, fourcc("TEXT"))))
            return false;
        out->data = (uint8_t *)text_utf8(f->data, (size_t)f->size, &out->size);
        return true;
    case MACHINE_FLAVOR_RTF: {
        if (!fetch_at(ref, i, fourcc("styl")) || !fetch_at(ref, i, fourcc("TEXT")))
            return false;
        drag *d = get_drag(ref); /* both are in now */
        flavor *t = find(d, i, fourcc("TEXT")), *s = find(d, i, fourcc("styl"));
        out->data = (uint8_t *)text_rtf(t->data, (size_t)t->size, s->data, (size_t)s->size, &out->size);
        return true;
    }
    case MACHINE_FLAVOR_FILE:
        if (!(f = fetch_at(ref, i, fourcc("hfs "))) || f->size < (int32_t)(10 + FSSpec_SIZEOF))
            return false;
        return (out->data = (uint8_t *)spec_path(f->data + 10, &out->size)) != NULL;
    case MACHINE_FLAVOR_IMAGE:
        if (!(f = fetch_at(ref, i, fourcc("PICT"))))
            return false;
        return machine_picture_rgba(f->data, (size_t)f->size, &out->data, &out->width, &out->height);
    case MACHINE_FLAVOR_PROMISED_FILE: {
        /* The sender writes the file in the drop location's folder and
         * says where in the promised flavor (an FSSpec, or an HFSFlavor). */
        drag *d = get_drag(ref);
        if (!d || !folder || !(f = find(d, i, fourcc("phfs"))) || f->size < 14)
            return false;
        uint32_t promised = be32_at(f->data + 10);
        free(d->drop_folder);
        d->drop_folder = strdup(folder);
        if (!(f = fetch_at(ref, i, promised)))
            return false;
        if (f->size >= (int32_t)(10 + FSSpec_SIZEOF) && promised == fourcc("hfs "))
            return (out->data = (uint8_t *)spec_path(f->data + 10, &out->size)) != NULL;
        if (f->size >= (int32_t)FSSpec_SIZEOF)
            return (out->data = (uint8_t *)spec_path(f->data, &out->size)) != NULL;
        return false;
    }
    }
    return false;
}

/* The drag as the host sees it.  Cheap representations of data the drag
 * already holds come with it; the rest on request. */
typedef struct {
    machine_drag_export x;
    machine_drag_item *items;
    machine_drag_rep *reps;
    machine_drag_data *data;
    int nreps;
} out_export;

static void export_add(out_export *e, int item, int kind, uint32_t type, bool now, uint32_t ft, uint32_t fc,
                       uint32_t ref)
{
    machine_drag_rep *r = &e->reps[e->nreps];
    machine_drag_data *dd = &e->data[e->nreps];
    memset(r, 0, sizeof *r);
    memset(dd, 0, sizeof *dd);
    r->kind = kind;
    r->type = type;
    r->file_type = ft;
    r->file_creator = fc;
    if (now && convert(ref, item, kind, NULL, dd)) {
        r->data = dd->data;
        r->size = dd->size;
    }
    e->items[item].nreps++;
    e->nreps++;
}

static void export_build(out_export *e, uint32_t ref)
{
    drag *d = get_drag(ref);
    memset(e, 0, sizeof *e);
    int max = 0;
    for (int i = 0; i < d->nitems; i++)
        max += 5;
    e->items = calloc((size_t)d->nitems + 1, sizeof *e->items);
    e->reps = calloc((size_t)max + 1, sizeof *e->reps);
    e->data = calloc((size_t)max + 1, sizeof *e->data);
    for (int i = 0; i < d->nitems; i++) {
        e->items[i].reps = e->reps + e->nreps;
        d = get_drag(ref);
        flavor *text = find(d, i, fourcc("TEXT")), *styl = find(d, i, fourcc("styl"));
        flavor *hfs = find(d, i, fourcc("hfs ")), *pict = find(d, i, fourcc("PICT"));
        flavor *phfs = find(d, i, fourcc("phfs"));
        bool text_now = text && !text->promised, styl_now = styl && !styl->promised;
        bool hfs_now = hfs && !hfs->promised;
        if (phfs && !phfs->promised && phfs->size >= 14) {
            uint8_t info[14];
            memcpy(info, phfs->data, sizeof info);
            export_add(e, i, MACHINE_FLAVOR_PROMISED_FILE, fourcc("phfs"), false, be32_at(info), be32_at(info + 4),
                       ref);
        } else if (hfs)
            export_add(e, i, MACHINE_FLAVOR_FILE, fourcc("hfs "), hfs_now, 0, 0, ref);
        if (text && styl)
            export_add(e, i, MACHINE_FLAVOR_RTF, fourcc("styl"), text_now && styl_now, 0, 0, ref);
        if (text)
            export_add(e, i, MACHINE_FLAVOR_TEXT, fourcc("TEXT"), text_now, 0, 0, ref);
        if (pict)
            export_add(e, i, MACHINE_FLAVOR_IMAGE, fourcc("PICT"), false, 0, 0, ref);
    }
    e->x.nitems = d->nitems;
    e->x.items = e->items;
    e->x.h = pt_h(d->mouse);
    e->x.v = pt_v(d->mouse);
}

static void export_free(out_export *e)
{
    for (int k = 0; k < e->nreps; k++)
        free(e->data[k].data);
    free(e->items);
    free(e->reps);
    free(e->data);
}

/* From TrackDrag's loop: the pointer has left the machine's windows, so
 * the host is offered the drag (once; it may refuse).  True if it took it. */
static bool out_offer(uint32_t ref)
{
    pthread_mutex_lock(&out_lock);
    bool offer = out_fn && out_outside && !out_refused;
    pthread_mutex_unlock(&out_lock);
    if (!offer)
        return false;
    out_export e;
    export_build(&e, ref);
    pthread_mutex_lock(&out_lock);
    out_ref = ref; /* requests may come as soon as the host has it */
    out_tracking = false;
    pthread_mutex_unlock(&out_lock);
    bool taken = out_fn(&e.x, out_ctx);
    export_free(&e);
    if (!taken) {
        pthread_mutex_lock(&out_lock);
        out_ref = 0;
        out_refused = true;
        out_tracking = true;
        pthread_mutex_unlock(&out_lock);
    }
    return taken;
}

/* A request from the host waiting: served here, on the emulator thread. */
static void out_serve(uint32_t ref)
{
    pthread_mutex_lock(&out_lock);
    if (!out_req.pending || out_req.taken) {
        pthread_mutex_unlock(&out_lock);
        return;
    }
    out_req.taken = true;
    int item = out_req.item, kind = out_req.kind;
    char *folder = out_req.folder ? strdup(out_req.folder) : NULL;
    pthread_mutex_unlock(&out_lock);
    machine_drag_data result;
    gaddr saved = qd_port();
    bool ok = convert(ref, item, kind, folder, &result);
    tb_SetPort(saved);
    free(folder);
    pthread_mutex_lock(&out_lock);
    out_req.result = result;
    out_req.ok = ok;
    out_req.done = true;
    pthread_cond_broadcast(&out_cond);
    pthread_mutex_unlock(&out_lock);
}

bool machine_drag_out_data(int item, int kind, const char *folder, machine_drag_data *out)
{
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&out_request_lock);
    pthread_mutex_lock(&out_lock);
    bool ok = false;
    if (out_ref) {
        memset(&out_req, 0, sizeof out_req);
        out_req.pending = true;
        out_req.item = item;
        out_req.kind = kind;
        out_req.folder = folder ? strdup(folder) : NULL;
        pthread_mutex_unlock(&out_lock);
        ev_wake();
        pthread_mutex_lock(&out_lock);
        /* Five seconds for the emulator to take it; once taken, as long
         * as the sender needs. */
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 5;
        while (!out_req.done && out_ref) {
            struct timespec now, ts;
            clock_gettime(CLOCK_REALTIME, &now);
            if (!out_req.taken && now.tv_sec >= deadline.tv_sec)
                break;
            ts = now;
            ts.tv_nsec += 50000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&out_cond, &out_lock, &ts);
        }
        if (out_req.done) {
            ok = out_req.ok;
            *out = out_req.result;
        }
        free(out_req.folder);
        memset(&out_req, 0, sizeof out_req);
    }
    pthread_mutex_unlock(&out_lock);
    pthread_mutex_unlock(&out_request_lock);
    return ok;
}

/* The host has the drag: follow it until it ends, and give TrackDrag's
 * result. */
static int16_t out_follow(uint32_t ref, gaddr sender)
{
    gaddr over = 0, saved = qd_port();
    int16_t result = dragNotAcceptedErr;
    drag *d = get_drag(ref);
    d->attributes = kDragHasLeftSenderWindow;
    for (;;) {
        out_serve(ref);
        pthread_mutex_lock(&out_lock);
        int n = nout_msgs;
        out_msg msg = n ? out_msgs[0] : (out_msg){ 0 };
        if (n)
            memmove(out_msgs, out_msgs + 1, sizeof out_msgs[0] * (size_t)--nout_msgs);
        pthread_mutex_unlock(&out_lock);
        if (!n) {
            if (g_cpu && g_cpu->halted)
                break;
            ev_wait_tick();
            continue;
        }
        d = get_drag(ref);
        if (!d) {
            free(msg.folder);
            break;
        }
        uint32_t pt = make_pt((int16_t)msg.v, (int16_t)msg.h);
        gaddr w = msg.kind == OUT_OVER && msg.inside ? wm_window_at(pt) : 0;
        if (msg.kind == OUT_END && msg.how == MACHINE_DRAG_OUT_INSIDE)
            w = wm_window_at(pt);
        if (msg.kind == OUT_OVER || msg.how == MACHINE_DRAG_OUT_INSIDE) {
            d->mouse = d->pinned = pt;
            d->mods = ev_modifiers();
            d->attributes = kDragHasLeftSenderWindow | (msg.inside || w ? kDragInsideSenderApplication : 0) |
                            (w && w == sender ? kDragInsideSenderWindow : 0);
            if (w != over)
                change_window(over, w, ref);
            else if (w)
                tell(kDragTrackingInWindow, w, ref);
            over = w;
            tb_SetPort(saved);
        }
        if (msg.kind != OUT_END)
            continue;
        d->up_mods = ev_modifiers();
        if (msg.how == MACHINE_DRAG_OUT_INSIDE) {
            result = drop(ref, w);
        } else {
            change_window(over, 0, ref);
            drop(ref, 0);
            if ((d = get_drag(ref)) && msg.folder) {
                free(d->drop_folder);
                d->drop_folder = msg.folder;
                msg.folder = NULL;
            }
            result = msg.how == MACHINE_DRAG_OUT_DROPPED ? noErr : userCanceledErr;
        }
        free(msg.folder);
        break;
    }
    tb_SetPort(saved);
    pthread_mutex_lock(&out_lock);
    out_ref = 0;
    for (int i = 0; i < nout_msgs; i++)
        free(out_msgs[i].folder);
    nout_msgs = 0;
    pthread_cond_broadcast(&out_cond);
    pthread_mutex_unlock(&out_lock);
    return result;
}

/* ---------------------------------------------------------------------- */
/* The zoom animations: an outline in gray, XORed on the screen through the
 * Window Manager's port, moves step by step from where the drag began to
 * where it ends (a rejected drag snapping back, a Finder icon flying to
 * its window), each step shown for a tick and taken off again, so the
 * screen is as it was afterwards.  Headless, Delay skips the clock ahead
 * instead of waiting, so a run never waits on it. */

enum { kZoomNoAcceleration = 0, kZoomAccelerate = 1, kZoomDecelerate = 2 };
enum { MAX_ZOOM_STEPS = 25 }; /* a little under half a second at a tick a step */

/* How far along the way step i of n is: accelerating starts slowly and
 * speeds up, decelerating the reverse. */
static double zoom_at(int i, int n, int acceleration)
{
    double t = (double)i / n;
    if (acceleration == kZoomAccelerate)
        return t * t;
    if (acceleration == kZoomDecelerate)
        return 1 - (1 - t) * (1 - t);
    return t;
}

/* The one-pixel outline of a shape, as DragGrayRgn draws it. */
static void outline(hrgn *shape)
{
    hrgn inner;
    hrgn_init(&inner);
    hrgn_copy(&inner, shape);
    hrgn_inset(&inner, 1, 1);
    hrgn_op(shape, shape, &inner, RGN_DIFF);
    hrgn_free(&inner);
}

typedef void (*zoom_shape_fn)(double t, hrgn *out, const void *ctx);

static void zoom(int steps, int acceleration, zoom_shape_fn shape_at, const void *ctx)
{
    gaddr port = wmgr_port();
    if (!port || steps <= 0)
        return;
    if (steps > MAX_ZOOM_STEPS)
        steps = MAX_ZOOM_STEPS;
    hrgn shown, next;
    hrgn_init(&shown);
    hrgn_init(&next);
    bool on = false;
    for (int i = 1; i <= steps; i++) {
        shape_at(zoom_at(i, steps, acceleration), &next, ctx);
        outline(&next);
        qd_fill_region(port, &next, qd_pat_gray, patXor);
        if (on)
            qd_fill_region(port, &shown, qd_pat_gray, patXor);
        hrgn_copy(&shown, &next);
        on = true;
        uint32_t ticks;
        tb_Delay(1, &ticks);
    }
    if (on)
        qd_fill_region(port, &shown, qd_pat_gray, patXor);
    hrgn_free(&shown);
    hrgn_free(&next);
}

static void rect_at(double t, hrgn *out, const void *ctx)
{
    const qrect *q = ctx; /* from, to */
    qrect r = {
        (int16_t)(q[0].top + (q[1].top - q[0].top) * t + 0.5),
        (int16_t)(q[0].left + (q[1].left - q[0].left) * t + 0.5),
        (int16_t)(q[0].bottom + (q[1].bottom - q[0].bottom) * t + 0.5),
        (int16_t)(q[0].right + (q[1].right - q[0].right) * t + 0.5),
    };
    hrgn_rect(out, r);
}

typedef struct {
    hrgn shape;
    int dh, dv;
} region_zoom;

static void region_at(double t, hrgn *out, const void *ctx)
{
    const region_zoom *z = ctx;
    hrgn_copy(out, &z->shape);
    hrgn_offset(out, (int)(z->dh * t + (z->dh < 0 ? -0.5 : 0.5)), (int)(z->dv * t + (z->dv < 0 ? -0.5 : 0.5)));
}

/* Both rectangles are in global coordinates. */
int16_t tb_ZoomRects(gaddr fromRect, gaddr toRect, int16_t zoomSteps, int16_t acceleration)
{
    if (!fromRect || !toRect)
        return paramErr;
    qrect q[2] = { qrect_get(fromRect), qrect_get(toRect) };
    zoom(zoomSteps, acceleration, rect_at, q);
    return noErr;
}

/* The region (global coordinates) moves by zoomDistance on its way. */
int16_t tb_ZoomRegion(gaddr region, uint32_t zoomDistance, int16_t zoomSteps, int16_t acceleration)
{
    if (!region || !rd32(region))
        return paramErr;
    region_zoom z;
    hrgn_init(&z.shape);
    rgn_read(region, &z.shape);
    z.dh = pt_h(zoomDistance);
    z.dv = pt_v(zoomDistance);
    zoom(zoomSteps, acceleration, region_at, &z);
    hrgn_free(&z.shape);
    return noErr;
}

int16_t tb_SetDragImage(uint32_t theDrag, gaddr imagePixMap, gaddr imageRgn, uint32_t imageOffsetPt,
                        uint32_t theImageFlags)
{
    return get_drag(theDrag) ? noErr : badDragRefErr;
}

int16_t tb_ChangeDragBehaviors(uint32_t theDrag, uint32_t inBehaviorsToSet, uint32_t inBehaviorsToClear)
{
    return get_drag(theDrag) ? noErr : badDragRefErr;
}
