/*
 * The Translation Manager (Macintosh Easy Open; Translation.h, $ABFC):
 * which file types an application opens by itself, whether a document can
 * be opened, and how a document in one format becomes another.  The
 * translators are components ("translation extensions") that a Mac had
 * installed; none is here, so the only way to open a document is the
 * application's own, and translating a file or a scrap needs a path that
 * does not exist: noTranslationPathErr, as on a Mac with Easy Open and no
 * translator for the pair.  A scrap whose source can give the format asked
 * for needs none, and is simply fetched.
 */
#include <stdio.h>
#include <string.h>

#include "carbonrest.h"

enum {
    invalidTranslationPathErr = -3025,
    noTranslationPathErr = -3030,
    badTranslationSpecErr = -3031,
    afpItemNotFound = -5012,
};
enum { domCannot = 0, domNative = 1, domTranslateFirst = 2, domWildcard = 3 };
enum { kMaxTypes = 64, kFileTranslationSpecSize = 48 };
#define T(s) fourcc(s)

/* The application with a signature: an FSSpec for it, found in the
 * Desktop Database of a volume (the hint's, else the startup volume's). */
static int16_t find_app(int16_t vref_hint, uint32_t signature, gaddr spec)
{
    gaddr pb = tb_NewPtr(DTPBRec_SIZEOF, false, true), name = tb_NewPtr(64, false, true);
    if (!pb || !name) {
        if (pb)
            tb_DisposePtr(pb);
        if (name)
            tb_DisposePtr(name);
        return memFullErr;
    }
    int16_t err = afpItemNotFound;
    int16_t vols[2] = { vref_hint, 0 };
    for (int i = 0; i < 2 && err; i++) {
        gmemset(pb, 0, DTPBRec_SIZEOF);
        wr16(pb + DTPBRec_ioVRefNum_off, (uint16_t)vols[i]);
        wr32(pb + DTPBRec_ioNamePtr_off, 0);
        if (tb_PBDTGetPath(pb) != noErr)
            continue;
        wr32(pb + DTPBRec_ioNamePtr_off, name);
        wr32(pb + DTPBRec_ioFileCreator_off, signature);
        wr16(pb + DTPBRec_ioIndex_off, 0);
        if (tb_PBDTGetAPPL(pb, false) == noErr)
            err = tb_FSMakeFSSpec((int16_t)rd16(pb + DTPBRec_ioVRefNum_off), (int32_t)rd32(pb + DTPBRec_ioAPPLParID_off),
                                  name, spec);
    }
    tb_DisposePtr(pb);
    tb_DisposePtr(name);
    return err;
}

/* The file types an application's FREF resources name (not applications). */
static int app_types(gaddr spec, uint32_t *types, int max)
{
    int16_t before = tb_CurResFile();
    int16_t rn = tb_FSpOpenResFile(spec, 1);
    int n = 0;
    if (rn >= 0) {
        int16_t count = tb_Count1Resources((int32_t)T("FREF"));
        for (int16_t i = 1; i <= count && n < max; i++) {
            gaddr h = tb_Get1IndResource((int32_t)T("FREF"), i);
            if (h)
                tb_LoadResource(h);
            if (!h || !rd32(h) || tb_GetHandleSize(h) < 4)
                continue;
            uint32_t type = rd32(rd32(h));
            if (type == T("APPL"))
                continue;
            bool seen = false;
            for (int k = 0; k < n; k++)
                seen = seen || types[k] == type;
            if (!seen)
                types[n++] = type;
        }
        tb_CloseResFile(rn);
        if (before)
            tb_UseResFile(before);
    }
    set_res_err(noErr);
    return n;
}

/* GetFileTypesThatAppCanNativelyOpen(appVRefNumHint, appSignature, nativeTypes):
 * the types of its FREFs in a TypesBlock (64 OSTypes, the rest zero); an
 * application not found is afpItemNotFound. */
int16_t tb_GetFileTypesThatAppCanNativelyOpen(int16_t appVRefNumHint, int32_t appSignature, gaddr nativeTypes)
{
    if (!nativeTypes)
        return paramErr;
    gmemset(nativeTypes, 0, kMaxTypes * 4);
    gaddr spec = tb_NewPtr(FSSpec_SIZEOF, true, true);
    if (!spec)
        return memFullErr;
    int16_t err = find_app(appVRefNumHint, (uint32_t)appSignature, spec);
    if (!err) {
        uint32_t types[kMaxTypes];
        int n = app_types(spec, types, kMaxTypes);
        for (int i = 0; i < n; i++)
            wr32(nativeTypes + 4u * (uint32_t)i, types[i]);
    }
    tb_DisposePtr(spec);
    return err;
}

/* ExtendFileTypeList(original, n, extended, *numberExtended): the types
 * with those that translators make into them: none are installed. */
int16_t tb_ExtendFileTypeList(gaddr originalTypeList, int16_t numberOriginalTypes, gaddr extendedTypeList,
                              gaddr numberExtendedTypes)
{
    if (numberOriginalTypes < 0 || (numberOriginalTypes && !originalTypeList) || !extendedTypeList)
        return paramErr;
    for (int16_t i = 0; i < numberOriginalTypes; i++)
        wr32(extendedTypeList + 4u * (uint32_t)i, rd32(originalTypeList + 4u * (uint32_t)i));
    if (numberExtendedTypes)
        wr16(numberExtendedTypes, (uint16_t)numberOriginalTypes);
    return noErr;
}

static void clear_spec(gaddr spec)
{
    if (spec)
        gmemset(spec, 0, kFileTranslationSpecSize);
}

/* CanDocBeOpened(document, appVRefNumHint, appSignature, nativeTypes,
 * onlyNative, *howToOpen, *howToTranslate): domNative when the application
 * (its native types: those given, else its FREFs') opens the document's
 * type; domCannot when nothing else could. */
int16_t tb_CanDocBeOpened(gaddr targetDocument, int16_t appVRefNumHint, int32_t appSignature, gaddr nativeTypes,
                          int8_t onlyNative, gaddr howToOpen, gaddr howToTranslate)
{
    if (!targetDocument || !howToOpen)
        return paramErr;
    clear_spec(howToTranslate);
    gaddr info = tb_NewPtr(16, false, true);
    if (!info)
        return memFullErr;
    int16_t err = tb_FSpGetFInfo(targetDocument, info);
    uint32_t type = rd32(info);
    tb_DisposePtr(info);
    if (err)
        return err;
    uint32_t types[kMaxTypes];
    int n = 0;
    if (nativeTypes) {
        for (; n < kMaxTypes && rd32(nativeTypes + 4u * (uint32_t)n); n++)
            types[n] = rd32(nativeTypes + 4u * (uint32_t)n);
    } else {
        gaddr block = tb_NewPtr(kMaxTypes * 4, true, true);
        if (!block)
            return memFullErr;
        if (!tb_GetFileTypesThatAppCanNativelyOpen(appVRefNumHint, appSignature, block))
            for (; n < kMaxTypes && rd32(block + 4u * (uint32_t)n); n++)
                types[n] = rd32(block + 4u * (uint32_t)n);
        tb_DisposePtr(block);
    }
    wr16(howToOpen, domCannot);
    for (int i = 0; i < n; i++)
        if (types[i] == type)
            wr16(howToOpen, domNative);
    return noErr;
}

/* GetFileTranslationPaths(source, dstDocType, max, results): the number of
 * ways to translate the document to the type; no translators, none. */
int16_t tb_GetFileTranslationPaths(gaddr srcDocument, int32_t dstDocType, uint16_t maxResultCount, gaddr resultBuffer)
{
    return 0;
}

/* GetPathFromTranslationDialog(document, application, typeList, *howToOpen,
 * *howToTranslate): the choice a dialog would offer; with nothing to
 * choose from there is no path. */
int16_t tb_GetPathFromTranslationDialog(gaddr theDocument, gaddr theApplication, gaddr typeList, gaddr howToOpen,
                                        gaddr howToTranslate)
{
    if (howToOpen)
        wr16(howToOpen, domCannot);
    clear_spec(howToTranslate);
    return noTranslationPathErr;
}

/* TranslateFile(source, destination, howToTranslate): the translator named
 * by the spec isn't installed. */
int16_t tb_TranslateFile(gaddr sourceDocument, gaddr destinationDocument, gaddr howToTranslate)
{
    if (!sourceDocument || !destinationDocument || !howToTranslate)
        return paramErr;
    return badTranslationSpecErr;
}

/* GetTranslationExtensionName(spec, name): of the extension that does it. */
int16_t tb_GetTranslationExtensionName(gaddr translationMethod, gaddr extensionName)
{
    if (extensionName)
        wr8(extensionName, 0);
    return translationMethod ? badTranslationSpecErr : paramErr;
}

/* GetDocumentKindString(vRefNum, docType, docCreator, kindString): as the
 * Finder says it: "application program", "<application> document" for a
 * creator that has an application, else "document". */
int16_t tb_GetDocumentKindString(int16_t docVRefNum, int32_t docType, int32_t docCreator, gaddr kindString)
{
    if (!kindString)
        return paramErr;
    char kind[64] = "document";
    if ((uint32_t)docType == T("APPL")) {
        snprintf(kind, sizeof kind, "application program");
    } else {
        gaddr spec = tb_NewPtr(FSSpec_SIZEOF, true, true);
        if (spec && find_app(docVRefNum, (uint32_t)docCreator, spec) == noErr) {
            char name[64];
            rest_pstr_get(spec + 6, name, sizeof name - 10);
            snprintf(kind, sizeof kind, "%s document", name);
        }
        if (spec)
            tb_DisposePtr(spec);
    }
    rest_pstr_put(kindString, kind, 62);
    return noErr;
}

/* TranslateScrap(sourceDataGetter, refCon, destinationFormat, destinationData,
 * progressDialogID): the destination format asked of the getter, which
 * (OSErr (ScrapType, Handle, refCon)) fills the handle if it has it;
 * otherwise there is no path. */
int16_t tb_TranslateScrap(gaddr sourceDataGetter, gaddr sourceDataGetterRefCon, int32_t destinationFormat,
                          gaddr destinationData, int16_t progressDialogID)
{
    if (!sourceDataGetter || !destinationData)
        return paramErr;
    uint32_t args[3] = { (uint32_t)destinationFormat, destinationData, sourceDataGetterRefCon };
    int16_t err = (int16_t)aem_call_proc(sourceDataGetter, 0x00000FE0, args, 3);
    return err ? noTranslationPathErr : noErr;
}
