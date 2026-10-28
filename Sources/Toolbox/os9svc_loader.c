/*
 * DriverLoaderLib (Devices.h's Driver Loader API): the unit table's drivers
 * (the native ones and any a program installed) are looked up, described,
 * switched for Driver Gestalt, opened and removed; PowerPC driver
 * fragments are loaded through the Code Fragment Manager and checked for
 * the DoDriverIO entry and TheDriverDescription the Driver Loader needs;
 * and a registry entry's drivers are searched for, finding none, as Blue
 * Box has no driver files for its (empty) device tree.  Installing a
 * PowerPC driver (InstallDriverFrom...) answers unimpErr: the Device
 * Manager here runs 68k drivers and its own native ones, not NDRVs.
 */
#include <stdlib.h>
#include <string.h>

#include "os9svc.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(int32_t)(v); c->pc = c->lr; return; } while (0)

enum { unimpErr = -4, badUnitErr = -21, unitEmptyErr = -22, fnfErr_ = -43 };
enum { cfragFragmentFormatErr = -2806 };
enum { kmDriverGestaltEnableMask = 0x0004, dRAMBasedMask = 0x0040, dOpenedMask = 0x0020 };
enum { kPrivateCFragCopy = 5 };

const os9_alias os9_loader_aliases[] = { { "", "" } };
const unsigned os9_loader_alias_count = 0;

/* ---- The unit table ---- */

/* The device control entry of a driver's reference number. */
static gaddr dce_of(int16_t refnum)
{
    int16_t unit = (int16_t)(-1 - refnum);
    gaddr table = rd32(LM_UTableBase);
    if (refnum >= 0 || !table || unit >= (int16_t)rd16(LM_UnitNtryCnt))
        return 0;
    gaddr h = rd32(table + 4 * (uint32_t)unit);
    return h ? rd32(h) : 0;
}

/* The driver header the entry points to. */
static gaddr header_of(gaddr dce)
{
    gaddr d = rd32(dce + DCtlEntry_dCtlDriver_off);
    if (d && (rd16(dce + DCtlEntry_dCtlFlags_off) & dRAMBasedMask))
        d = rd32(d); /* a handle */
    return d;
}

static void ds_HighestUnitNumber(ppc_cpu *c) { RETURN(rd16(LM_UnitNtryCnt) ? rd16(LM_UnitNtryCnt) - 1 : 0); }

/* LookupDrivers(beginningUnit, endingUnit, emptyUnits, *returnedRefNums,
 * refNums): the units in the range that have a driver (or, with emptyUnits,
 * those that don't); room for *returnedRefNums on the way in. */
static void ds_LookupDrivers(ppc_cpu *c)
{
    uint32_t begin = R(0) & 0xFFFF, end = R(1) & 0xFFFF, room;
    bool empty = (R(2) & 0xFF) != 0;
    if (!R(3) || !R(4) || begin > end)
        RETURN(paramErr);
    room = rd32(R(3));
    uint32_t n = 0, count = rd16(LM_UnitNtryCnt);
    for (uint32_t u = begin; u <= end && u < count && n < room; u++) {
        bool has = dce_of((int16_t)(-1 - (int32_t)u)) != 0;
        if (has != empty)
            wr16(R(4) + 2 * n++, (uint16_t)(-1 - (int32_t)u));
    }
    wr32(R(3), n);
    RETURN(noErr);
}

/* GetDriverInformation(refNum, *unitNum, *flags, *count, name, device,
 * driverLoadLocation, *fragmentConnID, *fragmentMain, driverDesc): of a
 * driver in the unit table; ours are not fragments, with no device in the
 * registry, so those come back empty. */
static void ds_GetDriverInformation(ppc_cpu *c)
{
    gaddr e = dce_of((int16_t)R(0));
    if (!e)
        RETURN(badUnitErr);
    uint16_t flags = rd16(e + DCtlEntry_dCtlFlags_off);
    if (R(1))
        wr16(R(1), (uint16_t)(-1 - (int16_t)R(0)));
    if (R(2))
        wr16(R(2), flags);
    if (R(3))
        wr32(R(3), flags & dOpenedMask ? 1 : 0);
    if (R(4)) {
        gaddr h = header_of(e), name = h ? h + 18 : 0;
        uint32_t n = name ? rd8(name) : 0;
        wr8(R(4), (uint8_t)n);
        for (uint32_t i = 0; i < n; i++)
            wr8(R(4) + 1 + i, rd8(name + 1 + i));
    }
    for (int i = 0; R(5) && i < 4; i++)
        wr32(R(5) + 4u * (uint32_t)i, 0); /* no registry entry */
    for (int i = 0; R(6) && i < 4; i++)
        wr32(R(6) + 4u * (uint32_t)i, 0); /* no load location */
    if (R(7))
        wr32(R(7), 0);
    if (R(8))
        wr32(R(8), 0);
    RETURN(noErr);
}

static void gestalt_flag(ppc_cpu *c, bool on)
{
    gaddr e = dce_of((int16_t)R(0));
    if (!e)
        RETURN(badUnitErr);
    uint16_t f = rd16(e + DCtlEntry_dCtlFlags_off);
    wr16(e + DCtlEntry_dCtlFlags_off, (uint16_t)(on ? f | kmDriverGestaltEnableMask : f & ~kmDriverGestaltEnableMask));
    RETURN(noErr);
}

static void ds_DriverGestaltOn(ppc_cpu *c) { gestalt_flag(c, true); }
static void ds_DriverGestaltOff(ppc_cpu *c) { gestalt_flag(c, false); }
static void ds_DriverGestaltIsOn(ppc_cpu *c) { RETURN((R(0) & kmDriverGestaltEnableMask) != 0); }

/* OpenInstalledDriver(refNum, ioPermission): PBOpen by the driver's name. */
static void ds_OpenInstalledDriver(ppc_cpu *c)
{
    gaddr e = dce_of((int16_t)R(0));
    if (!e)
        RETURN(badUnitErr);
    gaddr h = header_of(e);
    if (!h)
        RETURN(unitEmptyErr);
    gaddr pb = tb_NewPtr(IOParam_SIZEOF, false, true);
    if (!pb)
        RETURN(memFullErr);
    wr32(pb + IOParam_ioNamePtr_off, h + 18);
    wr8(pb + IOParam_ioPermssn_off, (uint8_t)R(1));
    int16_t err = tb_PBOpen(pb, false);
    tb_DisposePtr(pb);
    RETURN(err);
}

static void ds_RemoveDriver(ppc_cpu *c) { RETURN(tb_DrvrRemove((int16_t)R(0))); }

/* RenameDriver(refNum, newName): the name in a pointer-based driver's
 * header, in a header of its own made for the entry. */
static void ds_RenameDriver(ppc_cpu *c)
{
    gaddr e = dce_of((int16_t)R(0)), name = R(1);
    if (!e)
        RETURN(badUnitErr);
    if (!name)
        RETURN(paramErr);
    if (rd16(e + DCtlEntry_dCtlFlags_off) & dRAMBasedMask)
        RETURN(unimpErr);
    gaddr old = header_of(e);
    uint32_t n = rd8(name);
    gaddr hdr = tb_NewPtr((int32_t)(19 + n), true, true);
    if (!hdr)
        RETURN(memFullErr);
    if (old)
        for (uint32_t i = 0; i < 18; i++)
            wr8(hdr + i, rd8(old + i));
    wr8(hdr + 18, (uint8_t)n);
    for (uint32_t i = 0; i < n; i++)
        wr8(hdr + 19 + i, rd8(name + 1 + i));
    wr32(e + DCtlEntry_dCtlDriver_off, hdr); /* the old header may be the ROM's: left */
    RETURN(noErr);
}

/* HigherDriverVersion(v1, v2): 1 if the first NumVersion is later, -1 if
 * the second is, 0 if they are the same. */
static void ds_HigherDriverVersion(ppc_cpu *c)
{
    if (!R(0) || !R(1))
        RETURN(paramErr);
    uint32_t a = rd32(R(0)), b = rd32(R(1));
    RETURN(a > b ? 1 : a < b ? -1 : 0);
}

/* ---- Driver fragments ---- */

/* Call another export as a PowerPC program would, with the registers
 * given from r3, keeping this call's own. */
static uint32_t call_export(ppc_cpu *c, const char *name, const uint32_t *args, int n)
{
    void (*fn)(ppc_cpu *) = cfm_export_fn(name);
    if (!fn)
        return (uint32_t)unimpErr;
    uint32_t saved[8], pc = c->pc;
    for (int i = 0; i < 8; i++)
        saved[i] = c->gpr[3 + i];
    for (int i = 0; i < n; i++)
        c->gpr[3 + i] = args[i];
    fn(c);
    uint32_t r = c->gpr[3];
    for (int i = 0; i < 8; i++)
        c->gpr[3 + i] = saved[i];
    c->pc = pc;
    return r;
}

static gaddr pstring_of(const char *s)
{
    gaddr p = tb_NewPtr(64, false, true);
    if (p) {
        wr8(p, (uint8_t)strlen(s));
        for (size_t i = 0; i < strlen(s); i++)
            wr8(p + 1 + (uint32_t)i, (uint8_t)s[i]);
    }
    return p;
}

/* A symbol of a connection: its address, or the error. */
static int find_symbol(ppc_cpu *c, uint32_t conn, const char *name, gaddr *addr)
{
    gaddr n = pstring_of(name), out = tb_NewPtr(8, false, true);
    if (!n || !out) {
        if (n)
            tb_DisposePtr(n);
        if (out)
            tb_DisposePtr(out);
        return memFullErr;
    }
    uint32_t args[4] = { conn, n, out, out + 4 };
    int err = (int16_t)call_export(c, "FindSymbol", args, 4);
    *addr = rd32(out);
    tb_DisposePtr(n);
    tb_DisposePtr(out);
    return err;
}

/* VerifyFragmentAsDriver(connID, *fragmentMain, *driverDesc): the fragment
 * has DoDriverIO and a TheDriverDescription that says it is one. */
static int verify(ppc_cpu *c, uint32_t conn, gaddr main_out, gaddr desc_out)
{
    gaddr entry, desc;
    int err = find_symbol(c, conn, "DoDriverIO", &entry);
    if (!err)
        err = find_symbol(c, conn, "TheDriverDescription", &desc);
    if (err)
        return err;
    if (rd32(desc) != 0x70646573) /* 'pdes' */
        return cfragFragmentFormatErr;
    if (main_out)
        wr32(main_out, entry);
    if (desc_out)
        wr32(desc_out, desc);
    return noErr;
}

static void ds_VerifyFragmentAsDriver(ppc_cpu *c) { RETURN(verify(c, R(0), R(1), R(2))); }

/* Load a driver fragment (from a file spec or memory) and check it; the
 * connection is closed again when it isn't one. */
static int load_driver(ppc_cpu *c, bool from_memory, gaddr where, uint32_t length, gaddr name, gaddr conn_out,
                       gaddr main_out, gaddr desc_out)
{
    gaddr conn = tb_NewPtr(4, false, true), mainp = tb_NewPtr(4, false, true), msg = tb_NewPtr(256, false, true);
    int err = memFullErr;
    if (conn && mainp && msg) {
        uint32_t r;
        if (from_memory) {
            uint32_t a[7] = { where, length, name, kPrivateCFragCopy, conn, mainp, msg };
            r = call_export(c, "GetMemFragment", a, 7);
        } else {
            uint32_t a[8] = { where, 0, 0, name, kPrivateCFragCopy, conn, mainp, msg };
            r = call_export(c, "GetDiskFragment", a, 8);
        }
        err = (int16_t)r;
        if (!err) {
            err = verify(c, rd32(conn), main_out, desc_out);
            if (err) {
                uint32_t a[1] = { rd32(conn) };
                call_export(c, "CloseConnection", a, 1);
            } else if (conn_out) {
                wr32(conn_out, rd32(conn));
            }
        }
    }
    if (conn)
        tb_DisposePtr(conn);
    if (mainp)
        tb_DisposePtr(mainp);
    if (msg)
        tb_DisposePtr(msg);
    return err;
}

/* GetDriverMemoryFragment(memAddr, length, fragName, *connID, *main, *desc) */
static void ds_GetDriverMemoryFragment(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    RETURN(load_driver(c, true, R(0), R(1), R(2), R(3), R(4), R(5)));
}

/* GetDriverDiskFragment(spec, *connID, *main, *desc) */
static void ds_GetDriverDiskFragment(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    RETURN(load_driver(c, false, R(0), 0, 0, R(1), R(2), R(3)));
}

/* GetNamedDriverDiskFragment(spec, fragName, *connID, *main, *desc) */
static void ds_GetNamedDriverDiskFragment(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    RETURN(load_driver(c, false, R(0), 0, R(1), R(2), R(3), R(4)));
}

/* SetDriverClosureMemory(connID, holdDriverMemory): there is no virtual
 * memory to hold it from. */
static void ds_SetDriverClosureMemory(ppc_cpu *c) { RETURN(R(0) ? noErr : paramErr); }

/* ---- Drivers for a device ---- */

/* FindDriverCandidates(deviceID, *propBasedDriver, *propBasedDriverSize,
 * deviceName, *propBasedDriverType, *gotPropBasedDriver, fileBasedDrivers,
 * *nFileBasedDrivers): the property's driver, and the files in the
 * Extensions folder for the device: there are none. */
static void ds_FindDriverCandidates(ppc_cpu *c)
{
    gaddr device = R(0);
    if (!device)
        RETURN(paramErr);
    const uint8_t *data;
    uint32_t size;
    int err = os9_registry_property(device, "name", &data, &size);
    if (err)
        RETURN(err);
    /* A driver in the device's property is given as a block of its own. */
    const uint8_t *drv;
    uint32_t drv_size = 0;
    bool got = os9_registry_property(device, "driver,AAPL,MacOS,PowerPC", &drv, &drv_size) == noErr && drv_size > 0;
    gaddr copy = got ? tb_NewPtr((int32_t)drv_size, false, true) : 0;
    got = copy != 0;
    for (uint32_t i = 0; got && i < drv_size; i++)
        wr8(copy + i, drv[i]);
    if (R(1))
        wr32(R(1), got ? copy : 0);
    if (R(2))
        wr32(R(2), got ? drv_size : 0);
    if (R(3)) {
        uint32_t n = size && data[size - 1] == 0 ? size - 1 : size;
        if (n > 31)
            n = 31;
        wr8(R(3), (uint8_t)n);
        for (uint32_t i = 0; i < n; i++)
            wr8(R(3) + 1 + i, data[i]);
    }
    if (R(5))
        wr8(R(5), got);
    if (R(7))
        wr32(R(7), 0);
    RETURN(noErr);
}

/* ScanDriverCandidates(deviceID, fileBasedDrivers, n, matching, *nMatching) */
static void ds_ScanDriverCandidates(ppc_cpu *c)
{
    if (!R(0) || !R(4))
        RETURN(paramErr);
    wr32(R(4), 0);
    RETURN(noErr);
}

/* The calls that find a driver for a device and fail, as there is none. */
static void ds_NoDriverForDevice(ppc_cpu *c)
{
    const uint8_t *data;
    uint32_t size;
    if (!R(0))
        RETURN(paramErr);
    int err = os9_registry_property(R(0), "name", &data, &size);
    RETURN(err ? err : fnfErr_);
}

/* The Device Manager here doesn't run PowerPC drivers. */
static void ds_Unimplemented(ppc_cpu *c) { RETURN(unimpErr); }

/* GetCompatibleProperty(device, **compatibleNames, *nCompatibleNames): the
 * entry's "compatible" strings, packed, in a block of their own. */
static void ds_GetCompatibleProperty(ppc_cpu *c)
{
    const uint8_t *data;
    uint32_t size, count = 0;
    if (R(1))
        wr32(R(1), 0);
    if (os9_registry_property(R(0), "compatible", &data, &size) != noErr || !size || !R(1)) {
        if (R(2))
            wr32(R(2), 0);
        c->pc = c->lr;
        return;
    }
    gaddr block = tb_NewPtr((int32_t)size, false, true);
    if (block) {
        for (uint32_t i = 0; i < size; i++) {
            wr8(block + i, data[i]);
            if (!data[i])
                count++;
        }
        wr32(R(1), block);
    }
    if (R(2))
        wr32(R(2), block ? count : 0);
    c->pc = c->lr;
}

/* CompatibleDriverNames(nameInfoStr, compatibleNames, nCompatibleNames,
 * *nameCount): whether the Pascal string is among the packed strings, and
 * how many were before it. */
static void ds_CompatibleDriverNames(ppc_cpu *c)
{
    gaddr name = R(0), names = R(1);
    uint32_t n = R(2), len = name ? rd8(name) : 0;
    gaddr at = names;
    for (uint32_t i = 0; name && names && i < n; i++) {
        uint32_t l = 0;
        while (rd8(at + l))
            l++;
        bool same = l == len;
        for (uint32_t k = 0; same && k < l; k++)
            same = rd8(at + k) == rd8(name + 1 + k);
        if (same) {
            if (R(3))
                wr32(R(3), i);
            RETURN(1);
        }
        at += l + 1;
    }
    if (R(3))
        wr32(R(3), n);
    RETURN(0);
}

/* ---- Descriptions and candidates ---- */

enum { kDescFixedSize = 0x70, kServiceSize = 12 }; /* DriverDescription up to nServices */

/* The bytes a PEF container takes: the farthest end of its sections. */
static uint32_t pef_length(gaddr p)
{
    if (rd32(p) != 0x4A6F7921 || rd32(p + 4) != 0x70656666) /* 'Joy!' 'peff' */
        return 0;
    uint32_t n = rd16(p + 32), end = 40 + 28 * n;
    for (uint32_t i = 0; i < n; i++) {
        gaddr h = p + 40 + 28 * i;
        uint32_t e = rd32(h + 20) + rd32(h + 16);
        if (e > end)
            end = e;
    }
    return end;
}

/* Load a driver fragment and keep its description in a block of its own,
 * the connection let go. */
static int describe(ppc_cpu *c, bool from_memory, gaddr where, uint32_t length, gaddr name, gaddr desc_out)
{
    gaddr conn = tb_NewPtr(8, true, true);
    if (!conn)
        return memFullErr;
    wr32(desc_out, 0);
    int err = load_driver(c, from_memory, where, length, name, conn, 0, conn + 4);
    if (!err) {
        gaddr d = rd32(conn + 4);
        uint32_t n = rd32(d + kDescFixedSize), size = kDescFixedSize + 4 + kServiceSize * (n ? n : 1);
        gaddr copy = n < 256 ? tb_NewPtr((int32_t)size, false, true) : 0;
        for (uint32_t i = 0; copy && i < size; i++)
            wr8(copy + i, rd8(d + i));
        uint32_t a[1] = { rd32(conn) };
        call_export(c, "CloseConnection", a, 1);
        if (copy)
            wr32(desc_out, copy);
        else
            err = memFullErr;
    }
    tb_DisposePtr(conn);
    return err;
}

/* GetDriverDescription(fragmentPtr, **desc) */
static void ds_GetDriverDescription(ppc_cpu *c)
{
    if (!R(1))
        RETURN(paramErr);
    wr32(R(1), 0);
    uint32_t len = R(0) ? pef_length(R(0)) : 0;
    if (!len)
        RETURN(cfragFragmentFormatErr);
    RETURN(describe(c, true, R(0), len, 0, R(1)));
}

/* GetNamedDriverDescFromFSSpec(spec, fragName, **desc) */
static void ds_GetNamedDriverDescFromFSSpec(ppc_cpu *c)
{
    if (!R(2) || !R(0))
        RETURN(paramErr);
    RETURN(describe(c, false, R(0), 0, R(1), R(2)));
}

/* A Pascal string against another, as the Driver Loader's names are
 * compared: the same length and the same bytes. */
static bool pstr_same(gaddr a, gaddr b)
{
    if (rd8(a) != rd8(b))
        return false;
    for (uint32_t i = 1; i <= rd8(a); i++)
        if (rd8(a + i) != rd8(b + i))
            return false;
    return true;
}

/* CompareFileCandToPropCand(device, deviceName, propBasedCandidate,
 * fileBasedCandidate): which of a device's two drivers is better, 1 for
 * the one in the device's property and -1 for the file's, 0 for neither.
 * A driver whose name is the device's own beats one that matched through
 * a compatible name, and of two alike the higher version wins. */
static void ds_CompareFileCandToPropCand(ppc_cpu *c)
{
    gaddr name = R(1), prop = R(2), file = R(3);
    if (!name || !prop || !file)
        RETURN(paramErr);
    bool prop_exact = pstr_same(name, prop), file_exact = rd8(file + 0x6A) == 0;
    if (prop_exact != file_exact)
        RETURN(prop_exact ? 1 : -1);
    uint32_t pv = rd32(prop + 32), fv = rd32(file + 0x46 + 32);
    RETURN(pv > fv ? 1 : pv < fv ? -1 : 0);
}

const ppc_export os9_loader_exports[] = {
    { "GetDriverDescription", ds_GetDriverDescription },
    { "GetNamedDriverDescFromFSSpec", ds_GetNamedDriverDescFromFSSpec },
    { "CompareFileCandToPropCand", ds_CompareFileCandToPropCand },
    { "HighestUnitNumber", ds_HighestUnitNumber }, { "LookupDrivers", ds_LookupDrivers },
    { "GetDriverInformation", ds_GetDriverInformation }, { "DriverGestaltOn", ds_DriverGestaltOn },
    { "DriverGestaltOff", ds_DriverGestaltOff }, { "DriverGestaltIsOn", ds_DriverGestaltIsOn },
    { "OpenInstalledDriver", ds_OpenInstalledDriver }, { "RemoveDriver", ds_RemoveDriver },
    { "RenameDriver", ds_RenameDriver }, { "HigherDriverVersion", ds_HigherDriverVersion },
    { "VerifyFragmentAsDriver", ds_VerifyFragmentAsDriver },
    { "GetDriverMemoryFragment", ds_GetDriverMemoryFragment },
    { "GetDriverDiskFragment", ds_GetDriverDiskFragment },
    { "GetNamedDriverDiskFragment", ds_GetNamedDriverDiskFragment },
    { "SetDriverClosureMemory", ds_SetDriverClosureMemory },
    { "FindDriverCandidates", ds_FindDriverCandidates }, { "FindDriverCandidatesDetailed", ds_FindDriverCandidates },
    { "ScanDriverCandidates", ds_ScanDriverCandidates }, { "ScanDriverCandidatesDetailed", ds_ScanDriverCandidates },
    { "FindDriversForDevice", ds_NoDriverForDevice }, { "FindDriverForDeviceFromFile", ds_NoDriverForDevice },
    { "GetDriverForDevice", ds_NoDriverForDevice }, { "InstallDriverForDevice", ds_NoDriverForDevice },
    { "InstallDriverFromFragment", ds_Unimplemented }, { "InstallDriverFromFile", ds_Unimplemented },
    { "InstallDriverFromMemory", ds_Unimplemented }, { "InstallDriverFromResource", ds_Unimplemented },
    { "InstallDriverFromDisk", ds_Unimplemented }, { "ReplaceDriverWithFragment", ds_Unimplemented },
    { "GetCompatibleProperty", ds_GetCompatibleProperty }, { "CompatibleDriverNames", ds_CompatibleDriverNames },
};
const unsigned os9_loader_export_count = sizeof os9_loader_exports / sizeof os9_loader_exports[0];
