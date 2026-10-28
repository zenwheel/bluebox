/*
 * The hardware libraries' calls: PCILib (the Expansion Manager's accesses
 * to a PCI device's configuration and I/O space, and the byte swaps) and
 * VideoServicesLib (the video interrupt services and the hardware cursor).
 *
 * Blue Box has no PCI bus and no video card to reach, so a bus access
 * answers what the Expansion Manager did where the device was not there: a
 * node that is no entry is nrInvalidNodeErr, one not on a PCI bus
 * nrNotFoundErr, and one that claims to be on a PCI bus, there being no
 * bus behind it, noHardwareErr.  Video has no interrupt to wait for but
 * the program's own: a service fires when the program says it does
 * (VSLDoInterruptService) and no hardware cursor can be used.
 */
#include <string.h>

#include "os9rest.h"
#include "os9svc.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(int32_t)(v); c->pc = c->lr; return; } while (0)

enum { nrInvalidNodeErr = -2538, nrNotFoundErr = -2539, noHardwareErr = -200 };

/* What reaching a device through the Expansion Manager comes to. */
static int32_t bus_access(gaddr node)
{
    bool pci;
    if (!node)
        return paramErr;
    int err = os9_registry_on_pci(node, &pci);
    if (err)
        return err;
    return pci ? noHardwareErr : nrNotFoundErr;
}

int16_t tb_ExpMgrConfigReadByte(gaddr node, gaddr addr, gaddr value) { return (int16_t)bus_access(node); }
int16_t tb_ExpMgrConfigReadWord(gaddr node, gaddr addr, gaddr value) { return (int16_t)bus_access(node); }
int16_t tb_ExpMgrConfigReadLong(gaddr node, gaddr addr, gaddr value) { return (int16_t)bus_access(node); }
int16_t tb_ExpMgrIOReadByte(gaddr node, gaddr addr, gaddr value) { return (int16_t)bus_access(node); }
int16_t tb_ExpMgrIOReadWord(gaddr node, gaddr addr, gaddr value) { return (int16_t)bus_access(node); }
int16_t tb_ExpMgrIOReadLong(gaddr node, gaddr addr, gaddr value) { return (int16_t)bus_access(node); }
int16_t tb_ExpMgrConfigWriteByte(gaddr node, gaddr addr, uint8_t value) { return (int16_t)bus_access(node); }
int16_t tb_ExpMgrConfigWriteWord(gaddr node, gaddr addr, uint16_t value) { return (int16_t)bus_access(node); }
int16_t tb_ExpMgrConfigWriteLong(gaddr node, gaddr addr, uint32_t value) { return (int16_t)bus_access(node); }
int16_t tb_ExpMgrIOWriteByte(gaddr node, gaddr addr, uint8_t value) { return (int16_t)bus_access(node); }
int16_t tb_ExpMgrIOWriteWord(gaddr node, gaddr addr, uint16_t value) { return (int16_t)bus_access(node); }
int16_t tb_ExpMgrIOWriteLong(gaddr node, gaddr addr, uint32_t value) { return (int16_t)bus_access(node); }

int16_t tb_ExpMgrInterruptAcknowledgeReadByte(gaddr entry, gaddr value) { return (int16_t)bus_access(entry); }
int16_t tb_ExpMgrInterruptAcknowledgeReadWord(gaddr entry, gaddr value) { return (int16_t)bus_access(entry); }
int16_t tb_ExpMgrInterruptAcknowledgeReadLong(gaddr entry, gaddr value) { return (int16_t)bus_access(entry); }
int16_t tb_ExpMgrSpecialCycleWriteLong(gaddr entry, uint32_t value) { return (int16_t)bus_access(entry); }
/* A broadcast reaches every bus there is, which is none. */
int16_t tb_ExpMgrSpecialCycleBroadcastLong(uint32_t value) { return noHardwareErr; }

/* EndianSwap16Bit(x) and EndianSwap32Bit(x): the bytes reversed. */
static void pci_EndianSwap16(ppc_cpu *c) { RETURN((uint16_t)(R(0) << 8 | (R(0) >> 8 & 0xFF))); }
static void pci_EndianSwap32(ppc_cpu *c) { RETURN(__builtin_bswap32(R(0))); }

/* ---- VideoServicesLib ---- */

enum { MAX_SERVICES = 16, SERVICE_TAG = 0x56530000u };
static struct {
    bool used;
    uint32_t type, fired;
} services[MAX_SERVICES];

void os9rest_hw_reset(void) { memset(services, 0, sizeof services); }

static bool service_type(uint32_t t)
{
    static const uint32_t types[] = { 0x76626C20, 0x68626C20, 0x6672616D, 0x64636920, 0x63686E67, 0x72656D76, 0x61646420 };
    for (unsigned i = 0; i < sizeof types / sizeof types[0]; i++)
        if (t == types[i])
            return true;
    return false;
}

/* VSLNewInterruptService(device, type, *serviceID) */
static void vsl_New(ppc_cpu *c)
{
    bool pci;
    if (!R(0) || !R(2))
        RETURN(paramErr);
    int err = os9_registry_on_pci(R(0), &pci);
    if (err)
        RETURN(err);
    if (!service_type(R(1)))
        RETURN(paramErr);
    for (int i = 0; i < MAX_SERVICES; i++)
        if (!services[i].used) {
            services[i].used = true;
            services[i].type = R(1);
            services[i].fired = 0;
            wr32(R(2), SERVICE_TAG | (uint32_t)(i + 1));
            RETURN(noErr);
        }
    RETURN(memFullErr);
}

static int service_of(uint32_t id)
{
    uint32_t i = (id & 0xFFFF) - 1;
    return (id & 0xFFFF0000u) == SERVICE_TAG && i < MAX_SERVICES && services[i].used ? (int)i : -1;
}

static void vsl_Dispose(ppc_cpu *c)
{
    int i = service_of(R(0));
    if (i < 0)
        RETURN(paramErr);
    services[i].used = false;
    RETURN(noErr);
}

/* VSLDoInterruptService(serviceID): the service has happened. */
static void vsl_Do(ppc_cpu *c)
{
    int i = service_of(R(0));
    if (i < 0)
        RETURN(paramErr);
    services[i].fired++;
    RETURN(noErr);
}

/* VSLPrepareCursorForHardwareCursor(cursorRef, descriptor, info): the
 * answer of a card with no hardware cursor, false, the cursor drawn in
 * software. */
static void vsl_PrepareCursor(ppc_cpu *c) { RETURN(0); }

const ppc_export os9rest_hw_exports[] = {
    { "EndianSwap16Bit", pci_EndianSwap16 }, { "EndianSwap32Bit", pci_EndianSwap32 },
    { "VSLNewInterruptService", vsl_New }, { "VSLDisposeInterruptService", vsl_Dispose },
    { "VSLDoInterruptService", vsl_Do }, { "VSLPrepareCursorForHardwareCursor", vsl_PrepareCursor },
};
const unsigned os9rest_hw_export_count = sizeof os9rest_hw_exports / sizeof os9rest_hw_exports[0];
