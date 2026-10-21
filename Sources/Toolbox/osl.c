/*
 * Object Support Library.
 */
#include "machine.h"
#include "tb_internal.h"

#define T(s) fourcc(s)

enum { errAEHandlerNotFound = -1717 };

static void set_desc(gaddr desc, uint32_t type, gaddr h)
{
    AEDesc_set_descriptorType(desc, (int32_t)type);
    AEDesc_set_dataHandle(desc, h);
}

static uint32_t desc_type(gaddr desc) { return (uint32_t)AEDesc_descriptorType(desc); }


typedef struct {
    uint32_t desired, container;
    gaddr accessor;
    int32_t refcon;
} accessor_entry;

static accessor_entry accessors[32];
static int naccessors;

int16_t tb_AEInstallObjectAccessor(int32_t desired, int32_t container, gaddr accessor, int32_t refcon, int8_t sys)
{
    for (int i = 0; i < naccessors; i++)
        if (accessors[i].desired == (uint32_t)desired && accessors[i].container == (uint32_t)container) {
            accessors[i].accessor = accessor;
            accessors[i].refcon = refcon;
            return noErr;
        }
    if (naccessors >= 32)
        return memFullErr;
    accessors[naccessors++] = (accessor_entry){ (uint32_t)desired, (uint32_t)container, accessor, refcon };
    return noErr;
}

int16_t tb_AERemoveObjectAccessor(int32_t desired, int32_t container, gaddr accessor, int8_t sys)
{
    for (int i = 0; i < naccessors; i++)
        if (accessors[i].desired == (uint32_t)desired && accessors[i].container == (uint32_t)container) {
            accessors[i] = accessors[--naccessors];
            return noErr;
        }
    return errAEHandlerNotFound;
}

int16_t tb_AEGetObjectAccessor(int32_t desired, int32_t container, gaddr accessor_out, gaddr refcon_out, int8_t sys)
{
    for (int i = 0; i < naccessors; i++)
        if (accessors[i].desired == (uint32_t)desired && accessors[i].container == (uint32_t)container) {
            wr32(accessor_out, accessors[i].accessor);
            wr32(refcon_out, (uint32_t)accessors[i].refcon);
            return noErr;
        }
    return errAEHandlerNotFound;
}

int16_t tb_AECallObjectAccessor(int32_t desired, gaddr container_token, int32_t container_class, int32_t key_form,
                                gaddr key_data, gaddr token)
{
    return -1723; /* errAEAccessorNotFound */
}

int16_t tb_AESetObjectCallbacks(gaddr compare, gaddr count, gaddr dispose, gaddr getmark, gaddr mark, gaddr adjust,
                                gaddr errdesc)
{
    return noErr;
}

/* A null specifier is the application itself, whose token is null too
 * (programs resolve their events' direct object, absent from oapp, this
 * way: Netscape Navigator). */
int16_t tb_AEResolve(gaddr spec, int16_t flags, gaddr token)
{
    set_desc(token, T("null"), 0);
    if (!spec || desc_type(spec) == T("null"))
        return noErr;
    return -1728; /* errAENoSuchObject */
}

int16_t tb_AEDisposeToken(gaddr token) { return tb_AEDisposeDesc(token); }

void osl_reset(void) { naccessors = 0; }
