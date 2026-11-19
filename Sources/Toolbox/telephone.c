/*
 * The Telephone Manager's library (TelephoneLib, Telephones.h), which the
 * System file has carried since System 7.5 for PowerPC programs.  It works
 * through telephone tools ('vbnd' files in the Extensions folder, the
 * GeoPort Telephone tool for one), and Blue Box has none: InitTEL says so
 * with telNoTools, as on a Mac whose System has the library and no tool,
 * no procID names a tool, TELNew makes no terminal, Choose has nothing to
 * offer, and so no terminal, directory number or call handle is ever
 * valid: every call taking one says it is bad (GeoPort Audio, PhoneFloater).
 */
#include <stdint.h>

#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#undef RETURN
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

enum {
    curTELVersion = 3,
    telChooseFailed = -1,
    telNoTools = 8,
    telBadTermErr = -10001,
    telBadDNErr = -10002,
    telBadCAErr = -10003,
    telBadProcID = -10110,
};

/* What the last TELNew failed with (TELGetTelNewErr). */
static int16_t new_error;

static void put32(gaddr p, uint32_t v)
{
    if (p)
        wr32(p, v);
}

static void InitTEL(ppc_cpu *c) { RETURN((int32_t)telNoTools); }
static void TELGetTELVersion(ppc_cpu *c) { RETURN(curTELVersion); }

/* TELGetProcID(name): no tool has that name. */
static void TELGetProcID(ppc_cpu *c) { RETURN((uint32_t)-1); }

/* TELGetToolName(procID, name): none, so an empty name. */
static void TELGetToolName(ppc_cpu *c)
{
    if (R(1))
        wr8(R(1), 0);
    c->pc = c->lr;
}

/* TELNew(procID, flags, refCon, userData): no tool for any procID. */
static void TELNew(ppc_cpu *c)
{
    new_error = telBadProcID;
    RETURN(0);
}

/* TELNewWithResult(procID, flags, refCon, userData, &error). */
static void TELNewWithResult(ppc_cpu *c)
{
    new_error = telBadProcID;
    if (R(4))
        wr16(R(4), (uint16_t)new_error);
    RETURN(0);
}

static void TELGetTelNewErr(ppc_cpu *c) { RETURN((int32_t)new_error); }

/* TELDefault(&config, procID, allocate): no tool to make one. */
static void TELDefault(ppc_cpu *c)
{
    put32(R(0), 0);
    RETURN((int32_t)telBadProcID);
}

/* TELChoose(&hTEL, where, idleProc): no tools to choose among. */
static void TELChoose(ppc_cpu *c) { RETURN((int32_t)telChooseFailed); }

/* TELValidate(hTEL): true, the record isn't valid. */
static void TELValidate(ppc_cpu *c) { RETURN(1); }

/* Calls whose answer is nothing: a nil pointer or handle, false, zero. */
static void ret_zero(ppc_cpu *c) { RETURN(0); }
static void ret_void(ppc_cpu *c) { c->pc = c->lr; }

static void bad_term(ppc_cpu *c) { RETURN((int32_t)telBadTermErr); }
static void bad_dn(ppc_cpu *c) { RETURN((int32_t)telBadDNErr); }
static void bad_ca(ppc_cpu *c) { RETURN((int32_t)telBadCAErr); }

/* The lookups clear the handle they return. */
static void TELDNLookupByIndex(ppc_cpu *c)
{
    put32(R(4), 0);
    RETURN((int32_t)telBadTermErr);
}

static void TELDNLookupByName(ppc_cpu *c)
{
    put32(R(2), 0);
    RETURN((int32_t)telBadTermErr);
}

static void TELCALookup(ppc_cpu *c)
{
    put32(R(3), 0);
    RETURN((int32_t)telBadDNErr);
}

static void TELSetupCall(ppc_cpu *c)
{
    put32(R(1), 0);
    RETURN((int32_t)telBadDNErr);
}

const ppc_export tel_exports[] = {
    { "InitTEL", InitTEL },
    { "TELGetTELVersion", TELGetTELVersion },
    { "TELGetProcID", TELGetProcID },
    { "TELGetToolName", TELGetToolName },
    { "TELNew", TELNew },
    { "TELNewWithResult", TELNewWithResult },
    { "TELGetTelNewErr", TELGetTelNewErr },
    { "TELDefault", TELDefault },
    { "TELChoose", TELChoose },
    { "TELValidate", TELValidate },
    { "TELGetConfig", ret_zero },
    { "TELGetVersion", ret_zero },
    { "TELSetupPreflight", ret_zero },
    { "TELSetupFilter", ret_zero },
    { "TELMenu", ret_zero },
    { "TELCountDNs", ret_zero },
    { "TELCountCAs", ret_zero },
    { "TELSetupSetup", ret_void },
    { "TELSetupItem", ret_void },
    { "TELSetupCleanup", ret_void },
    { "TELSetupXCleanup", ret_void },
    { "TELSetupPostflight", ret_void },
    { "TELIdle", ret_void },
    { "TELActivate", ret_void },
    { "TELResume", ret_void },
    { "TELEvent", ret_void },
    /* A terminal's calls */
    { "TELGetInfo", bad_term },
    { "TELOpenTerm", bad_term },
    { "TELResetTerm", bad_term },
    { "TELCloseTerm", bad_term },
    { "TELTermMsgHand", bad_term },
    { "TELClrTermMsgHand", bad_term },
    { "TELTermEventsSupp", bad_term },
    { "TELSetConfig", bad_term },
    { "TELDispose", bad_term },
    { "TELDNLookupByIndex", TELDNLookupByIndex },
    { "TELDNLookupByName", TELDNLookupByName },
    { "TELCallbackClear", bad_term },
    { "TELOtherFeatureList", bad_term },
    { "TELOtherFeatureImplement", bad_term },
    { "TELToolFunctions", bad_term },
    { "TELOtherFunction", bad_term },
    { "TELGetHooksw", bad_term },
    { "TELSetHooksw", bad_term },
    { "TELGetVolume", bad_term },
    { "TELSetVolume", bad_term },
    { "TELAlert", bad_term },
    { "TELGetDisplay", bad_term },
    { "TELSetDisplay", bad_term },
    { "TELIntlToEnglish", bad_term },
    { "TELEnglishToIntl", bad_term },
    { "TELGetHSSoundInput", bad_term },
    { "TELDisposeHSSoundInput", bad_term },
    { "TELGetHSSoundOutput", bad_term },
    { "TELDisposeHSSoundOutput", bad_term },
    { "TELSetIndHSConnect", bad_term },
    { "TELGetIndHSConnect", bad_term },
    { "TELHSSetDTMF", bad_term },
    { "TELHSGetDTMF", bad_term },
    /* A directory number's */
    { "TELDNSelect", bad_dn },
    { "TELDNDispose", bad_dn },
    { "TELGetDNInfo", bad_dn },
    { "TELGetDNFlags", bad_dn },
    { "TELDNMsgHand", bad_dn },
    { "TELClrDNMsgHand", bad_dn },
    { "TELDNEventsSupp", bad_dn },
    { "TELCALookup", TELCALookup },
    { "TELCAMsgHand", bad_dn },
    { "TELClrCAMsgHand", bad_dn },
    { "TELCAEventsSupp", bad_dn },
    { "TELSetupCall", TELSetupCall },
    { "TELForwardSet", bad_dn },
    { "TELForwardClear", bad_dn },
    { "TELDNDSet", bad_dn },
    { "TELDNDClear", bad_dn },
    { "TELGetDNSoundInput", bad_dn },
    { "TELDisposeDNSoundInput", bad_dn },
    { "TELGetDNSoundOutput", bad_dn },
    { "TELDisposeDNSoundOutput", bad_dn },
    { "TELDNSetAutoAnswer", bad_dn },
    { "TELDNTollSaverControl", bad_dn },
    { "TELDNSetDTMF", bad_dn },
    { "TELDNGetDTMF", bad_dn },
    { "TELGetDNStatus", bad_dn },
    { "TELGetDNProgressDet", bad_dn },
    { "TELSetDNProgressDet", bad_dn },
    /* A call's */
    { "TELCADispose", bad_ca },
    { "TELGetCAState", bad_ca },
    { "TELGetCAFlags", bad_ca },
    { "TELGetCAInfo", bad_ca },
    { "TELConnect", bad_ca },
    { "TELDialDigits", bad_ca },
    { "TELAcceptCall", bad_ca },
    { "TELRejectCall", bad_ca },
    { "TELDeflectCall", bad_ca },
    { "TELAnswerCall", bad_ca },
    { "TELDrop", bad_ca },
    { "TELHold", bad_ca },
    { "TELRetrieve", bad_ca },
    { "TELConferencePrep", bad_ca },
    { "TELConferenceEstablish", bad_ca },
    { "TELConferenceSplit", bad_ca },
    { "TELTransferPrep", bad_ca },
    { "TELTransferEstablish", bad_ca },
    { "TELTransferBlind", bad_ca },
    { "TELCallbackSet", bad_ca },
    { "TELCallbackNow", bad_ca },
    { "TELCallPickup", bad_ca },
    { "TELParkCall", bad_ca },
    { "TELRetrieveParkedCall", bad_ca },
    { "TELVoiceMailAccess", bad_ca },
    { "TELPaging", bad_ca },
    { "TELIntercom", bad_ca },
    { "TELCAVoiceDetect", bad_ca },
    { "TELCASilenceDetect", bad_ca },
};
const unsigned tel_export_count = sizeof tel_exports / sizeof tel_exports[0];
