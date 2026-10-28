/*
 * Open Transport's utility and kernel-utility calls that clients use
 * (OTUtilityLib, OTClientUtilLib, OpenTransportUtilLib, OpenTptKernelUtilLib):
 * the interrupt and critical-section state, time stamps and the clock,
 * queues, bit maps, hash lists, gates, random numbers, port references,
 * no-copy buffers, and the client-registration and notification calls.
 *
 * Blue Box runs one thread of guest code at a time and delivers notifiers
 * and tasks from its event loop, so the interrupt-level and critical-section
 * calls keep a level count and nothing more.  The calls here that take a
 * callback (hash searches, gate procedures) run it as the program's own
 * code, PowerPC or 68k.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "machine.h"
#include "ot_internal.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

/* ---- Callbacks into the program's code ---- */

/* Whether the call being served came from 68k code (set by ot_68k.c). */
bool ot_from_68k;

uint32_t ot_callproc_as(bool m68k, gaddr proc, uint32_t procinfo, const uint32_t *args, int n)
{
    if (!proc)
        return 0;
    if (m68k || mm_is_descriptor(proc) || mm_is_68k_proc(proc))
        return mm_call(proc, procinfo, args, n);
    return ppc_call(proc, args, n);
}

uint32_t ot_callproc(gaddr proc, uint32_t procinfo, const uint32_t *args, int n)
{
    return ot_callproc_as(ot_from_68k, proc, procinfo, args, n);
}

/* ---- Interrupt level and critical sections ---- */

static int interrupt_level;
static int sync_sections;

static void u_EnterInterrupt(ppc_cpu *c)
{
    interrupt_level++;
    c->pc = c->lr;
}

static void u_LeaveInterrupt(ppc_cpu *c)
{
    if (interrupt_level > 0)
        interrupt_level--;
    c->pc = c->lr;
}

static void u_IsAtInterruptLevel(ppc_cpu *c) { RETURN(interrupt_level > 0); }

/* Libraries load, and synchronous calls run, only at system task time. */
static void u_CanLoadLibraries(ppc_cpu *c) { RETURN(interrupt_level == 0); }
static void u_CanMakeSyncCall(ppc_cpu *c) { RETURN(interrupt_level == 0 && !ot_in_notifier()); }

/* OTEnterCriticalSection() returns a token OTLeaveCriticalSection(token)
 * gives back; with one thread running there is nothing to exclude. */
static void u_EnterCriticalSection(ppc_cpu *c) { RETURN(0); }
static void u_nop(ppc_cpu *c) { c->pc = c->lr; }

static void u_EnterSynchronousSection(ppc_cpu *c)
{
    sync_sections++;
    RETURN(kOTNoError);
}

static void u_LeaveSynchronousSection(ppc_cpu *c)
{
    if (sync_sections > 0)
        sync_sections--;
    RETURN(kOTNoError);
}

void ot_util_reset(void)
{
    interrupt_level = 0;
    sync_sections = 0;
}

/* ---- Time ---- */

static uint64_t micros(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

static uint64_t stamp_at(gaddr p) { return p ? (uint64_t)rd32(p) << 32 | rd32(p + 4) : 0; }

static void put_stamp(gaddr p, uint64_t t)
{
    if (p) {
        wr32(p, (uint32_t)(t >> 32));
        wr32(p + 4, (uint32_t)t);
    }
}

/* OTSubtractTimeStamps(result, start, end): result = start - end (the
 * caller names the later stamp first, as Apple's samples do); the result. */
static void u_SubtractTimeStamps(ppc_cpu *c)
{
    put_stamp(R(0), stamp_at(R(1)) - stamp_at(R(2)));
    RETURN(R(0));
}

static void u_TimeStampInMicroseconds(ppc_cpu *c) { RETURN((uint32_t)stamp_at(R(0))); }

/* Seconds, and 60 Hz ticks, since Open Transport was started. */
static uint64_t start_micros;

static void u_GetClockTimeInSecs(ppc_cpu *c)
{
    if (!start_micros)
        start_micros = micros();
    RETURN((uint32_t)((micros() - start_micros) / 1000000u));
}

static void u_GetTimerTicks(ppc_cpu *c) { RETURN(tb_TickCount()); }

/* ---- Queues ---- */

/* OTEnqueue(void **listHead, void *object, linkOffset): object goes on the
 * head, the old head into its link. */
static void u_Enqueue(ppc_cpu *c)
{
    gaddr head = R(0), obj = R(1), off = R(2);
    if (head && obj) {
        wr32(obj + off, rd32(head));
        wr32(head, obj);
    }
    c->pc = c->lr;
}

/* OTDequeue(void **listHead, linkOffset): the head object, its link the
 * new head. */
static void u_Dequeue(ppc_cpu *c)
{
    gaddr head = R(0), off = R(1);
    gaddr obj = head ? rd32(head) : 0;
    if (obj) {
        wr32(head, rd32(obj + off));
        wr32(obj + off, 0);
    }
    RETURN(obj);
}

/* ---- Bit maps ---- */

/* Bit n is bit (n & 7) of byte n >> 3, bit 0 the low bit: the order the
 * atomic bit operations use. */
static bool bit_test(gaddr map, uint32_t n) { return (rd8(map + (n >> 3)) >> (n & 7)) & 1; }

static void u_SetBit(ppc_cpu *c)
{
    bool was = false;
    if (R(0)) {
        was = bit_test(R(0), R(1));
        wr8(R(0) + (R(1) >> 3), (uint8_t)(rd8(R(0) + (R(1) >> 3)) | 1u << (R(1) & 7)));
    }
    RETURN(was);
}

static void u_ClearBit(ppc_cpu *c)
{
    bool was = false;
    if (R(0)) {
        was = bit_test(R(0), R(1));
        wr8(R(0) + (R(1) >> 3), (uint8_t)(rd8(R(0) + (R(1) >> 3)) & ~(1u << (R(1) & 7))));
    }
    RETURN(was);
}

static void u_TestBit(ppc_cpu *c) { RETURN(R(0) && bit_test(R(0), R(1))); }

/* OTSetFirstClearBit(map, startBit, numBits): the number of the first clear
 * bit from startBit within numBits, now set; kOTNotFoundErr if there is none. */
static void u_SetFirstClearBit(ppc_cpu *c)
{
    gaddr map = R(0);
    uint32_t start = R(1), num = R(2);
    for (uint32_t i = 0; map && i < num; i++) {
        uint32_t n = start + i;
        if (!bit_test(map, n)) {
            wr8(map + (n >> 3), (uint8_t)(rd8(map + (n >> 3)) | 1u << (n & 7)));
            RETURN(n);
            return;
        }
    }
    RETURN((uint32_t)kOTNotFoundErr);
}

/* ---- Hash lists ---- */

/* OTHashList: fHashProc, fHashTableSize, fHashBuckets; the buckets follow
 * it in the memory OTInitHashList was given.  The table size is a power of
 * two, so a hash value picks a bucket by its low bits.  An OTLink is one
 * pointer, the next. */
enum { kHashListSize = 12 };

static uint32_t hash_size_for(uint32_t entries)
{
    uint32_t n = 1;
    while (n < entries && n < 0x40000000u)
        n <<= 1;
    return n;
}

static void u_CalculateHashListMemoryNeeds(ppc_cpu *c) { RETURN(kHashListSize + 4 * hash_size_for(R(0))); }

static void u_InitHashList(ppc_cpu *c)
{
    gaddr mem = R(0);
    uint32_t bytes = R(1);
    if (!mem || bytes < kHashListSize + 4) {
        RETURN((uint32_t)kOTBadReferenceErr);
        return;
    }
    uint32_t room = (bytes - kHashListSize) / 4, size = 1;
    while (size * 2 <= room)
        size *= 2;
    wr32(mem, R(2));
    wr32(mem + 4, size);
    wr32(mem + 8, mem + kHashListSize);
    gmemset(mem + kHashListSize, 0, 4 * size);
    RETURN(kOTNoError);
}

static gaddr hash_bucket(gaddr list, uint32_t hash)
{
    return rd32(list + 8) + 4 * (hash & (rd32(list + 4) - 1));
}

static uint32_t hash_of(gaddr list, gaddr link)
{
    uint32_t args[1] = { link };
    return ot_callproc(rd32(list), 0x000000F1 /* C, long result, one long */, args, 1);
}

static void u_AddToHashList(ppc_cpu *c)
{
    gaddr list = R(0), link = R(1);
    if (list && link && rd32(list + 4)) {
        gaddr b = hash_bucket(list, hash_of(list, link));
        wr32(link, rd32(b));
        wr32(b, link);
    }
    c->pc = c->lr;
}

static void u_RemoveLinkFromHashList(ppc_cpu *c)
{
    gaddr list = R(0), link = R(1);
    bool ok = false;
    if (list && link && rd32(list + 4)) {
        gaddr b = hash_bucket(list, hash_of(list, link));
        for (gaddr at = b; rd32(at) && !ok; at = rd32(at))
            if (rd32(at) == link) {
                wr32(at, rd32(link));
                wr32(link, 0);
                ok = true;
            }
    }
    RETURN(ok);
}

static void u_IsInHashList(ppc_cpu *c)
{
    gaddr list = R(0), link = R(1);
    bool ok = false;
    if (list && link && rd32(list + 4)) {
        gaddr b = hash_bucket(list, hash_of(list, link));
        for (gaddr l = rd32(b); l && !ok; l = rd32(l))
            ok = l == link;
    }
    RETURN(ok);
}

/* OTFindInHashList(list, searchProc, ref, hashValue) and OTRemoveFromHashList
 * (the same, taking the link out): the first link searchProc(ref, link) says
 * yes to in the hash value's bucket. */
static void hash_find(ppc_cpu *c, bool remove)
{
    gaddr list = R(0), proc = R(1), ref = R(2), found = 0;
    if (list && proc && rd32(list + 4)) {
        gaddr b = hash_bucket(list, R(3));
        for (gaddr at = b; rd32(at) && !found; at = rd32(at)) {
            gaddr l = rd32(at);
            uint32_t args[2] = { ref, l };
            if (ot_callproc(proc, 0x000003D1, args, 2) & 0xFF) {
                found = l;
                if (remove) {
                    wr32(at, rd32(l));
                    wr32(l, 0);
                }
            }
        }
    }
    RETURN(found);
}

static void u_FindInHashList(ppc_cpu *c) { hash_find(c, false); }
static void u_RemoveFromHashList(ppc_cpu *c) { hash_find(c, true); }

/* ---- Random numbers ---- */

static void u_GetRandomSeed(ppc_cpu *c) { RETURN((uint32_t)(micros() * 2654435761u) ^ tb_TickCount()); }

/* OTGetRandomNumber(&seed, lo, hi): a linear congruential step, the result
 * between lo and hi inclusive. */
static void u_GetRandomNumber(ppc_cpu *c)
{
    gaddr seedp = R(0);
    uint32_t lo = R(1), hi = R(2), seed = seedp ? rd32(seedp) : 0x12345678u;
    seed = seed * 1664525u + 1013904223u;
    if (seedp)
        wr32(seedp, seed);
    if (hi < lo) {
        uint32_t t = lo;
        lo = hi;
        hi = t;
    }
    uint32_t span = hi - lo + 1;
    RETURN(span ? lo + (seed >> 8 | seed << 24) % span : seed);
}

/* ---- Gates ---- */

/* OTGate: fLIFO (the newly queued links), fList (the ones in order), fProc,
 * fNumQueued, fInside.  A gate lets one caller through at a time: the first
 * to OTEnterGate goes on (true); the others leave their link and go (false),
 * and whoever leaves the gate has the queued links handed to fProc in turn. */
static void u_InitGate(ppc_cpu *c)
{
    gaddr g = R(0);
    if (g) {
        gmemset(g, 0, 20);
        wr32(g + 8, R(1));
    }
    c->pc = c->lr;
}

static void u_EnterGate(ppc_cpu *c)
{
    gaddr g = R(0), link = R(1);
    if (!g) {
        RETURN(0);
        return;
    }
    if (rd32(g + 16) == 0 && rd32(g + 12) == 0) {
        wr32(g + 16, 1);
        RETURN(1);
        return;
    }
    if (link) { /* queued behind whoever is inside */
        wr32(link, rd32(g));
        wr32(g, link);
        wr32(g + 12, rd32(g + 12) + 1);
    }
    RETURN(0);
}

static gaddr gate_next(gaddr g)
{
    gaddr first = rd32(g + 4);
    if (!first) { /* take what has arrived, oldest first */
        gaddr l = rd32(g), prev = 0;
        wr32(g, 0);
        while (l) {
            gaddr next = rd32(l);
            wr32(l, prev);
            prev = l;
            l = next;
        }
        first = prev;
    }
    if (first) {
        wr32(g + 4, rd32(first));
        wr32(first, 0);
        wr32(g + 12, rd32(g + 12) - 1);
    }
    return first;
}

/* OTLeaveGate: true when the gate is open again, false when a queued link's
 * procedure kept it (by returning false, to carry the work on). */
static void u_LeaveGate(ppc_cpu *c)
{
    gaddr g = R(0);
    if (!g) {
        RETURN(0);
        return;
    }
    for (gaddr l; (l = gate_next(g));) {
        uint32_t args[1] = { l };
        if (!(ot_callproc(rd32(g + 8), 0x000000D1 /* C, Boolean result, one long */, args, 1) & 0xFF)) {
            RETURN(0);
            return;
        }
    }
    wr32(g + 16, 0);
    RETURN(1);
}

/* ---- Port references ---- */

/* An OTPortRef packs the device type (10 bits), the bus type (4), the slot
 * (8) and a number the driver chooses (8): device type in the low bits. */
static uint32_t port_ref(uint32_t bus, uint32_t dev, uint32_t slot, uint32_t other)
{
    return (dev & 0x3FF) | (bus & 0xF) << 10 | (slot & 0xFF) << 14 | (other & 0xFF) << 22;
}

uint32_t ot_make_port_ref(uint32_t bus, uint32_t dev, uint32_t slot, uint32_t other)
{
    return port_ref(bus, dev, slot, other);
}

static void u_CreatePortRef(ppc_cpu *c) { RETURN(port_ref(R(0), R(1), R(2), R(3))); }
static void u_GetDeviceTypeFromPortRef(ppc_cpu *c) { RETURN(R(0) & 0x3FF); }
static void u_GetBusTypeFromPortRef(ppc_cpu *c) { RETURN(R(0) >> 10 & 0xF); }

static void u_GetSlotFromPortRef(ppc_cpu *c)
{
    if (R(1))
        wr16(R(1), (uint16_t)(R(0) >> 22 & 0xFF));
    RETURN(R(0) >> 14 & 0xFF);
}

static void u_SetDeviceTypeInPortRef(ppc_cpu *c) { RETURN((R(0) & ~0x3FFu) | (R(1) & 0x3FF)); }
static void u_SetBusTypeInPortRef(ppc_cpu *c) { RETURN((R(0) & ~(0xFu << 10)) | (R(1) & 0xF) << 10); }

/* ---- No-copy buffers ---- */

/* An OTBuffer chain OT hands out (kOTNetbufDataIsOTBufferStar) is one
 * block per buffer: the 28-byte header, then its bytes. */
enum { kBufferHeader = 28, kBufferMagic = 0x4F544266 /* 'OTBf' */ };

gaddr ot_buffer_new(const uint8_t *data, uint32_t len)
{
    gaddr b = tb_NewPtr((int32_t)(kBufferHeader + len), false, true);
    if (!b)
        return 0;
    gmemset(b, 0, kBufferHeader);
    wr32(b + 12, b + kBufferHeader);
    wr32(b + 16, len);
    wr32(b + 20, kBufferMagic);
    if (len)
        gput(b + kBufferHeader, data, len);
    return b;
}

/* OTReleaseBuffer(OTBuffer *): the chain given back. */
static void u_ReleaseBuffer(ppc_cpu *c)
{
    for (gaddr b = R(0), next; b; b = next) {
        next = rd32(b + 8);
        if (rd32(b + 20) == kBufferMagic) {
            wr32(b + 20, 0);
            tb_DisposePtr(b);
        }
    }
    c->pc = c->lr;
}

/* StoreMsgIntoNetbuf(TNetbuf *, OTBuffer *): the chain's bytes into the
 * netbuf as far as maxlen allows (len set); true if all of it fit. */
static void u_StoreMsgIntoNetbuf(ppc_cpu *c)
{
    gaddr nb = R(0);
    uint32_t room = nb ? rd32(nb) : 0, n = 0;
    bool all = true;
    for (gaddr b = R(1); nb && b; b = rd32(b + 8)) {
        uint32_t len = rd32(b + 16), k = len < room - n ? len : room - n;
        if (k)
            gmemcpy(rd32(nb + 8) + n, rd32(b + 12), k);
        n += k;
        all &= k == len;
    }
    if (nb)
        wr32(nb + 4, n);
    RETURN(all);
}

/* ---- Shared client memory and the rest of the allocators ---- */

static void u_AllocSharedClientMem(ppc_cpu *c) { RETURN(tb_NewPtr((int32_t)R(0), true, true)); }

static void u_FreeSharedClientMem(ppc_cpu *c)
{
    if (R(0))
        tb_DisposePtr(R(0));
    c->pc = c->lr;
}

/* ---- Exports ---- */

const ppc_export otu_exports[] = {
    { "OTEnterInterrupt", u_EnterInterrupt }, { "OTLeaveInterrupt", u_LeaveInterrupt },
    { "OTIsAtInterruptLevel", u_IsAtInterruptLevel }, { "OTCanLoadLibraries", u_CanLoadLibraries },
    { "OTCanMakeSyncCall", u_CanMakeSyncCall },
    { "OTEnterCriticalSection", u_EnterCriticalSection }, { "OTLeaveCriticalSection", u_nop },
    { "OTEnterSynchronousSection", u_EnterSynchronousSection },
    { "OTLeaveSynchronousSection", u_LeaveSynchronousSection },
    { "OTSubtractTimeStamps", u_SubtractTimeStamps }, { "OTTimeStampInMicroseconds", u_TimeStampInMicroseconds },
    { "OTGetClockTimeInSecs", u_GetClockTimeInSecs }, { "OTGetTimerTicks", u_GetTimerTicks },
    { "OTEnqueue", u_Enqueue }, { "OTDequeue", u_Dequeue },
    { "OTSetBit", u_SetBit }, { "OTClearBit", u_ClearBit }, { "OTTestBit", u_TestBit },
    { "OTSetFirstClearBit", u_SetFirstClearBit },
    { "OTCalculateHashListMemoryNeeds", u_CalculateHashListMemoryNeeds }, { "OTInitHashList", u_InitHashList },
    { "OTAddToHashList", u_AddToHashList }, { "OTRemoveLinkFromHashList", u_RemoveLinkFromHashList },
    { "OTIsInHashList", u_IsInHashList }, { "OTFindInHashList", u_FindInHashList },
    { "OTRemoveFromHashList", u_RemoveFromHashList },
    { "OTGetRandomSeed", u_GetRandomSeed }, { "OTGetRandomNumber", u_GetRandomNumber },
    { "OTInitGate", u_InitGate }, { "OTEnterGate", u_EnterGate }, { "OTLeaveGate", u_LeaveGate },
    { "OTCreatePortRef", u_CreatePortRef }, { "OTGetDeviceTypeFromPortRef", u_GetDeviceTypeFromPortRef },
    { "OTGetBusTypeFromPortRef", u_GetBusTypeFromPortRef }, { "OTGetSlotFromPortRef", u_GetSlotFromPortRef },
    { "OTSetDeviceTypeInPortRef", u_SetDeviceTypeInPortRef }, { "OTSetBusTypeInPortRef", u_SetBusTypeInPortRef },
    { "OTReleaseBuffer", u_ReleaseBuffer }, { "StoreMsgIntoNetbuf", u_StoreMsgIntoNetbuf },
    { "OTAllocSharedClientMem", u_AllocSharedClientMem }, { "OTFreeSharedClientMem", u_FreeSharedClientMem },
};
const unsigned otu_export_count = sizeof otu_exports / sizeof otu_exports[0];
