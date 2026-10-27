/*
 * FileSigningLib (FileSigning.h): Mac OS 9's signed files, on a Mac that
 * has no keychain to sign with or certificates to verify against.  A
 * file's signatures are its 'sig#' resources, so whether it has one, which
 * to verify and loading one are answered from the file; removing them
 * removes the resources.  Signing needs a certificate from a keychain and
 * verifying needs the certificate module, neither of which is here
 * (errKCNoCertificateModule, as KeychainLib's calls answer).  PowerPC
 * only.  MacInstall asks whether its archives are signed as soon as it
 * finds the Keychain Manager.
 */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "filemgr_internal.h"
#include "forks.h"
#include "os9svc.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(int32_t)(v); c->pc = c->lr; return; } while (0)

enum {
    errSecNoSignatureFound = -13836,
    errKCNoCertificateModule = -25313,
    fnfErrSig = -43,
};
#define SIG_TYPE 0x73696723u /* 'sig#' */

static gaddr progress_proc, progress_context;

void os9svc_filesign_reset(void) { progress_proc = progress_context = 0; }

/* The IDs of a file's 'sig#' resources, in its map's order (up to max);
 * returns how many, or -1 when the file isn't there. */
static int signatures(gaddr spec, int16_t *ids, int max, gaddr *first_data, uint32_t *first_len)
{
    char path[PATH_MAX], err[128];
    if (!spec || !fm_host_path((int16_t)rd16(spec), (int32_t)rd32(spec + 2), spec + 6, true, path, sizeof path))
        return -1;
    mac_file mf;
    if (!appledouble_read(path, &mf, err, sizeof err))
        return -1;
    int n = 0;
    rsrc_index idx;
    if (mf.rsrc && rsrc_parse(mf.rsrc, mf.rsrc_len, &idx, err, sizeof err)) {
        for (size_t i = 0; i < idx.count; i++) {
            if (idx.entries[i].type != SIG_TYPE)
                continue;
            if (n == 0 && first_data) {
                *first_len = idx.entries[i].data_len;
                *first_data = tb_NewHandle((int32_t)*first_len, false, false);
                if (*first_data && rd32(*first_data))
                    gput(rd32(*first_data), rsrc_data(mf.rsrc, &idx, &idx.entries[i]), *first_len);
            }
            if (n < max)
                ids[n] = idx.entries[i].id;
            n++;
        }
        rsrc_index_free(&idx);
    }
    mac_file_free(&mf);
    return n;
}

/* SecMacHasSignature(theFile, signatureType): a Boolean. */
static void fs_HasSignature(ppc_cpu *c) { RETURN(signatures(R(0), NULL, 0, NULL, NULL) > 0); }

/* SecMacFindSignatureToVerify(theFile, signatureType, *signatureToVerify) */
static void fs_FindSignatureToVerify(ppc_cpu *c)
{
    int16_t id;
    int n = signatures(R(0), &id, 1, NULL, NULL);
    if (n < 0)
        RETURN(fnfErrSig);
    if (n == 0)
        RETURN(errSecNoSignatureFound);
    if (R(2))
        wr16(R(2), (uint16_t)id);
    RETURN(noErr);
}

/* SecMacLoadSigPound(theFile, *resHandle): the signature resource, in a
 * handle of the caller's. */
static void fs_LoadSigPound(ppc_cpu *c)
{
    gaddr h = 0;
    uint32_t len = 0;
    int n = signatures(R(0), NULL, 0, &h, &len);
    if (n < 0)
        RETURN(fnfErrSig);
    if (n == 0)
        RETURN(errSecNoSignatureFound);
    if (!h)
        RETURN(memFullErr);
    if (R(1))
        wr32(R(1), h);
    RETURN(noErr);
}

/* Remove the 'sig#' resources with these IDs (all of them when all). */
static int16_t remove_signatures(gaddr spec, bool all, int16_t which)
{
    int16_t ids[64];
    int n = signatures(spec, ids, 64, NULL, NULL);
    if (n < 0)
        return fnfErrSig;
    bool any = false;
    for (int i = 0; i < n && i < 64; i++)
        any |= all || ids[i] == which;
    if (!any)
        return errSecNoSignatureFound;
    char path[PATH_MAX];
    fm_host_path((int16_t)rd16(spec), (int32_t)rd32(spec + 2), spec + 6, true, path, sizeof path);
    bool in_chain = false;
    int16_t was_open = 0;
    bool open_already = rm_path_open(path, &in_chain, &was_open);
    int16_t cur = tb_CurResFile();
    int16_t rn = open_already ? was_open : tb_FSpOpenResFile(spec, 3 /* fsRdWrPerm */);
    if (rn == -1)
        return tb_ResError();
    tb_UseResFile(rn);
    for (int i = 0; i < n && i < 64; i++) {
        if (!all && ids[i] != which)
            continue;
        gaddr h = tb_Get1Resource((int32_t)SIG_TYPE, ids[i]);
        if (h)
            tb_RemoveResource(h);
    }
    tb_UpdateResFile(rn);
    int16_t err = tb_ResError();
    if (!open_already)
        tb_CloseResFile(rn);
    tb_UseResFile(cur);
    return err;
}

/* SecMacRemoveSignature(theFile, signatureType, signatureToRemove) */
static void fs_RemoveSignature(ppc_cpu *c) { RETURN(remove_signatures(R(0), false, (int16_t)R(2))); }

/* SecMacRemoveAllSignatures(theFile, signatureType) */
static void fs_RemoveAllSignatures(ppc_cpu *c) { RETURN(remove_signatures(R(0), true, 0)); }

/* Verifying: a file without a signature says so; one with a signature
 * would need the certificate module. */
static void fs_VerifyFile(ppc_cpu *c)
{
    int n = signatures(R(0), NULL, 0, NULL, NULL);
    RETURN(n < 0 ? fnfErrSig : n == 0 ? errSecNoSignatureFound : errKCNoCertificateModule);
}

/* Signing needs a certificate from a keychain. */
static void fs_SignFile(ppc_cpu *c) { RETURN(errKCNoCertificateModule); }

/* SecSetProgressCallback(callbackProc, userContext) */
static void fs_SetProgressCallback(ppc_cpu *c)
{
    progress_proc = R(0);
    progress_context = R(1);
    RETURN(noErr);
}

static void fs_RemoveProgressCallback(ppc_cpu *c)
{
    if (R(0) == progress_proc)
        progress_proc = progress_context = 0;
    RETURN(noErr);
}

/* DefaultSecProgressCallbackProc(info, userContext): keep going. */
static void fs_DefaultProgress(ppc_cpu *c) { RETURN(noErr); }

/* SecMacGetDefaultPolicyOIDs(trustPolicy): no policy module, no OIDs. */
static void fs_GetDefaultPolicyOIDs(ppc_cpu *c) { RETURN(0); }

/* SecMacDisplaySigner(inputErr, signer, alwaysShowUI, theFile): there is
 * no signer to show; the verification's result stands. */
static void fs_DisplaySigner(ppc_cpu *c) { RETURN((int32_t)R(0)); }

const ppc_export os9_filesign_exports[] = {
    { "SecMacHasSignature", fs_HasSignature },
    { "SecMacFindSignatureToVerify", fs_FindSignatureToVerify },
    { "SecMacLoadSigPound", fs_LoadSigPound },
    { "SecMacRemoveSignature", fs_RemoveSignature },
    { "SecMacRemoveAllSignatures", fs_RemoveAllSignatures },
    { "SecMacVerifyFile", fs_VerifyFile },
    { "SecMacVerifyFileSimple", fs_VerifyFile },
    { "SecMacSignFile", fs_SignFile },
    { "SecMacSignFileSimple", fs_SignFile },
    { "SecSetProgressCallback", fs_SetProgressCallback },
    { "SecRemoveProgressCallback", fs_RemoveProgressCallback },
    { "DefaultSecProgressCallbackProc", fs_DefaultProgress },
    { "SecMacGetDefaultPolicyOIDs", fs_GetDefaultPolicyOIDs },
    { "SecMacDisplaySigner", fs_DisplaySigner },
};
const unsigned os9_filesign_export_count = sizeof os9_filesign_exports / sizeof os9_filesign_exports[0];
const os9_alias os9_filesign_aliases[] = { { "", "" } };
const unsigned os9_filesign_alias_count = 0;
