/*
 * Drag Manager: drags within the program's own windows.
 *
 * A drag holds items, each with flavors whose data is copied in (or
 * promised and fetched through the drag's send procedure when a receiver
 * asks).  TrackDrag follows the mouse with the drag region's gray outline,
 * as DragGrayRgn draws it, sends the tracking handlers of the window under
 * the mouse their enter, in and leave messages, and on release offers the
 * drag to that window's receive handlers.  Drags to and from the host's
 * windows and the Finder are not part of it.
 */
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"

enum {
    badDragRefErr = -1850, badDragItemErr = -1851, badDragFlavorErr = -1852, duplicateFlavorErr = -1853,
    cantGetFlavorErr = -1854, duplicateHandlerErr = -1855, handlerNotFoundErr = -1856,
    dragNotAcceptedErr = -1857,
};
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
} drag;

static drag drags[MAX_DRAGS];

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
    memset(d, 0, sizeof *d);
}

void drag_reset(void)
{
    for (int i = 0; i < MAX_DRAGS; i++)
        if (drags[i].used)
            free_drag(&drags[i]);
    ntracking = nreceiving = 0;
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

/* Drops land in the program's own windows: no Finder location. */
int16_t tb_GetDropLocation(uint32_t theDrag, gaddr dropLocation)
{
    if (!get_drag(theDrag))
        return badDragRefErr;
    wr32(dropLocation, 0x6E756C6C); /* typeNull */
    wr32(dropLocation + 4, 0);
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
    bool shown = false;
    int dh = 0, dv = 0;

    for (int i = 0; i < ntracking; i++)
        tracking[i].entered = false;
    gaddr current = 0;
    bool first = true;
    do {
        uint32_t m = ev_mouse_global();
        d->mouse = d->pinned = m;
        d->mods = ev_modifiers();
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
    if (shown)
        qd_fill_region(wmgr_port(), &moved, pat, patXor);
    tb_SetPort(saved);
    hrgn_free(&shape);
    hrgn_free(&moved);
    hrgn_free(&inner);

    d->up_mods = ev_modifiers();
    d->mouse = d->pinned = ev_mouse_global();
    int16_t result = dragNotAcceptedErr;
    if (current) {
        tell(kDragTrackingLeaveWindow, current, theDrag);
        for (int i = 0; i < nreceiving && result != noErr; i++)
            if (serves(&receiving[i], current)) {
                handler h = receiving[i];
                result = call_receive(&h, current, theDrag) ? dragNotAcceptedErr : noErr;
            }
    }
    for (int i = 0; i < ntracking; i++)
        if (tracking[i].entered) {
            tracking[i].entered = false;
            handler h = tracking[i];
            call_tracking(&h, kDragTrackingLeaveHandler, current, theDrag);
        }
    tb_SetPort(saved);
    return result;
}

/* The zoom animations: the drag snaps back at once. */
int16_t tb_ZoomRects(gaddr fromRect, gaddr toRect, int16_t zoomSteps, int16_t acceleration) { return noErr; }
int16_t tb_ZoomRegion(gaddr region, uint32_t zoomDistance, int16_t zoomSteps, int16_t acceleration) { return noErr; }

int16_t tb_SetDragImage(uint32_t theDrag, gaddr imagePixMap, gaddr imageRgn, uint32_t imageOffsetPt,
                        uint32_t theImageFlags)
{
    return get_drag(theDrag) ? noErr : badDragRefErr;
}

int16_t tb_ChangeDragBehaviors(uint32_t theDrag, uint32_t inBehaviorsToSet, uint32_t inBehaviorsToClear)
{
    return get_drag(theDrag) ? noErr : badDragRefErr;
}
