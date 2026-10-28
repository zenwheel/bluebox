/*
 * KeychainLib (KeychainCore.h and KeychainHI.h): the Keychain Manager on a
 * Mac that has no keychain.  Items can be made and read in memory
 * (KCNewItem, attributes and data), callbacks can be registered and the
 * interaction switch set, but there is no keychain to put an item in or
 * find one in: no keychain is the default (errKCNoDefaultKeychain), none
 * can be made or opened (errKCNoStorageModule, errKCNoSuchKeychain), and
 * the certificate and policy calls have no module to run in
 * (errKCNoCertificateModule, errKCNoPolicyModule).  PowerPC only.
 */
#include <stdlib.h>
#include <string.h>

#include "carbon_cf.h"
#include "os9svc.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(int32_t)(v); c->pc = c->lr; return; } while (0)

enum {
    errKCNoStorageModule = -25312,
    errKCNoCertificateModule = -25313,
    errKCNoPolicyModule = -25314,
    errKCNotAvailable = -25291,
    errKCNoSuchKeychain = -25294,
    errKCInvalidKeychain = -25295,
    errKCDuplicateCallback = -25297,
    errKCInvalidCallback = -25298,
    errKCItemNotFound = -25300,
    errKCBufferTooSmall = -25301,
    errKCDataTooLarge = -25302,
    errKCNoSuchAttr = -25303,
    errKCInvalidItemRef = -25304,
    errKCInvalidSearchRef = -25305,
    errKCNoSuchClass = -25306,
    errKCNoDefaultKeychain = -25307,
    errKCReadOnlyAttr = -25309,
};
enum { KC_VERSION = 0x02028000 }; /* Keychain Manager 2.0.2 */
enum { MAX_DATA = 0xFFFF, MAX_ATTR = 24, MAX_ITEMS = 64, MAX_CALLBACKS = 16, ITEM_TAG = 0x4B490000u };

typedef struct {
    uint32_t tag, length;
    uint8_t *data;
} attr;

typedef struct {
    bool used;
    uint32_t cls;
    uint8_t *data;
    uint32_t length;
    attr attrs[MAX_ATTR];
    int nattrs;
    uint32_t refs; /* the references to it: its maker's, and the arrays that hold it */
} kcitem;

static kcitem items[MAX_ITEMS];
static gaddr item_callbacks; /* KCItemRefArrayCallBacks, once asked for */
static gaddr callbacks[MAX_CALLBACKS];
static uint32_t callback_masks[MAX_CALLBACKS];
static bool interaction_allowed = true;

static void item_free(kcitem *it)
{
    for (int i = 0; i < it->nattrs; i++)
        free(it->attrs[i].data);
    free(it->data);
    memset(it, 0, sizeof *it);
}

void os9svc_keychain_reset(void)
{
    for (int i = 0; i < MAX_ITEMS; i++)
        if (items[i].used)
            item_free(&items[i]);
    item_callbacks = 0;
    memset(callbacks, 0, sizeof callbacks);
    memset(callback_masks, 0, sizeof callback_masks);
    interaction_allowed = true;
}

static kcitem *item_of(uint32_t ref)
{
    uint32_t i = (ref & 0xFFFF) - 1;
    return (ref & 0xFFFF0000u) == ITEM_TAG && i < MAX_ITEMS && items[i].used ? &items[i] : NULL;
}

/* Argument n of a call: registers r3 to r10, then the caller's parameter area. */
static uint32_t arg(ppc_cpu *c, int n) { return n < 8 ? c->gpr[3 + n] : rd32(c->gpr[1] + 24 + 4u * (uint32_t)n); }

static bool known_attr(uint32_t tag)
{
    static const char *const tags[] = {
        "clas", "cdat", "mdat", "desc", "icmt", "crtr", "type", "scrp", "labl", "invi", "nega", "cusi", "acct",
        "svce", "gena", "sdmn", "srvr", "atyp", "port", "path", "vlme", "addr", "ssig", "ptcl", "subj", "cn  ",
        "issu", "snbr", "mail", "hpky", "iurl", "encr", "decr", "sign", "veri", "wrap", "unwr", "sdat", "edat",
    };
    for (unsigned i = 0; i < sizeof tags / sizeof tags[0]; i++)
        if (tag == fourcc(tags[i]))
            return true;
    return false;
}

static int item_set_attr(kcitem *it, uint32_t tag, const uint8_t *data, uint32_t len)
{
    attr *a = NULL;
    for (int i = 0; i < it->nattrs; i++)
        if (it->attrs[i].tag == tag)
            a = &it->attrs[i];
    if (!a) {
        if (it->nattrs == MAX_ATTR)
            return memFullErr;
        a = &it->attrs[it->nattrs++];
        a->tag = tag;
    }
    uint8_t *copy = len ? malloc(len) : NULL;
    if (len && !copy)
        return memFullErr;
    if (len)
        memcpy(copy, data, len);
    free(a->data);
    a->data = copy;
    a->length = len;
    return noErr;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* KCNewItem(itemClass, itemCreator, length, data, *item) */
static void kc_NewItem(ppc_cpu *c)
{
    uint32_t cls = R(0), length = R(2);
    if (!R(4))
        RETURN(paramErr);
    wr32(R(4), 0);
    if (cls != fourcc("inet") && cls != fourcc("ashp") && cls != fourcc("genp") && cls != fourcc("cert"))
        RETURN(errKCNoSuchClass);
    if (length > MAX_DATA)
        RETURN(errKCDataTooLarge);
    if (length && !R(3))
        RETURN(paramErr);
    for (int i = 0; i < MAX_ITEMS; i++)
        if (!items[i].used) {
            kcitem *it = &items[i];
            memset(it, 0, sizeof *it);
            it->used = true;
            it->refs = 1;
            it->cls = cls;
            it->data = length ? malloc(length) : NULL;
            if (length && !it->data) {
                it->used = false;
                RETURN(memFullErr);
            }
            for (uint32_t k = 0; k < length; k++)
                it->data[k] = rd8(R(3) + k);
            it->length = length;
            uint8_t v[4];
            put32(v, cls);
            item_set_attr(it, fourcc("clas"), v, 4);
            put32(v, R(1));
            item_set_attr(it, fourcc("crtr"), v, 4);
            put32(v, rd32(LM_Time));
            item_set_attr(it, fourcc("cdat"), v, 4);
            item_set_attr(it, fourcc("mdat"), v, 4);
            wr32(R(4), ITEM_TAG | (uint32_t)(i + 1));
            RETURN(noErr);
        }
    RETURN(memFullErr);
}

/* KCSetAttribute(item, attr): KCAttribute is the tag, the length and a
 * pointer to the data. */
static void kc_SetAttribute(ppc_cpu *c)
{
    kcitem *it = item_of(R(0));
    if (!it || !R(1))
        RETURN(it ? paramErr : errKCInvalidItemRef);
    uint32_t tag = rd32(R(1)), len = rd32(R(1) + 4);
    gaddr data = rd32(R(1) + 8);
    if (!known_attr(tag))
        RETURN(errKCNoSuchAttr);
    if (tag == fourcc("clas") || tag == fourcc("cdat") || tag == fourcc("mdat"))
        RETURN(errKCReadOnlyAttr);
    if (len > MAX_DATA)
        RETURN(errKCDataTooLarge);
    uint8_t *buf = len ? malloc(len) : NULL;
    if (len && (!buf || !data)) {
        free(buf);
        RETURN(data ? memFullErr : paramErr);
    }
    for (uint32_t i = 0; i < len; i++)
        buf[i] = rd8(data + i);
    int err = item_set_attr(it, tag, buf, len);
    free(buf);
    RETURN(err);
}

/* KCGetAttribute(item, attr, *actualLength) */
static void kc_GetAttribute(ppc_cpu *c)
{
    kcitem *it = item_of(R(0));
    if (!it || !R(1))
        RETURN(it ? paramErr : errKCInvalidItemRef);
    uint32_t tag = rd32(R(1)), room = rd32(R(1) + 4);
    gaddr data = rd32(R(1) + 8);
    if (!known_attr(tag))
        RETURN(errKCNoSuchAttr);
    for (int i = 0; i < it->nattrs; i++)
        if (it->attrs[i].tag == tag) {
            if (R(2))
                wr32(R(2), it->attrs[i].length);
            if (it->attrs[i].length > room)
                RETURN(errKCBufferTooSmall);
            for (uint32_t k = 0; data && k < it->attrs[i].length; k++)
                wr8(data + k, it->attrs[i].data[k]);
            RETURN(noErr);
        }
    RETURN(errKCNoSuchAttr);
}

/* KCSetData(item, length, data) */
static void kc_SetData(ppc_cpu *c)
{
    kcitem *it = item_of(R(0));
    if (!it)
        RETURN(errKCInvalidItemRef);
    uint32_t len = R(1);
    if (len > MAX_DATA)
        RETURN(errKCDataTooLarge);
    if (len && !R(2))
        RETURN(paramErr);
    uint8_t *buf = len ? malloc(len) : NULL;
    if (len && !buf)
        RETURN(memFullErr);
    for (uint32_t i = 0; i < len; i++)
        buf[i] = rd8(R(2) + i);
    free(it->data);
    it->data = buf;
    it->length = len;
    RETURN(noErr);
}

/* KCGetData(item, maxLength, data, *actualLength) */
static void kc_GetData(ppc_cpu *c)
{
    kcitem *it = item_of(R(0));
    if (!it)
        RETURN(errKCInvalidItemRef);
    if (R(3))
        wr32(R(3), it->length);
    if (it->length > R(1))
        RETURN(errKCBufferTooSmall);
    for (uint32_t i = 0; R(2) && i < it->length; i++)
        wr8(R(2) + i, it->data[i]);
    RETURN(noErr);
}

/* KCReleaseItem(*item) */
static void kc_ReleaseItem(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    kcitem *it = item_of(rd32(R(0)));
    if (!it)
        RETURN(errKCInvalidItemRef);
    if (--it->refs == 0)
        item_free(it);
    wr32(R(0), 0);
    RETURN(noErr);
}

/* KCItemRefArrayCallBacks: a CFArray of item references retains and
 * releases them as the arrays hold them; the items are compared by
 * reference. */
static void kc_ArrayRetain(ppc_cpu *c)
{
    kcitem *it = item_of(R(1));
    if (it)
        it->refs++;
    RETURN(R(1));
}

static void kc_ArrayRelease(ppc_cpu *c)
{
    kcitem *it = item_of(R(1));
    if (it && --it->refs == 0)
        item_free(it);
    c->pc = c->lr;
}

gaddr os9svc_keychain_data(const char *name)
{
    if (strcmp(name, "KCItemRefArrayCallBacks"))
        return 0;
    if (!item_callbacks) {
        item_callbacks = tb_NewPtr(20, true, true);
        if (item_callbacks) {
            wr32(item_callbacks + 4, cf_native(kc_ArrayRetain, "KCRetainItemRef"));
            wr32(item_callbacks + 8, cf_native(kc_ArrayRelease, "KCReleaseItemRef"));
        }
    }
    return item_callbacks;
}

/* The library's start-up and shut-down routines and the SOM run-time
 * checks its C++ classes were built with: nothing to do for them here. */
static void kc_Init(ppc_cpu *c) { RETURN(noErr); }
static void kc_Nothing(ppc_cpu *c) { c->pc = c->lr; }

/* What a keychain call does where there is no keychain: a null KCRef
 * means the default one, which there isn't; any other is not a keychain. */
static void kc_ByKeychain(ppc_cpu *c) { RETURN(R(0) ? errKCInvalidKeychain : errKCNoDefaultKeychain); }

/* The calls that act on an item and a keychain: the item must be one. */
static void kc_ItemInKeychain(ppc_cpu *c)
{
    if (!item_of(R(0)))
        RETURN(errKCInvalidItemRef);
    RETURN(errKCNoDefaultKeychain); /* an item made here is in no keychain; none to add it to */
}

static void kc_ItemNotInKeychain(ppc_cpu *c)
{
    if (!item_of(R(0)))
        RETURN(errKCInvalidItemRef);
    RETURN(errKCItemNotFound);
}

/* KCGetKeychain(item, *keychain) */
static void kc_GetKeychain(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), 0);
    RETURN(item_of(R(0)) ? errKCNoSuchKeychain : errKCInvalidItemRef);
}

/* KCCopyItem(item, destKeychain, *copy) */
static void kc_CopyItem(ppc_cpu *c)
{
    if (R(2))
        wr32(R(2), 0);
    if (!item_of(R(0)))
        RETURN(errKCInvalidItemRef);
    RETURN(R(1) ? errKCInvalidKeychain : errKCNoDefaultKeychain);
}

static void kc_GetDefaultKeychain(ppc_cpu *c)
{
    if (R(0))
        wr32(R(0), 0);
    RETURN(errKCNoDefaultKeychain);
}

static void kc_SetDefaultKeychain(ppc_cpu *c) { RETURN(errKCInvalidKeychain); }

static void kc_GetStatus(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), 0);
    RETURN(R(0) ? errKCInvalidKeychain : errKCNoDefaultKeychain);
}

static void kc_GetKeychainName(ppc_cpu *c) { RETURN(R(0) ? errKCInvalidKeychain : errKCNoDefaultKeychain); }

static void kc_CountKeychains(ppc_cpu *c) { RETURN(0); }

static void kc_GetIndKeychain(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), 0);
    RETURN(errKCNoSuchKeychain);
}

/* KCReleaseKeychain(*keychain) */
static void kc_ReleaseKeychain(ppc_cpu *c) { RETURN(errKCInvalidKeychain); }

static void kc_MakeKCRef(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), 0);
    RETURN(errKCNoSuchKeychain);
}

static void kc_MakeAliasFromKCRef(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), 0);
    RETURN(errKCInvalidKeychain);
}

static void kc_NoStorage(ppc_cpu *c) { RETURN(errKCNoStorageModule); }

/* KCCreateKeychain(password, *keychain) */
static void kc_CreateKeychain(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), 0);
    RETURN(errKCNoStorageModule);
}

/* KCFindFirstItem(keychain, attrList, *search, *item) */
static void kc_FindFirstItem(ppc_cpu *c)
{
    if (R(2))
        wr32(R(2), 0);
    if (R(3))
        wr32(R(3), 0);
    RETURN(R(0) ? errKCInvalidKeychain : errKCNoDefaultKeychain);
}

static void kc_FindNextItem(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), 0);
    RETURN(errKCInvalidSearchRef);
}

static void kc_ReleaseSearch(ppc_cpu *c) { RETURN(errKCInvalidSearchRef); }

/* KCAddCallback(callbackProc, eventMask, userContext) */
static void kc_AddCallback(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    for (int i = 0; i < MAX_CALLBACKS; i++)
        if (callbacks[i] == R(0))
            RETURN(errKCDuplicateCallback);
    for (int i = 0; i < MAX_CALLBACKS; i++)
        if (!callbacks[i]) {
            callbacks[i] = R(0);
            callback_masks[i] = R(1);
            RETURN(noErr);
        }
    RETURN(memFullErr);
}

static void kc_RemoveCallback(ppc_cpu *c)
{
    for (int i = 0; i < MAX_CALLBACKS; i++)
        if (R(0) && callbacks[i] == R(0)) {
            callbacks[i] = 0;
            RETURN(noErr);
        }
    RETURN(errKCInvalidCallback);
}

static void kc_GetKeychainManagerVersion(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    wr32(R(0), KC_VERSION);
    RETURN(noErr);
}

static void kc_SetInteractionAllowed(ppc_cpu *c)
{
    interaction_allowed = (R(0) & 0xFF) != 0;
    RETURN(noErr);
}

static void kc_IsInteractionAllowed(ppc_cpu *c) { RETURN(interaction_allowed); }

/* The password calls: nothing is found and nothing can be added, there
 * being no keychain.  The outputs (the actual length, the item) are
 * cleared; item_arg and len_arg say where they are in the call. */
static void password_call(ppc_cpu *c, int len_arg, int item_arg)
{
    if (len_arg >= 0 && arg(c, len_arg))
        wr32(arg(c, len_arg), 0);
    if (item_arg >= 0 && arg(c, item_arg))
        wr32(arg(c, item_arg), 0);
    RETURN(errKCNoDefaultKeychain);
}

#define PW(name, len, item) static void kc_##name(ppc_cpu *c) { password_call(c, len, item); }
PW(AddAppleSharePassword, -1, 7)
PW(FindAppleSharePassword, 7, 8)
PW(AddInternetPassword, -1, 8)
PW(AddInternetPasswordWithPath, -1, 9)
PW(FindInternetPassword, 8, 9)
PW(FindInternetPasswordWithPath, 9, 10)
PW(AddGenericPassword, -1, 4)
PW(FindGenericPassword, 4, 5)

static void kc_NoCertificates(ppc_cpu *c) { RETURN(errKCNoCertificateModule); }
static void kc_NoPolicies(ppc_cpu *c) { RETURN(errKCNoPolicyModule); }
static void kc_NotAvailable(ppc_cpu *c) { RETURN(errKCNotAvailable); }

const ppc_export os9_keychain_exports[] = {
    { "KCGetKeychainManagerVersion", kc_GetKeychainManagerVersion },
    { "KCSetInteractionAllowed", kc_SetInteractionAllowed }, { "KCIsInteractionAllowed", kc_IsInteractionAllowed },
    { "KCNewItem", kc_NewItem }, { "KCSetAttribute", kc_SetAttribute }, { "KCGetAttribute", kc_GetAttribute },
    { "KCSetData", kc_SetData }, { "KCGetData", kc_GetData },
    { "KCReleaseItem", kc_ReleaseItem }, { "KCReleaseItemRef", kc_ReleaseItem },
    { "KCAddItem", kc_ItemInKeychain }, { "KCUpdateItem", kc_ItemInKeychain },
    { "KCDeleteItem", kc_ItemNotInKeychain }, { "KCGetKeychain", kc_GetKeychain }, { "KCCopyItem", kc_CopyItem },
    { "KCGetDefaultKeychain", kc_GetDefaultKeychain }, { "KCSetDefaultKeychain", kc_SetDefaultKeychain },
    { "KCGetActiveKeychain", kc_GetDefaultKeychain }, { "KCSetActiveKeychain", kc_SetDefaultKeychain },
    { "KCGetStatus", kc_GetStatus }, { "KCGetKeychainName", kc_GetKeychainName },
    { "kcgetkeychainname", kc_GetKeychainName }, { "KCCountKeychains", kc_CountKeychains },
    { "KCGetIndKeychain", kc_GetIndKeychain }, { "KCReleaseKeychain", kc_ReleaseKeychain },
    { "KCMakeKCRefFromFSSpec", kc_MakeKCRef }, { "KCMakeKCRefFromAlias", kc_MakeKCRef },
    { "KCMakeAliasFromKCRef", kc_MakeAliasFromKCRef },
    { "KCUnlock", kc_ByKeychain }, { "KCLock", kc_ByKeychain }, { "KCUnlockKeychain", kc_ByKeychain },
    { "KCLockKeychain", kc_ByKeychain }, { "kcunlock", kc_ByKeychain }, { "kcunlockkeychain", kc_ByKeychain },
    { "KCChangeSettings", kc_ByKeychain },
    { "KCCreateKeychain", kc_CreateKeychain }, { "kccreatekeychain", kc_CreateKeychain },
    { "KCAddNewAsymmetricKeyPair", kc_NoStorage }, { "kcaddnewasymmetrickeypair", kc_NoStorage },
    { "KCFindFirstItem", kc_FindFirstItem }, { "KCFindNextItem", kc_FindNextItem },
    { "KCReleaseSearch", kc_ReleaseSearch }, { "KCReleaseSearchRef", kc_ReleaseSearch },
    { "KCAddCallback", kc_AddCallback }, { "KCRemoveCallback", kc_RemoveCallback },
    { "KCAddAppleSharePassword", kc_AddAppleSharePassword }, { "kcaddapplesharepassword", kc_AddAppleSharePassword },
    { "KCFindAppleSharePassword", kc_FindAppleSharePassword },
    { "kcfindapplesharepassword", kc_FindAppleSharePassword },
    { "KCAddInternetPassword", kc_AddInternetPassword }, { "kcaddinternetpassword", kc_AddInternetPassword },
    { "KCAddInternetPasswordWithPath", kc_AddInternetPasswordWithPath },
    { "kcaddinternetpasswordwithpath", kc_AddInternetPasswordWithPath },
    { "KCFindInternetPassword", kc_FindInternetPassword }, { "kcfindinternetpassword", kc_FindInternetPassword },
    { "KCFindInternetPasswordWithPath", kc_FindInternetPasswordWithPath },
    { "kcfindinternetpasswordwithpath", kc_FindInternetPasswordWithPath },
    { "KCAddGenericPassword", kc_AddGenericPassword }, { "kcaddgenericpassword", kc_AddGenericPassword },
    { "KCFindGenericPassword", kc_FindGenericPassword }, { "kcfindgenericpassword", kc_FindGenericPassword },
    { "KCAddX509Certificate", kc_NoCertificates }, { "KCAddX509CertificateChain", kc_NoCertificates },
    { "KCAddX509CertificateFile", kc_NoCertificates }, { "KCAddX509Certificates", kc_NoCertificates },
    { "KCChooseCertificate", kc_NoCertificates }, { "KCCreateX509CertificateChain", kc_NoCertificates },
    { "KCDisplayTrustResults", kc_NoCertificates }, { "KCDisplayX509Certificate", kc_NoCertificates },
    { "KCDisplayX509CertificateChain", kc_NoCertificates }, { "KCFindOrCreateX509Certificate", kc_NoCertificates },
    { "KCFindX509Certificate", kc_NoCertificates }, { "KCFindX509Certificates", kc_NoCertificates },
    { "KCIsPersonalCertificate", kc_NoCertificates }, { "KCIsRootCertificate", kc_NoCertificates },
    { "KCVerifyX509CertificateGroup", kc_NoPolicies },
    { "KCDispatch", kc_NotAvailable },
    { "init", kc_Init }, { "term", kc_Nothing }, { "__som_check_ev", kc_Nothing }, { "__som_check_new", kc_Nothing },
};
const unsigned os9_keychain_export_count = sizeof os9_keychain_exports / sizeof os9_keychain_exports[0];
const os9_alias os9_keychain_aliases[] = { { "", "" } };
const unsigned os9_keychain_alias_count = 0;
