/*
 * The low-memory accessors (LMGetX / LMSetX) that LowMem.h and Mac OS 8.1's
 * InterfaceLib and PrivateInterfaceLib export beyond the ones gen.py makes
 * from the low-memory table: one row for each, the address and what the
 * routine does there (lowmem_acc.c runs a row).  The rows were read from the
 * routines of Mac OS 8.1 itself, which are one or a few instructions each.
 * lmt_N below is the entry of row N: a PowerPC export cannot tell the
 * routine which name it was bound under, so each row has its own.
 */
#include "ppc.h"
#include "ppc_bridge.h"
#include "lowmem_acc.h"

static const lm_entry lm_table[] = {
    { "LMGetADBBase", LM_GETU, 4, 0, 0x0CF8 },
    { "LMGetAGBHandle", LM_GETU, 4, 0, 0x0D1C },
    { "LMGetASCBase", LM_GETU, 4, 0, 0x0CC0 },
    { "LMGetATalkHk1", LM_GETU, 4, 0, 0x0B14 },
    { "LMGetAddrMapFlags", LM_GETU, 4, 0, 0x0DD0 },
    { "LMGetAddressError", LM_GETU, 4, 0, 0x000C },
    { "LMGetAlarmState", LM_GETU, 1, 0, 0x021F },
    { "LMGetApp2Packs", LM_IGETU, 4, 4, 0x0BC8 },
    { "LMGetAt2MenuBottom", LM_GETS, 2, 0, 0x0B28 },
    { "LMGetAutoInt1", LM_GETU, 4, 0, 0x0064 },
    { "LMGetAutoInt2", LM_GETU, 4, 0, 0x0068 },
    { "LMGetAutoInt3", LM_GETU, 4, 0, 0x006C },
    { "LMGetAutoInt4", LM_GETU, 4, 0, 0x0070 },
    { "LMGetAutoInt5", LM_GETU, 4, 0, 0x0074 },
    { "LMGetAutoInt6", LM_GETU, 4, 0, 0x0078 },
    { "LMGetAutoInt7", LM_GETU, 4, 0, 0x007C },
    { "LMGetBackgrounderVars", LM_GETU, 4, 0, 0x0B64 },
    { "LMGetBootGlobPtr", LM_GETU, 4, 0, 0x0DDC },
    { "LMGetBootMask", LM_GETS, 2, 0, 0x0B0E },
    { "LMGetBootTmp8", LM_ADDR, 8, 0, 0x0B36 },
    { "LMGetBoxFlag", LM_GETU, 1, 0, 0x0CB3 },
    { "LMGetBrianBits", LM_GETS, 2, 0, 0x0B20 },
    { "LMGetBtDskRfn", LM_GETS, 2, 0, 0x0B34 },
    { "LMGetBusError", LM_GETU, 4, 0, 0x0008 },
    { "LMGetCHKError", LM_GETU, 4, 0, 0x0018 },
    { "LMGetCMVector", LM_GETU, 4, 0, 0x0BB4 },
    { "LMGetCQDGlobals", LM_GETU, 4, 0, 0x0CCC },
    { "LMGetCarlByte", LM_GETU, 1, 0, 0x0B72 },
    { "LMGetChooserBits", LM_GETU, 1, 0, 0x0946 },
    { "LMGetChunkyDepth", LM_GETS, 2, 0, 0x0D60 },
    { "LMGetCkdDB", LM_GETS, 2, 0, 0x0340 },
    { "LMGetCloseOrnHook", LM_GETU, 4, 0, 0x0A88 },
    { "LMGetCoProcessor", LM_GETU, 4, 0, 0x0034 },
    { "LMGetColLines", LM_GETS, 2, 0, 0x0C22 },
    { "LMGetCoreEditVars", LM_ADDR, 12, 0, 0x0954 },
    { "LMGetCrsrAddr", LM_GETU, 4, 0, 0x0888 },
    { "LMGetCrsrBase", LM_GETU, 4, 0, 0x0898 },
    { "LMGetCrsrCouple", LM_GETU, 1, 0, 0x08CF },
    { "LMGetCrsrDevice", LM_GETU, 4, 0, 0x089C },
    { "LMGetCrsrNew", LM_GETU, 1, 0, 0x08CE },
    { "LMGetCrsrObscure", LM_GETU, 1, 0, 0x08D2 },
    { "LMGetCrsrPtr", LM_GETU, 4, 0, 0x0D62 },
    { "LMGetCrsrRect", LM_OUT, 8, 0, 0x083C },
    { "LMGetCrsrRow", LM_GETS, 2, 0, 0x08AC },
    { "LMGetCrsrScale", LM_GETU, 1, 0, 0x08D3 },
    { "LMGetCurDeKind", LM_GETS, 2, 0, 0x0A22 },
    { "LMGetCurDragAction", LM_GETU, 4, 0, 0x0A46 },
    { "LMGetCurFMInput", LM_OUT, 16, 0, 0x0988 },
    { "LMGetCurLayer", LM_GETU, 4, 0, 0x0A90 },
    { "LMGetDSCtrAdj", LM_GETU, 4, 0, 0x0DA8 },
    { "LMGetDSDrawProc", LM_GETU, 4, 0, 0x0334 },
    { "LMGetDSPGlobals", LM_GETU, 4, 0, 0x1FC4 },
    { "LMGetDSWndUpdate", LM_GETU, 1, 0, 0x015D },
    { "LMGetDeskPatEnable", LM_GETS, 2, 0, 0x0CE8 },
    { "LMGetDeskPort", LM_GETU, 4, 0, 0x09E2 },
    { "LMGetDiskVars", LM_ADDR, 62, 0, 0x0222 },
    { "LMGetDivideByZero", LM_GETU, 4, 0, 0x0014 },
    { "LMGetDockingGlobals", LM_GETU, 4, 0, 0x1FF8 },
    { "LMGetDrMstrBlk", LM_GETS, 2, 0, 0x034C },
    { "LMGetDragFlag", LM_GETS, 2, 0, 0x0A44 },
    { "LMGetDrawCrsrVector", LM_GETU, 4, 0, 0x1FB8 },
    { "LMGetDskErr", LM_GETS, 2, 0, 0x0142 },
    { "LMGetDskRtnAdr", LM_GETU, 4, 0, 0x0124 },
    { "LMGetDskSwtchHook", LM_GETU, 4, 0, 0x03EA },
    { "LMGetDskVerify", LM_GETU, 1, 0, 0x012C },
    { "LMGetEgretGlobals", LM_GETU, 4, 0, 0x0DE0 },
    { "LMGetEndSRTPtr", LM_GETU, 4, 0, 0x0DB4 },
    { "LMGetErCode", LM_GETS, 2, 0, 0x03A2 },
    { "LMGetEraseCrsrVector", LM_GETU, 4, 0, 0x1FBC },
    { "LMGetEvtBufCnt", LM_GETS, 2, 0, 0x0154 },
    { "LMGetExpandMem", LM_GETU, 4, 0, 0x02B6 },
    { "LMGetExtFSHook", LM_GETU, 4, 0, 0x03E6 },
    { "LMGetExtraPrint", LM_ADDR, 13, 0, 0x0947 },
    { "LMGetFDevDisable", LM_GETU, 1, 0, 0x0BB3 },
    { "LMGetFLckUnlck", LM_GETU, 1, 0, 0x0348 },
    { "LMGetFMDefaultSize", LM_GETU, 1, 0, 0x0987 },
    { "LMGetFMDotsPerInch", LM_OUT, 4, 0, 0x09B2 },
    { "LMGetFMExist", LM_GETU, 1, 0, 0x0D42 },
    { "LMGetFMStyleTab", LM_ADDR, 24, 0, 0x09B6 },
    { "LMGetFMSynth", LM_GETU, 4, 0, 0x0C1C },
    { "LMGetFMgrOutRec", LM_OUT, 26, 0, 0x0998 },
    { "LMGetFONDID", LM_GETS, 2, 0, 0x0BC6 },
    { "LMGetFP040Vects", LM_IGETU, 4, 4, 0x1FC8 },
    { "LMGetFP68881", LM_IGETU, 4, 4, 0x00C0 },
    { "LMGetFPState", LM_ADDR, 6, 0, 0x0A4A },
    { "LMGetFSMVars", LM_GETU, 4, 0, 0x0BB8 },
    { "LMGetFSQueueHook", LM_GETU, 4, 0, 0x03E2 },
    { "LMGetFSTemp4", LM_GETU, 4, 0, 0x03DE },
    { "LMGetFSTemp8", LM_ADDR, 8, 0, 0x03D6 },
    { "LMGetFScaleHFact", LM_GETU, 4, 0, 0x0BF6 },
    { "LMGetFScaleVFact", LM_GETU, 4, 0, 0x0BFA },
    { "LMGetFileShareVars", LM_GETU, 4, 0, 0x0B50 },
    { "LMGetFinder", LM_GETU, 1, 0, 0x0261 },
    { "LMGetFlushOnly", LM_GETU, 1, 0, 0x0346 },
    { "LMGetFondState", LM_GETU, 1, 0, 0x0903 },
    { "LMGetFontFlag", LM_GETU, 1, 0, 0x015E },
    { "LMGetFormatError", LM_GETU, 4, 0, 0x0038 },
    { "LMGetFourDHack", LM_GETU, 4, 0, 0x0B1C },
    { "LMGetFrcSync", LM_GETU, 1, 0, 0x0349 },
    { "LMGetGZRootPtr", LM_GETU, 4, 0, 0x032C },
    { "LMGetHFSFlags", LM_GETU, 1, 0, 0x0376 },
    { "LMGetHFSStkTop", LM_GETU, 4, 0, 0x036A },
    { "LMGetHSCHndl", LM_GETU, 4, 0, 0x0B84 },
    { "LMGetHiHeapMark", LM_GETU, 4, 0, 0x0BAE },
    { "LMGetHpChk", LM_GETU, 4, 0, 0x0316 },
    { "LMGetIOPMgrVars", LM_GETU, 4, 0, 0x0C28 },
    { "LMGetIconBitmap", LM_ADDR, 14, 0, 0x0A0E },
    { "LMGetIconTLAddr", LM_GETU, 4, 0, 0x0DAC },
    { "LMGetIllegalInstruction", LM_GETU, 4, 0, 0x0010 },
    { "LMGetIntFlag", LM_GETU, 1, 0, 0x015F },
    { "LMGetJAllocCrsr", LM_GETU, 4, 0, 0x088C },
    { "LMGetJCheckLoad", LM_GETU, 4, 0, 0x07F0 },
    { "LMGetJDCDReset", LM_GETU, 4, 0, 0x0B48 },
    { "LMGetJDiskSel", LM_GETU, 4, 0, 0x0B40 },
    { "LMGetJDtrmV2", LM_GETU, 4, 0, 0x06C4 },
    { "LMGetJDtrmV3", LM_GETU, 4, 0, 0x07BC },
    { "LMGetJFileOpen", LM_GETU, 4, 0, 0x0784 },
    { "LMGetJFindDrive", LM_GETU, 4, 0, 0x06C8 },
    { "LMGetJFlushCache", LM_GETU, 4, 0, 0x06F4 },
    { "LMGetJKybdTask", LM_GETU, 4, 0, 0x021A },
    { "LMGetJOpcodeProc", LM_GETU, 4, 0, 0x0894 },
    { "LMGetJSendCmd", LM_GETU, 4, 0, 0x0B44 },
    { "LMGetJSetCCrsr", LM_GETU, 4, 0, 0x0890 },
    { "LMGetJShell", LM_GETS, 2, 0, 0x0212 },
    { "LMGetJSwapMMU", LM_GETU, 4, 0, 0x0DBC },
    { "LMGetKbdVars", LM_GETS, 2, 0, 0x0216 },
    { "LMGetKeyLast", LM_GETS, 2, 0, 0x0184 },
    { "LMGetKeyMVars", LM_GETS, 2, 0, 0x0B04 },
    { "LMGetKeyRepTime", LM_GETU, 4, 0, 0x018A },
    { "LMGetLastBG", LM_GETU, 4, 0, 0x0D3A },
    { "LMGetLastBinPat", LM_OUT, 8, 0, 0x0CE0 },
    { "LMGetLastDepth", LM_GETS, 2, 0, 0x0D40 },
    { "LMGetLastFore", LM_GETU, 4, 0, 0x0D36 },
    { "LMGetLastMode", LM_GETS, 2, 0, 0x0D3E },
    { "LMGetLastTxGDevice", LM_GETU, 4, 0, 0x0DC4 },
    { "LMGetLaunchFlag", LM_GETU, 1, 0, 0x0902 },
    { "LMGetLayerPalette", LM_GETU, 4, 0, 0x0DCC },
    { "LMGetLine1010", LM_GETU, 4, 0, 0x0028 },
    { "LMGetLine1111", LM_GETU, 4, 0, 0x002C },
    { "LMGetLoadFiller", LM_GETU, 4, 0, 0x090C },
    { "LMGetLoadTrap", LM_GETU, 1, 0, 0x012D },
    { "LMGetLoaderPBlock", LM_ADDR, 10, 0, 0x093A },
    { "LMGetLockMemCt", LM_GETU, 4, 0, 0x1FF4 },
    { "LMGetMAErrProc", LM_GETU, 4, 0, 0x0BE8 },
    { "LMGetMASuperTab", LM_GETU, 4, 0, 0x0BEC },
    { "LMGetMBFlash", LM_GETU, 4, 0, 0x0D5C },
    { "LMGetMBProcHndl", LM_GETU, 4, 0, 0x0D54 },
    { "LMGetMMDefFlags", LM_GETS, 2, 0, 0x0326 },
    { "LMGetMMFlags", LM_GETU, 1, 0, 0x1EFC },
    { "LMGetMMU24Tbl", LM_GETU, 4, 0, 0x0CB4 },
    { "LMGetMMUFlags", LM_GETU, 1, 0, 0x0CB0 },
    { "LMGetMMUTblSize", LM_GETU, 4, 0, 0x0CB8 },
    { "LMGetMacJmp", LM_GETU, 4, 0, 0x0120 },
    { "LMGetMacJmpFlag", LM_GETU, 1, 0, 0x0BFF },
    { "LMGetMacZero", LM_GETU, 4, 0, 0x0000 },
    { "LMGetMacsBugPC", LM_GETU, 4, 0, 0x0C70 },
    { "LMGetMacsBugSP", LM_GETU, 4, 0, 0x0C6C },
    { "LMGetMacsBugSR", LM_GETU, 4, 0, 0x0C74 },
    { "LMGetMaxDB", LM_GETS, 2, 0, 0x0344 },
    { "LMGetMem2Vectors24", LM_IGETU, 4, 4, 0x1EE0 },
    { "LMGetMem2Vectors32", LM_IGETU, 4, 4, 0x1FE0 },
    { "LMGetMemVectors24", LM_IGETU, 4, 4, 0x1E00 },
    { "LMGetMemVectors32", LM_IGETU, 4, 4, 0x1F00 },
    { "LMGetMenuLayer", LM_GETU, 4, 0, 0x0B68 },
    { "LMGetMickeyBytes", LM_GETU, 4, 0, 0x0D6A },
    { "LMGetMimeGlobs", LM_GETU, 4, 0, 0x0BF0 },
    { "LMGetMmInOK", LM_GETU, 1, 0, 0x012E },
    { "LMGetMonkeyLives", LM_GETS, 2, 0, 0x0100 },
    { "LMGetMouse", LM_OUT, 4, 0, 0x0830 },
    { "LMGetMouseMask", LM_OUT, 4, 0, 0x08D6 },
    { "LMGetMouseOffset", LM_OUT, 4, 0, 0x08DA },
    { "LMGetNMIFlag", LM_GETU, 1, 0, 0x0C2C },
    { "LMGetNMQHdr", LM_GETU, 4, 0, 0x0B60 },
    { "LMGetNTSC", LM_GETU, 1, 0, 0x0B3E },
    { "LMGetNewMount", LM_GETS, 2, 0, 0x034A },
    { "LMGetNewUnused", LM_GETS, 2, 0, 0x0BC0 },
    { "LMGetNxtDB", LM_GETS, 2, 0, 0x0342 },
    { "LMGetOSTable", LM_IGETU, 4, 4, 0x0400 },
    { "LMGetOSTableSplit", LM_IGETU, 4, 4, 0x06CC },
    { "LMGetOSTableSplit2", LM_IGETU, 4, 4, 0x06F8 },
    { "LMGetOSTableSplit3", LM_IGETU, 4, 4, 0x0788 },
    { "LMGetOSTableSplit4", LM_IGETU, 4, 4, 0x07C0 },
    { "LMGetOSTableSplit5", LM_IGETU, 4, 4, 0x07F4 },
    { "LMGetOldTimeSCSIDB", LM_GETS, 2, 0, 0x0DA6 },
    { "LMGetOmegaSANE", LM_IGETS, 2, 2, 0x0B6C },
    { "LMGetPMMU", LM_IGETU, 4, 4, 0x00E0 },
    { "LMGetPMSPPtr", LM_GETU, 4, 0, 0x0386 },
    { "LMGetPSCIntTbl", LM_GETU, 4, 0, 0x1FC0 },
    { "LMGetPWMBuf1", LM_GETU, 4, 0, 0x0B0A },
    { "LMGetPWMBuf2", LM_GETU, 4, 0, 0x0312 },
    { "LMGetPWMValue", LM_GETS, 2, 0, 0x0138 },
    { "LMGetPadRsrc", LM_ADDR, 18, 0, 0x0B88 },
    { "LMGetPadRsrc2", LM_GETS, 2, 0, 0x0B9C },
    { "LMGetParams", LM_OUT, 50, 0, 0x03A4 },
    { "LMGetPhys2Log", LM_GETU, 4, 0, 0x1EF0 },
    { "LMGetPhysMemTop", LM_GETU, 4, 0, 0x1EF8 },
    { "LMGetPmgrHandle", LM_GETU, 4, 0, 0x0DC8 },
    { "LMGetPollProc", LM_GETU, 4, 0, 0x013E },
    { "LMGetPollRtnAddr", LM_GETU, 4, 0, 0x0128 },
    { "LMGetPollStack", LM_GETU, 4, 0, 0x013A },
    { "LMGetPortAUse", LM_GETU, 1, 0, 0x0290 },
    { "LMGetPowerMgrVars", LM_GETU, 4, 0, 0x0D18 },
    { "LMGetPrivilegeViolation", LM_GETU, 4, 0, 0x0020 },
    { "LMGetQDErr", LM_GETS, 2, 0, 0x0D6E },
    { "LMGetRGBBlack", LM_OUT, 6, 0, 0x0C10 },
    { "LMGetRGBWhite", LM_OUT, 6, 0, 0x0C16 },
    { "LMGetRMGRHiVars", LM_GETU, 4, 0, 0x0B80 },
    { "LMGetRMGRPerm", LM_GETU, 1, 0, 0x0BA4 },
    { "LMGetROMMapHndl", LM_GETU, 4, 0, 0x0B06 },
    { "LMGetRSDHndl", LM_GETU, 4, 0, 0x028A },
    { "LMGetRawMouse", LM_OUT, 4, 0, 0x082C },
    { "LMGetRealMemTop", LM_GETU, 4, 0, 0x1EF4 },
    { "LMGetRegRsrc", LM_GETU, 1, 0, 0x0347 },
    { "LMGetReqstVol", LM_GETU, 4, 0, 0x03EE },
    { "LMGetResOneDeep", LM_GETU, 1, 0, 0x0B9A },
    { "LMGetResReadOnly", LM_GETS, 2, 0, 0x0A5C },
    { "LMGetReserved", LM_GETU, 4, 0, 0x00DC },
    { "LMGetReserved2", LM_IGETU, 4, 4, 0x00EC },
    { "LMGetReset", LM_GETU, 4, 0, 0x0004 },
    { "LMGetRowBits", LM_GETS, 2, 0, 0x0C20 },
    { "LMGetSCCASts", LM_GETU, 1, 0, 0x02CE },
    { "LMGetSCCBSts", LM_GETU, 1, 0, 0x02CF },
    { "LMGetSCCIOPFlag", LM_GETU, 1, 0, 0x0BFE },
    { "LMGetSCSI2Base", LM_GETU, 4, 0, 0x1FF0 },
    { "LMGetSCSIBase", LM_GETU, 4, 0, 0x0C00 },
    { "LMGetSCSIDMA", LM_GETU, 4, 0, 0x0C04 },
    { "LMGetSCSIDrvrs", LM_GETS, 2, 0, 0x0B2E },
    { "LMGetSCSIGlobals", LM_GETU, 4, 0, 0x0C0C },
    { "LMGetSCSIHsk", LM_GETU, 4, 0, 0x0C08 },
    { "LMGetSCSIPoll", LM_GETU, 1, 0, 0x0C2F },
    { "LMGetSDMJmpTblPtr", LM_GETU, 4, 0, 0x0DB8 },
    { "LMGetSEVarBase", LM_GETU, 4, 0, 0x0C30 },
    { "LMGetSEVarBase0", LM_GETU, 4, 0, 0x0C34 },
    { "LMGetSEVarBase1", LM_ADDR, 24, 0, 0x0C38 },
    { "LMGetSEVarBase2", LM_GETU, 4, 0, 0x0C50 },
    { "LMGetSEVarBase3", LM_GETU, 4, 0, 0x0C54 },
    { "LMGetSEVarBase4", LM_ADDR, 20, 0, 0x0C58 },
    { "LMGetSEVarBase5", LM_ADDR, 56, 0, 0x0C78 },
    { "LMGetSInitFlags", LM_GETS, 2, 0, 0x0D90 },
    { "LMGetSMGlobals", LM_GETU, 4, 0, 0x0CC4 },
    { "LMGetSPMisc1", LM_GETU, 1, 0, 0x020A },
    { "LMGetSaneTrapAddr", LM_GETU, 4, 0, 0x0DE4 },
    { "LMGetSaveFondFlags", LM_GETU, 1, 0, 0x0986 },
    { "LMGetSaveSP", LM_GETU, 4, 0, 0x0A94 },
    { "LMGetSaveSegHandle", LM_GETU, 4, 0, 0x0930 },
    { "LMGetSavedHandle", LM_GETU, 4, 0, 0x0A28 },
    { "LMGetSavedHilite", LM_GETU, 1, 0, 0x0D43 },
    { "LMGetScrDmpType", LM_GETU, 1, 0, 0x02F9 },
    { "LMGetScrapTag", LM_ADDR, 16, 0, 0x0970 },
    { "LMGetScreenBytes", LM_GETU, 4, 0, 0x0C24 },
    { "LMGetScrnVBLPtr", LM_GETU, 4, 0, 0x0D10 },
    { "LMGetSegHiEnable", LM_GETU, 1, 0, 0x0BB2 },
    { "LMGetSerialVars", LM_ADDR, 8, 0, 0x02D0 },
    { "LMGetSetOSDefKey", LM_GETU, 4, 0, 0x0CDC },
    { "LMGetShieldDepth", LM_GETS, 2, 0, 0x0D4C },
    { "LMGetShieldRect", LM_OUT, 8, 0, 0x0D44 },
    { "LMGetShutDownQHdr", LM_GETU, 4, 0, 0x0BBC },
    { "LMGetSlotPrTbl", LM_GETU, 4, 0, 0x0D08 },
    { "LMGetSlotQDT", LM_GETU, 4, 0, 0x0D04 },
    { "LMGetSlotTicks", LM_GETU, 4, 0, 0x0D14 },
    { "LMGetSlotVBLQ", LM_GETU, 4, 0, 0x0D0C },
    { "LMGetSonyVars", LM_GETU, 4, 0, 0x0134 },
    { "LMGetSoundDCE", LM_GETU, 4, 0, 0x027A },
    { "LMGetSoundGlue", LM_GETU, 4, 0, 0x0AE8 },
    { "LMGetSoundVBL", LM_ADDR, 16, 0, 0x026A },
    { "LMGetSpuriousInterrupt", LM_GETU, 4, 0, 0x0060 },
    { "LMGetSrcDevice", LM_GETU, 4, 0, 0x08A0 },
    { "LMGetStkLowPt", LM_GETU, 4, 0, 0x0110 },
    { "LMGetSwitcher", LM_GETU, 4, 0, 0x0282 },
    { "LMGetSysEvtBuf", LM_GETU, 4, 0, 0x0146 },
    { "LMGetSysVolCPtr", LM_GETU, 4, 0, 0x037C },
    { "LMGetSystemInfo", LM_GETU, 1, 0, 0x0B73 },
    { "LMGetSystemMenuList", LM_GETU, 4, 0, 0x0286 },
    { "LMGetT1Arbitrate", LM_GETU, 1, 0, 0x0B3F },
    { "LMGetTEWdBreak", LM_GETU, 4, 0, 0x0AF6 },
    { "LMGetTRAPError", LM_GETU, 4, 0, 0x001C },
    { "LMGetTRAPTable", LM_IGETU, 4, 4, 0x0080 },
    { "LMGetTableSeed", LM_GETU, 4, 0, 0x0D20 },
    { "LMGetTagData", LM_GETS, 2, 0, 0x02FA },
    { "LMGetTaskLock", LM_GETU, 1, 0, 0x0A62 },
    { "LMGetTempRect", LM_ADDR, 8, 0, 0x09FA },
    { "LMGetTheCrsr", LM_ADDR, 0, 0, 0x0844 },
    { "LMGetTimeVars", LM_GETU, 4, 0, 0x0B30 },
    { "LMGetTimeViaDB", LM_GETS, 2, 0, 0x0CEA },
    { "LMGetTocks", LM_GETU, 1, 0, 0x0173 },
    { "LMGetToolTable", LM_IGETU, 4, 4, 0x0E00 },
    { "LMGetTop2MenuItem", LM_GETS, 2, 0, 0x0B26 },
    { "LMGetTrace", LM_GETU, 4, 0, 0x0024 },
    { "LMGetTrapAgain", LM_GETU, 4, 0, 0x0B00 },
    { "LMGetTwitcher2", LM_GETU, 4, 0, 0x0B7C },
    { "LMGetUnassigned", LM_GETU, 4, 0, 0x0030 },
    { "LMGetUnassigned2", LM_IGETU, 4, 4, 0x0040 },
    { "LMGetUninited", LM_GETU, 4, 0, 0x003C },
    { "LMGetUnivInfoPtr", LM_GETU, 4, 0, 0x0DD8 },
    { "LMGetUnivROMFlags", LM_GETU, 4, 0, 0x0DD4 },
    { "LMGetUsedFWidths", LM_GETU, 1, 0, 0x0BF5 },
    { "LMGetVIA2DT", LM_IGETU, 4, 4, 0x0D70 },
    { "LMGetVMGlobals", LM_GETU, 4, 0, 0x0B78 },
    { "LMGetVMVectors", LM_GETU, 4, 0, 0x0CF0 },
    { "LMGetVectorPtr", LM_GETU, 4, 0, 0x2000 },
    { "LMGetVectors", LM_IGETU, 4, 4, 0x2004 },
    { "LMGetVertRRate", LM_GETS, 2, 0, 0x0D30 },
    { "LMGetVia2Rbv", LM_GETU, 4, 0, 0x0CEC },
    { "LMGetVidMode", LM_GETU, 1, 0, 0x0C2E },
    { "LMGetVidType", LM_GETU, 1, 0, 0x0C2D },
    { "LMGetVideoInfoOK", LM_GETU, 4, 0, 0x0DB0 },
    { "LMGetWMgrUpdate", LM_GETU, 4, 0, 0x0AFC },
    { "LMGetWarhol", LM_GETU, 4, 0, 0x0DE8 },
    { "LMGetWarmStart", LM_GETU, 4, 0, 0x0CFC },
    { "LMGetXRgSvArea1", LM_GETU, 4, 0, 0x036E },
    { "LMGetXRgSvArea2", LM_ADDR, 5, 0, 0x0377 },
    { "LMGetXRgSvArea3", LM_GETU, 4, 0, 0x0380 },
    { "LMGetXRgSvArea5", LM_ADDR, 14, 0, 0x038A },
    { "LMGetXRgSvArea6", LM_ADDR, 6, 0, 0x039C },
    { "LMGetmbSaveLoc", LM_GETU, 4, 0, 0x0B5C },
    { "LMGetsInfoPtr", LM_GETU, 4, 0, 0x0CBC },
    { "LMGetsRsrcTblPtr", LM_GETU, 4, 0, 0x0D24 },
    { "LMGetsdmBusErr", LM_GETU, 4, 0, 0x0DC0 },
    { "LMSetADBBase", LM_SET, 4, 0, 0x0CF8 },
    { "LMSetAGBHandle", LM_SET, 4, 0, 0x0D1C },
    { "LMSetASCBase", LM_SET, 4, 0, 0x0CC0 },
    { "LMSetATalkHk1", LM_SET, 4, 0, 0x0B14 },
    { "LMSetAddrMapFlags", LM_SET, 4, 0, 0x0DD0 },
    { "LMSetAddressError", LM_SET, 4, 0, 0x000C },
    { "LMSetAlarmState", LM_SET, 1, 0, 0x021F },
    { "LMSetApp2Packs", LM_ISET, 4, 4, 0x0BC8 },
    { "LMSetAt2MenuBottom", LM_SET, 2, 0, 0x0B28 },
    { "LMSetAutoInt1", LM_SET, 4, 0, 0x0064 },
    { "LMSetAutoInt2", LM_SET, 4, 0, 0x0068 },
    { "LMSetAutoInt3", LM_SET, 4, 0, 0x006C },
    { "LMSetAutoInt4", LM_SET, 4, 0, 0x0070 },
    { "LMSetAutoInt5", LM_SET, 4, 0, 0x0074 },
    { "LMSetAutoInt6", LM_SET, 4, 0, 0x0078 },
    { "LMSetAutoInt7", LM_SET, 4, 0, 0x007C },
    { "LMSetBackgrounderVars", LM_SET, 4, 0, 0x0B64 },
    { "LMSetBootGlobPtr", LM_SET, 4, 0, 0x0DDC },
    { "LMSetBootMask", LM_SET, 2, 0, 0x0B0E },
    { "LMSetBootTmp8", LM_COPYIN, 8, 0, 0x0B36 },
    { "LMSetBoxFlag", LM_SET, 1, 0, 0x0CB3 },
    { "LMSetBrianBits", LM_SET, 2, 0, 0x0B20 },
    { "LMSetBtDskRfn", LM_SET, 2, 0, 0x0B34 },
    { "LMSetBusError", LM_SET, 4, 0, 0x0008 },
    { "LMSetCHKError", LM_SET, 4, 0, 0x0018 },
    { "LMSetCMVector", LM_SET, 4, 0, 0x0BB4 },
    { "LMSetCQDGlobals", LM_SET, 4, 0, 0x0CCC },
    { "LMSetCarlByte", LM_SET, 1, 0, 0x0B72 },
    { "LMSetChooserBits", LM_SET, 1, 0, 0x0946 },
    { "LMSetChunkyDepth", LM_SET, 2, 0, 0x0D60 },
    { "LMSetCkdDB", LM_SET, 2, 0, 0x0340 },
    { "LMSetCloseOrnHook", LM_SET, 4, 0, 0x0A88 },
    { "LMSetCoProcessor", LM_SET, 4, 0, 0x0034 },
    { "LMSetColLines", LM_SET, 2, 0, 0x0C22 },
    { "LMSetCoreEditVars", LM_COPYIN, 12, 0, 0x0954 },
    { "LMSetCrsrAddr", LM_SET, 4, 0, 0x0888 },
    { "LMSetCrsrBase", LM_SET, 4, 0, 0x0898 },
    { "LMSetCrsrCouple", LM_SET, 1, 0, 0x08CF },
    { "LMSetCrsrDevice", LM_SET, 4, 0, 0x089C },
    { "LMSetCrsrNew", LM_SET, 1, 0, 0x08CE },
    { "LMSetCrsrObscure", LM_SET, 1, 0, 0x08D2 },
    { "LMSetCrsrPtr", LM_SET, 4, 0, 0x0D62 },
    { "LMSetCrsrRect", LM_COPYIN, 8, 0, 0x083C },
    { "LMSetCrsrRow", LM_SET, 2, 0, 0x08AC },
    { "LMSetCrsrScale", LM_SET, 1, 0, 0x08D3 },
    { "LMSetCurDeKind", LM_SET, 2, 0, 0x0A22 },
    { "LMSetCurDragAction", LM_SET, 4, 0, 0x0A46 },
    { "LMSetCurFMInput", LM_COPYIN, 16, 0, 0x0988 },
    { "LMSetCurLayer", LM_SET, 4, 0, 0x0A90 },
    { "LMSetDSCtrAdj", LM_SET, 4, 0, 0x0DA8 },
    { "LMSetDSDrawProc", LM_SET, 4, 0, 0x0334 },
    { "LMSetDSPGlobals", LM_SET, 4, 0, 0x1FC4 },
    { "LMSetDSWndUpdate", LM_SET, 1, 0, 0x015D },
    { "LMSetDeskPatEnable", LM_SET, 2, 0, 0x0CE8 },
    { "LMSetDeskPort", LM_SET, 4, 0, 0x09E2 },
    { "LMSetDiskVars", LM_COPYIN, 62, 0, 0x0222 },
    { "LMSetDivideByZero", LM_SET, 4, 0, 0x0014 },
    { "LMSetDockingGlobals", LM_SET, 4, 0, 0x1FF8 },
    { "LMSetDrMstrBlk", LM_SET, 2, 0, 0x034C },
    { "LMSetDragFlag", LM_SET, 2, 0, 0x0A44 },
    { "LMSetDrawCrsrVector", LM_SET, 4, 0, 0x1FB8 },
    { "LMSetDskErr", LM_SET, 2, 0, 0x0142 },
    { "LMSetDskRtnAdr", LM_SET, 4, 0, 0x0124 },
    { "LMSetDskSwtchHook", LM_SET, 4, 0, 0x03EA },
    { "LMSetDskVerify", LM_SET, 1, 0, 0x012C },
    { "LMSetEgretGlobals", LM_SET, 4, 0, 0x0DE0 },
    { "LMSetEndSRTPtr", LM_SET, 4, 0, 0x0DB4 },
    { "LMSetErCode", LM_SET, 2, 0, 0x03A2 },
    { "LMSetEraseCrsrVector", LM_SET, 4, 0, 0x1FBC },
    { "LMSetEvtBufCnt", LM_SET, 2, 0, 0x0154 },
    { "LMSetExpandMem", LM_SET, 4, 0, 0x02B6 },
    { "LMSetExtFSHook", LM_SET, 4, 0, 0x03E6 },
    { "LMSetExtraPrint", LM_COPYIN, 13, 0, 0x0947 },
    { "LMSetFDevDisable", LM_SET, 1, 0, 0x0BB3 },
    { "LMSetFLckUnlck", LM_SET, 1, 0, 0x0348 },
    { "LMSetFMDefaultSize", LM_SET, 1, 0, 0x0987 },
    { "LMSetFMDotsPerInch", LM_SET, 4, 0, 0x09B2 },
    { "LMSetFMExist", LM_SET, 1, 0, 0x0D42 },
    { "LMSetFMStyleTab", LM_COPYIN, 24, 0, 0x09B6 },
    { "LMSetFMSynth", LM_SET, 4, 0, 0x0C1C },
    { "LMSetFMgrOutRec", LM_COPYIN, 26, 0, 0x0998 },
    { "LMSetFONDID", LM_SET, 2, 0, 0x0BC6 },
    { "LMSetFP040Vects", LM_ISET, 4, 4, 0x1FC8 },
    { "LMSetFP68881", LM_ISET, 4, 4, 0x00C0 },
    { "LMSetFPState", LM_COPYIN, 6, 0, 0x0A4A },
    { "LMSetFSMVars", LM_SET, 4, 0, 0x0BB8 },
    { "LMSetFSQueueHook", LM_SET, 4, 0, 0x03E2 },
    { "LMSetFSTemp4", LM_SET, 4, 0, 0x03DE },
    { "LMSetFSTemp8", LM_COPYIN, 8, 0, 0x03D6 },
    { "LMSetFScaleHFact", LM_SET, 4, 0, 0x0BF6 },
    { "LMSetFScaleVFact", LM_SET, 4, 0, 0x0BFA },
    { "LMSetFileShareVars", LM_SET, 4, 0, 0x0B50 },
    { "LMSetFinder", LM_SET, 1, 0, 0x0261 },
    { "LMSetFlushOnly", LM_SET, 1, 0, 0x0346 },
    { "LMSetFondState", LM_SET, 1, 0, 0x0903 },
    { "LMSetFontFlag", LM_SET, 1, 0, 0x015E },
    { "LMSetFormatError", LM_SET, 4, 0, 0x0038 },
    { "LMSetFourDHack", LM_SET, 4, 0, 0x0B1C },
    { "LMSetFrcSync", LM_SET, 1, 0, 0x0349 },
    { "LMSetGZRootPtr", LM_SET, 4, 0, 0x032C },
    { "LMSetHFSFlags", LM_SET, 1, 0, 0x0376 },
    { "LMSetHFSStkTop", LM_SET, 4, 0, 0x036A },
    { "LMSetHSCHndl", LM_SET, 4, 0, 0x0B84 },
    { "LMSetHiHeapMark", LM_SET, 4, 0, 0x0BAE },
    { "LMSetHpChk", LM_SET, 4, 0, 0x0316 },
    { "LMSetIOPMgrVars", LM_SET, 4, 0, 0x0C28 },
    { "LMSetIconBitmap", LM_COPYIN, 14, 0, 0x0A0E },
    { "LMSetIconTLAddr", LM_SET, 4, 0, 0x0DAC },
    { "LMSetIllegalInstruction", LM_SET, 4, 0, 0x0010 },
    { "LMSetIntFlag", LM_SET, 1, 0, 0x015F },
    { "LMSetJAllocCrsr", LM_SET, 4, 0, 0x088C },
    { "LMSetJCheckLoad", LM_SET, 4, 0, 0x07F0 },
    { "LMSetJDCDReset", LM_SET, 4, 0, 0x0B48 },
    { "LMSetJDiskSel", LM_SET, 4, 0, 0x0B40 },
    { "LMSetJDtrmV2", LM_SET, 4, 0, 0x06C4 },
    { "LMSetJDtrmV3", LM_SET, 4, 0, 0x07BC },
    { "LMSetJFileOpen", LM_SET, 4, 0, 0x0784 },
    { "LMSetJFindDrive", LM_SET, 4, 0, 0x06C8 },
    { "LMSetJFlushCache", LM_SET, 4, 0, 0x06F4 },
    { "LMSetJKybdTask", LM_SET, 4, 0, 0x021A },
    { "LMSetJOpcodeProc", LM_SET, 4, 0, 0x0894 },
    { "LMSetJSendCmd", LM_SET, 4, 0, 0x0B44 },
    { "LMSetJSetCCrsr", LM_SET, 4, 0, 0x0890 },
    { "LMSetJShell", LM_SET, 2, 0, 0x0212 },
    { "LMSetJSwapMMU", LM_SET, 4, 0, 0x0DBC },
    { "LMSetKbdVars", LM_SET, 2, 0, 0x0216 },
    { "LMSetKeyLast", LM_SET, 2, 0, 0x0184 },
    { "LMSetKeyMVars", LM_SET, 2, 0, 0x0B04 },
    { "LMSetKeyRepTime", LM_SET, 4, 0, 0x018A },
    { "LMSetLastBG", LM_SET, 4, 0, 0x0D3A },
    { "LMSetLastBinPat", LM_COPYIN, 8, 0, 0x0CE0 },
    { "LMSetLastDepth", LM_SET, 2, 0, 0x0D40 },
    { "LMSetLastFore", LM_SET, 4, 0, 0x0D36 },
    { "LMSetLastMode", LM_SET, 2, 0, 0x0D3E },
    { "LMSetLastTxGDevice", LM_SET, 4, 0, 0x0DC4 },
    { "LMSetLaunchFlag", LM_SET, 1, 0, 0x0902 },
    { "LMSetLayerPalette", LM_SET, 4, 0, 0x0DCC },
    { "LMSetLine1010", LM_SET, 4, 0, 0x0028 },
    { "LMSetLine1111", LM_SET, 4, 0, 0x002C },
    { "LMSetLoadFiller", LM_SET, 4, 0, 0x090C },
    { "LMSetLoadTrap", LM_SET, 1, 0, 0x012D },
    { "LMSetLoaderPBlock", LM_COPYIN, 10, 0, 0x093A },
    { "LMSetLockMemCt", LM_SET, 4, 0, 0x1FF4 },
    { "LMSetMAErrProc", LM_SET, 4, 0, 0x0BE8 },
    { "LMSetMASuperTab", LM_SET, 4, 0, 0x0BEC },
    { "LMSetMBFlash", LM_SET, 4, 0, 0x0D5C },
    { "LMSetMBProcHndl", LM_SET, 4, 0, 0x0D54 },
    { "LMSetMMDefFlags", LM_SET, 2, 0, 0x0326 },
    { "LMSetMMFlags", LM_SET, 1, 0, 0x1EFC },
    { "LMSetMMU24Tbl", LM_SET, 4, 0, 0x0CB4 },
    { "LMSetMMUFlags", LM_SET, 1, 0, 0x0CB0 },
    { "LMSetMMUTblSize", LM_SET, 4, 0, 0x0CB8 },
    { "LMSetMacJmp", LM_SET, 4, 0, 0x0120 },
    { "LMSetMacJmpFlag", LM_SET, 1, 0, 0x0BFF },
    { "LMSetMacZero", LM_SET, 4, 0, 0x0000 },
    { "LMSetMacsBugPC", LM_SET, 4, 0, 0x0C70 },
    { "LMSetMacsBugSP", LM_SET, 4, 0, 0x0C6C },
    { "LMSetMacsBugSR", LM_SET, 4, 0, 0x0C74 },
    { "LMSetMaxDB", LM_SET, 2, 0, 0x0344 },
    { "LMSetMem2Vectors24", LM_ISET, 4, 4, 0x1EE0 },
    { "LMSetMem2Vectors32", LM_ISET, 4, 4, 0x1FE0 },
    { "LMSetMemVectors24", LM_ISET, 4, 4, 0x1E00 },
    { "LMSetMemVectors32", LM_ISET, 4, 4, 0x1F00 },
    { "LMSetMenuLayer", LM_SET, 4, 0, 0x0B68 },
    { "LMSetMickeyBytes", LM_SET, 4, 0, 0x0D6A },
    { "LMSetMimeGlobs", LM_SET, 4, 0, 0x0BF0 },
    { "LMSetMmInOK", LM_SET, 1, 0, 0x012E },
    { "LMSetMonkeyLives", LM_SET, 2, 0, 0x0100 },
    { "LMSetMouse", LM_SET, 4, 0, 0x0830 },
    { "LMSetMouseMask", LM_SET, 4, 0, 0x08D6 },
    { "LMSetMouseOffset", LM_SET, 4, 0, 0x08DA },
    { "LMSetNMIFlag", LM_SET, 1, 0, 0x0C2C },
    { "LMSetNMQHdr", LM_SET, 4, 0, 0x0B60 },
    { "LMSetNTSC", LM_SET, 1, 0, 0x0B3E },
    { "LMSetNewMount", LM_SET, 2, 0, 0x034A },
    { "LMSetNewUnused", LM_SET, 2, 0, 0x0BC0 },
    { "LMSetNxtDB", LM_SET, 2, 0, 0x0342 },
    { "LMSetOSTable", LM_ISET, 4, 4, 0x0400 },
    { "LMSetOSTableSplit", LM_ISET, 4, 4, 0x06CC },
    { "LMSetOSTableSplit2", LM_ISET, 4, 4, 0x06F8 },
    { "LMSetOSTableSplit3", LM_ISET, 4, 4, 0x0788 },
    { "LMSetOSTableSplit4", LM_ISET, 4, 4, 0x07C0 },
    { "LMSetOSTableSplit5", LM_ISET, 4, 4, 0x07F4 },
    { "LMSetOldTimeSCSIDB", LM_SET, 2, 0, 0x0DA6 },
    { "LMSetOmegaSANE", LM_ISET, 2, 2, 0x0B6C },
    { "LMSetPMMU", LM_ISET, 4, 4, 0x00E0 },
    { "LMSetPMSPPtr", LM_SET, 4, 0, 0x0386 },
    { "LMSetPSCIntTbl", LM_SET, 4, 0, 0x1FC0 },
    { "LMSetPWMBuf1", LM_SET, 4, 0, 0x0B0A },
    { "LMSetPWMBuf2", LM_SET, 4, 0, 0x0312 },
    { "LMSetPWMValue", LM_SET, 2, 0, 0x0138 },
    { "LMSetPadRsrc", LM_COPYIN, 18, 0, 0x0B88 },
    { "LMSetPadRsrc2", LM_SET, 2, 0, 0x0B9C },
    { "LMSetParams", LM_COPYIN, 50, 0, 0x03A4 },
    { "LMSetPhys2Log", LM_SET, 4, 0, 0x1EF0 },
    { "LMSetPhysMemTop", LM_SET, 4, 0, 0x1EF8 },
    { "LMSetPmgrHandle", LM_SET, 4, 0, 0x0DC8 },
    { "LMSetPollProc", LM_SET, 4, 0, 0x013E },
    { "LMSetPollRtnAddr", LM_SET, 4, 0, 0x0128 },
    { "LMSetPollStack", LM_SET, 4, 0, 0x013A },
    { "LMSetPortAUse", LM_SET, 1, 0, 0x0290 },
    { "LMSetPowerMgrVars", LM_SET, 4, 0, 0x0D18 },
    { "LMSetPrivilegeViolation", LM_SET, 4, 0, 0x0020 },
    { "LMSetQDErr", LM_SET, 2, 0, 0x0D6E },
    { "LMSetRGBBlack", LM_COPYIN, 6, 0, 0x0C10 },
    { "LMSetRGBWhite", LM_COPYIN, 6, 0, 0x0C16 },
    { "LMSetRMGRHiVars", LM_SET, 4, 0, 0x0B80 },
    { "LMSetRMGRPerm", LM_SET, 1, 0, 0x0BA4 },
    { "LMSetROMMapHndl", LM_SET, 4, 0, 0x0B06 },
    { "LMSetRSDHndl", LM_SET, 4, 0, 0x028A },
    { "LMSetRawMouse", LM_SET, 4, 0, 0x082C },
    { "LMSetRealMemTop", LM_SET, 4, 0, 0x1EF4 },
    { "LMSetRegRsrc", LM_SET, 1, 0, 0x0347 },
    { "LMSetReqstVol", LM_SET, 4, 0, 0x03EE },
    { "LMSetResOneDeep", LM_SET, 1, 0, 0x0B9A },
    { "LMSetResReadOnly", LM_SET, 2, 0, 0x0A5C },
    { "LMSetReserved", LM_SET, 4, 0, 0x00DC },
    { "LMSetReserved2", LM_ISET, 4, 4, 0x00EC },
    { "LMSetReset", LM_SET, 4, 0, 0x0004 },
    { "LMSetRowBits", LM_SET, 2, 0, 0x0C20 },
    { "LMSetSCCASts", LM_SET, 1, 0, 0x02CE },
    { "LMSetSCCBSts", LM_SET, 1, 0, 0x02CF },
    { "LMSetSCCIOPFlag", LM_SET, 1, 0, 0x0BFE },
    { "LMSetSCSI2Base", LM_SET, 4, 0, 0x1FF0 },
    { "LMSetSCSIBase", LM_SET, 4, 0, 0x0C00 },
    { "LMSetSCSIDMA", LM_SET, 4, 0, 0x0C04 },
    { "LMSetSCSIDrvrs", LM_SET, 2, 0, 0x0B2E },
    { "LMSetSCSIGlobals", LM_SET, 4, 0, 0x0C0C },
    { "LMSetSCSIHsk", LM_SET, 4, 0, 0x0C08 },
    { "LMSetSCSIPoll", LM_SET, 1, 0, 0x0C2F },
    { "LMSetSDMJmpTblPtr", LM_SET, 4, 0, 0x0DB8 },
    { "LMSetSEVarBase", LM_SET, 4, 0, 0x0C30 },
    { "LMSetSEVarBase0", LM_SET, 4, 0, 0x0C34 },
    { "LMSetSEVarBase1", LM_COPYIN, 24, 0, 0x0C38 },
    { "LMSetSEVarBase2", LM_SET, 4, 0, 0x0C50 },
    { "LMSetSEVarBase3", LM_SET, 4, 0, 0x0C54 },
    { "LMSetSEVarBase4", LM_COPYIN, 20, 0, 0x0C58 },
    { "LMSetSEVarBase5", LM_COPYIN, 56, 0, 0x0C78 },
    { "LMSetSInitFlags", LM_SET, 2, 0, 0x0D90 },
    { "LMSetSMGlobals", LM_SET, 4, 0, 0x0CC4 },
    { "LMSetSPMisc1", LM_SET, 1, 0, 0x020A },
    { "LMSetSaneTrapAddr", LM_SET, 4, 0, 0x0DE4 },
    { "LMSetSaveFondFlags", LM_SET, 1, 0, 0x0986 },
    { "LMSetSaveSP", LM_SET, 4, 0, 0x0A94 },
    { "LMSetSaveSegHandle", LM_SET, 4, 0, 0x0930 },
    { "LMSetSavedHandle", LM_SET, 4, 0, 0x0A28 },
    { "LMSetSavedHilite", LM_SET, 1, 0, 0x0D43 },
    { "LMSetScrDmpType", LM_SET, 1, 0, 0x02F9 },
    { "LMSetScrapTag", LM_COPYIN, 16, 0, 0x0970 },
    { "LMSetScreenBytes", LM_SET, 4, 0, 0x0C24 },
    { "LMSetScrnVBLPtr", LM_SET, 4, 0, 0x0D10 },
    { "LMSetSegHiEnable", LM_SET, 1, 0, 0x0BB2 },
    { "LMSetSerialVars", LM_COPYIN, 8, 0, 0x02D0 },
    { "LMSetSetOSDefKey", LM_SET, 4, 0, 0x0CDC },
    { "LMSetShieldDepth", LM_SET, 2, 0, 0x0D4C },
    { "LMSetShieldRect", LM_COPYIN, 8, 0, 0x0D44 },
    { "LMSetShutDownQHdr", LM_SET, 4, 0, 0x0BBC },
    { "LMSetSlotPrTbl", LM_SET, 4, 0, 0x0D08 },
    { "LMSetSlotQDT", LM_SET, 4, 0, 0x0D04 },
    { "LMSetSlotTicks", LM_SET, 4, 0, 0x0D14 },
    { "LMSetSlotVBLQ", LM_SET, 4, 0, 0x0D0C },
    { "LMSetSonyVars", LM_SET, 4, 0, 0x0134 },
    { "LMSetSoundDCE", LM_SET, 4, 0, 0x027A },
    { "LMSetSoundGlue", LM_SET, 4, 0, 0x0AE8 },
    { "LMSetSoundVBL", LM_COPYIN, 16, 0, 0x026A },
    { "LMSetSpuriousInterrupt", LM_SET, 4, 0, 0x0060 },
    { "LMSetSrcDevice", LM_SET, 4, 0, 0x08A0 },
    { "LMSetStkLowPt", LM_SET, 4, 0, 0x0110 },
    { "LMSetSwitcher", LM_SET, 4, 0, 0x0282 },
    { "LMSetSysEvtBuf", LM_SET, 4, 0, 0x0146 },
    { "LMSetSysVolCPtr", LM_SET, 4, 0, 0x037C },
    { "LMSetSystemInfo", LM_SET, 1, 0, 0x0B73 },
    { "LMSetSystemMenuList", LM_SET, 4, 0, 0x0286 },
    { "LMSetT1Arbitrate", LM_SET, 1, 0, 0x0B3F },
    { "LMSetTEWdBreak", LM_SET, 4, 0, 0x0AF6 },
    { "LMSetTRAPError", LM_SET, 4, 0, 0x001C },
    { "LMSetTRAPTable", LM_ISET, 4, 4, 0x0080 },
    { "LMSetTableSeed", LM_SET, 4, 0, 0x0D20 },
    { "LMSetTagData", LM_SET, 2, 0, 0x02FA },
    { "LMSetTaskLock", LM_SET, 1, 0, 0x0A62 },
    { "LMSetTempRect", LM_COPYIN, 8, 0, 0x09FA },
    { "LMSetTheCrsr", LM_COPYIN, 68, 0, 0x0844 },
    { "LMSetTimeVars", LM_SET, 4, 0, 0x0B30 },
    { "LMSetTimeViaDB", LM_SET, 2, 0, 0x0CEA },
    { "LMSetTocks", LM_SET, 1, 0, 0x0173 },
    { "LMSetToolTable", LM_ISET, 4, 4, 0x0E00 },
    { "LMSetTop2MenuItem", LM_SET, 2, 0, 0x0B26 },
    { "LMSetTrace", LM_SET, 4, 0, 0x0024 },
    { "LMSetTrapAgain", LM_SET, 4, 0, 0x0B00 },
    { "LMSetTwitcher2", LM_SET, 4, 0, 0x0B7C },
    { "LMSetUnassigned", LM_SET, 4, 0, 0x0030 },
    { "LMSetUnassigned2", LM_ISET, 4, 4, 0x0040 },
    { "LMSetUninited", LM_SET, 4, 0, 0x003C },
    { "LMSetUnivInfoPtr", LM_SET, 4, 0, 0x0DD8 },
    { "LMSetUnivROMFlags", LM_SET, 4, 0, 0x0DD4 },
    { "LMSetUsedFWidths", LM_SET, 1, 0, 0x0BF5 },
    { "LMSetVIA2DT", LM_ISET, 4, 4, 0x0D70 },
    { "LMSetVMGlobals", LM_SET, 4, 0, 0x0B78 },
    { "LMSetVMVectors", LM_SET, 4, 0, 0x0CF0 },
    { "LMSetVectorPtr", LM_SET, 4, 0, 0x2000 },
    { "LMSetVectors", LM_ISET, 4, 4, 0x2004 },
    { "LMSetVertRRate", LM_SET, 2, 0, 0x0D30 },
    { "LMSetVia2Rbv", LM_SET, 4, 0, 0x0CEC },
    { "LMSetVidMode", LM_SET, 1, 0, 0x0C2E },
    { "LMSetVidType", LM_SET, 1, 0, 0x0C2D },
    { "LMSetVideoInfoOK", LM_SET, 4, 0, 0x0DB0 },
    { "LMSetWMgrUpdate", LM_SET, 4, 0, 0x0AFC },
    { "LMSetWarhol", LM_SET, 4, 0, 0x0DE8 },
    { "LMSetWarmStart", LM_SET, 4, 0, 0x0CFC },
    { "LMSetXRgSvArea1", LM_SET, 4, 0, 0x036E },
    { "LMSetXRgSvArea2", LM_COPYIN, 5, 0, 0x0377 },
    { "LMSetXRgSvArea3", LM_SET, 4, 0, 0x0380 },
    { "LMSetXRgSvArea5", LM_COPYIN, 14, 0, 0x038A },
    { "LMSetXRgSvArea6", LM_COPYIN, 6, 0, 0x039C },
    { "LMSetmbSaveLoc", LM_SET, 4, 0, 0x0B5C },
    { "LMSetsInfoPtr", LM_SET, 4, 0, 0x0CBC },
    { "LMSetsRsrcTblPtr", LM_SET, 4, 0, 0x0D24 },
    { "LMSetsdmBusErr", LM_SET, 4, 0, 0x0DC0 },
};

static void lmt_0(ppc_cpu *c) { lmacc_run(&lm_table[0], c); }
static void lmt_1(ppc_cpu *c) { lmacc_run(&lm_table[1], c); }
static void lmt_2(ppc_cpu *c) { lmacc_run(&lm_table[2], c); }
static void lmt_3(ppc_cpu *c) { lmacc_run(&lm_table[3], c); }
static void lmt_4(ppc_cpu *c) { lmacc_run(&lm_table[4], c); }
static void lmt_5(ppc_cpu *c) { lmacc_run(&lm_table[5], c); }
static void lmt_6(ppc_cpu *c) { lmacc_run(&lm_table[6], c); }
static void lmt_7(ppc_cpu *c) { lmacc_run(&lm_table[7], c); }
static void lmt_8(ppc_cpu *c) { lmacc_run(&lm_table[8], c); }
static void lmt_9(ppc_cpu *c) { lmacc_run(&lm_table[9], c); }
static void lmt_10(ppc_cpu *c) { lmacc_run(&lm_table[10], c); }
static void lmt_11(ppc_cpu *c) { lmacc_run(&lm_table[11], c); }
static void lmt_12(ppc_cpu *c) { lmacc_run(&lm_table[12], c); }
static void lmt_13(ppc_cpu *c) { lmacc_run(&lm_table[13], c); }
static void lmt_14(ppc_cpu *c) { lmacc_run(&lm_table[14], c); }
static void lmt_15(ppc_cpu *c) { lmacc_run(&lm_table[15], c); }
static void lmt_16(ppc_cpu *c) { lmacc_run(&lm_table[16], c); }
static void lmt_17(ppc_cpu *c) { lmacc_run(&lm_table[17], c); }
static void lmt_18(ppc_cpu *c) { lmacc_run(&lm_table[18], c); }
static void lmt_19(ppc_cpu *c) { lmacc_run(&lm_table[19], c); }
static void lmt_20(ppc_cpu *c) { lmacc_run(&lm_table[20], c); }
static void lmt_21(ppc_cpu *c) { lmacc_run(&lm_table[21], c); }
static void lmt_22(ppc_cpu *c) { lmacc_run(&lm_table[22], c); }
static void lmt_23(ppc_cpu *c) { lmacc_run(&lm_table[23], c); }
static void lmt_24(ppc_cpu *c) { lmacc_run(&lm_table[24], c); }
static void lmt_25(ppc_cpu *c) { lmacc_run(&lm_table[25], c); }
static void lmt_26(ppc_cpu *c) { lmacc_run(&lm_table[26], c); }
static void lmt_27(ppc_cpu *c) { lmacc_run(&lm_table[27], c); }
static void lmt_28(ppc_cpu *c) { lmacc_run(&lm_table[28], c); }
static void lmt_29(ppc_cpu *c) { lmacc_run(&lm_table[29], c); }
static void lmt_30(ppc_cpu *c) { lmacc_run(&lm_table[30], c); }
static void lmt_31(ppc_cpu *c) { lmacc_run(&lm_table[31], c); }
static void lmt_32(ppc_cpu *c) { lmacc_run(&lm_table[32], c); }
static void lmt_33(ppc_cpu *c) { lmacc_run(&lm_table[33], c); }
static void lmt_34(ppc_cpu *c) { lmacc_run(&lm_table[34], c); }
static void lmt_35(ppc_cpu *c) { lmacc_run(&lm_table[35], c); }
static void lmt_36(ppc_cpu *c) { lmacc_run(&lm_table[36], c); }
static void lmt_37(ppc_cpu *c) { lmacc_run(&lm_table[37], c); }
static void lmt_38(ppc_cpu *c) { lmacc_run(&lm_table[38], c); }
static void lmt_39(ppc_cpu *c) { lmacc_run(&lm_table[39], c); }
static void lmt_40(ppc_cpu *c) { lmacc_run(&lm_table[40], c); }
static void lmt_41(ppc_cpu *c) { lmacc_run(&lm_table[41], c); }
static void lmt_42(ppc_cpu *c) { lmacc_run(&lm_table[42], c); }
static void lmt_43(ppc_cpu *c) { lmacc_run(&lm_table[43], c); }
static void lmt_44(ppc_cpu *c) { lmacc_run(&lm_table[44], c); }
static void lmt_45(ppc_cpu *c) { lmacc_run(&lm_table[45], c); }
static void lmt_46(ppc_cpu *c) { lmacc_run(&lm_table[46], c); }
static void lmt_47(ppc_cpu *c) { lmacc_run(&lm_table[47], c); }
static void lmt_48(ppc_cpu *c) { lmacc_run(&lm_table[48], c); }
static void lmt_49(ppc_cpu *c) { lmacc_run(&lm_table[49], c); }
static void lmt_50(ppc_cpu *c) { lmacc_run(&lm_table[50], c); }
static void lmt_51(ppc_cpu *c) { lmacc_run(&lm_table[51], c); }
static void lmt_52(ppc_cpu *c) { lmacc_run(&lm_table[52], c); }
static void lmt_53(ppc_cpu *c) { lmacc_run(&lm_table[53], c); }
static void lmt_54(ppc_cpu *c) { lmacc_run(&lm_table[54], c); }
static void lmt_55(ppc_cpu *c) { lmacc_run(&lm_table[55], c); }
static void lmt_56(ppc_cpu *c) { lmacc_run(&lm_table[56], c); }
static void lmt_57(ppc_cpu *c) { lmacc_run(&lm_table[57], c); }
static void lmt_58(ppc_cpu *c) { lmacc_run(&lm_table[58], c); }
static void lmt_59(ppc_cpu *c) { lmacc_run(&lm_table[59], c); }
static void lmt_60(ppc_cpu *c) { lmacc_run(&lm_table[60], c); }
static void lmt_61(ppc_cpu *c) { lmacc_run(&lm_table[61], c); }
static void lmt_62(ppc_cpu *c) { lmacc_run(&lm_table[62], c); }
static void lmt_63(ppc_cpu *c) { lmacc_run(&lm_table[63], c); }
static void lmt_64(ppc_cpu *c) { lmacc_run(&lm_table[64], c); }
static void lmt_65(ppc_cpu *c) { lmacc_run(&lm_table[65], c); }
static void lmt_66(ppc_cpu *c) { lmacc_run(&lm_table[66], c); }
static void lmt_67(ppc_cpu *c) { lmacc_run(&lm_table[67], c); }
static void lmt_68(ppc_cpu *c) { lmacc_run(&lm_table[68], c); }
static void lmt_69(ppc_cpu *c) { lmacc_run(&lm_table[69], c); }
static void lmt_70(ppc_cpu *c) { lmacc_run(&lm_table[70], c); }
static void lmt_71(ppc_cpu *c) { lmacc_run(&lm_table[71], c); }
static void lmt_72(ppc_cpu *c) { lmacc_run(&lm_table[72], c); }
static void lmt_73(ppc_cpu *c) { lmacc_run(&lm_table[73], c); }
static void lmt_74(ppc_cpu *c) { lmacc_run(&lm_table[74], c); }
static void lmt_75(ppc_cpu *c) { lmacc_run(&lm_table[75], c); }
static void lmt_76(ppc_cpu *c) { lmacc_run(&lm_table[76], c); }
static void lmt_77(ppc_cpu *c) { lmacc_run(&lm_table[77], c); }
static void lmt_78(ppc_cpu *c) { lmacc_run(&lm_table[78], c); }
static void lmt_79(ppc_cpu *c) { lmacc_run(&lm_table[79], c); }
static void lmt_80(ppc_cpu *c) { lmacc_run(&lm_table[80], c); }
static void lmt_81(ppc_cpu *c) { lmacc_run(&lm_table[81], c); }
static void lmt_82(ppc_cpu *c) { lmacc_run(&lm_table[82], c); }
static void lmt_83(ppc_cpu *c) { lmacc_run(&lm_table[83], c); }
static void lmt_84(ppc_cpu *c) { lmacc_run(&lm_table[84], c); }
static void lmt_85(ppc_cpu *c) { lmacc_run(&lm_table[85], c); }
static void lmt_86(ppc_cpu *c) { lmacc_run(&lm_table[86], c); }
static void lmt_87(ppc_cpu *c) { lmacc_run(&lm_table[87], c); }
static void lmt_88(ppc_cpu *c) { lmacc_run(&lm_table[88], c); }
static void lmt_89(ppc_cpu *c) { lmacc_run(&lm_table[89], c); }
static void lmt_90(ppc_cpu *c) { lmacc_run(&lm_table[90], c); }
static void lmt_91(ppc_cpu *c) { lmacc_run(&lm_table[91], c); }
static void lmt_92(ppc_cpu *c) { lmacc_run(&lm_table[92], c); }
static void lmt_93(ppc_cpu *c) { lmacc_run(&lm_table[93], c); }
static void lmt_94(ppc_cpu *c) { lmacc_run(&lm_table[94], c); }
static void lmt_95(ppc_cpu *c) { lmacc_run(&lm_table[95], c); }
static void lmt_96(ppc_cpu *c) { lmacc_run(&lm_table[96], c); }
static void lmt_97(ppc_cpu *c) { lmacc_run(&lm_table[97], c); }
static void lmt_98(ppc_cpu *c) { lmacc_run(&lm_table[98], c); }
static void lmt_99(ppc_cpu *c) { lmacc_run(&lm_table[99], c); }
static void lmt_100(ppc_cpu *c) { lmacc_run(&lm_table[100], c); }
static void lmt_101(ppc_cpu *c) { lmacc_run(&lm_table[101], c); }
static void lmt_102(ppc_cpu *c) { lmacc_run(&lm_table[102], c); }
static void lmt_103(ppc_cpu *c) { lmacc_run(&lm_table[103], c); }
static void lmt_104(ppc_cpu *c) { lmacc_run(&lm_table[104], c); }
static void lmt_105(ppc_cpu *c) { lmacc_run(&lm_table[105], c); }
static void lmt_106(ppc_cpu *c) { lmacc_run(&lm_table[106], c); }
static void lmt_107(ppc_cpu *c) { lmacc_run(&lm_table[107], c); }
static void lmt_108(ppc_cpu *c) { lmacc_run(&lm_table[108], c); }
static void lmt_109(ppc_cpu *c) { lmacc_run(&lm_table[109], c); }
static void lmt_110(ppc_cpu *c) { lmacc_run(&lm_table[110], c); }
static void lmt_111(ppc_cpu *c) { lmacc_run(&lm_table[111], c); }
static void lmt_112(ppc_cpu *c) { lmacc_run(&lm_table[112], c); }
static void lmt_113(ppc_cpu *c) { lmacc_run(&lm_table[113], c); }
static void lmt_114(ppc_cpu *c) { lmacc_run(&lm_table[114], c); }
static void lmt_115(ppc_cpu *c) { lmacc_run(&lm_table[115], c); }
static void lmt_116(ppc_cpu *c) { lmacc_run(&lm_table[116], c); }
static void lmt_117(ppc_cpu *c) { lmacc_run(&lm_table[117], c); }
static void lmt_118(ppc_cpu *c) { lmacc_run(&lm_table[118], c); }
static void lmt_119(ppc_cpu *c) { lmacc_run(&lm_table[119], c); }
static void lmt_120(ppc_cpu *c) { lmacc_run(&lm_table[120], c); }
static void lmt_121(ppc_cpu *c) { lmacc_run(&lm_table[121], c); }
static void lmt_122(ppc_cpu *c) { lmacc_run(&lm_table[122], c); }
static void lmt_123(ppc_cpu *c) { lmacc_run(&lm_table[123], c); }
static void lmt_124(ppc_cpu *c) { lmacc_run(&lm_table[124], c); }
static void lmt_125(ppc_cpu *c) { lmacc_run(&lm_table[125], c); }
static void lmt_126(ppc_cpu *c) { lmacc_run(&lm_table[126], c); }
static void lmt_127(ppc_cpu *c) { lmacc_run(&lm_table[127], c); }
static void lmt_128(ppc_cpu *c) { lmacc_run(&lm_table[128], c); }
static void lmt_129(ppc_cpu *c) { lmacc_run(&lm_table[129], c); }
static void lmt_130(ppc_cpu *c) { lmacc_run(&lm_table[130], c); }
static void lmt_131(ppc_cpu *c) { lmacc_run(&lm_table[131], c); }
static void lmt_132(ppc_cpu *c) { lmacc_run(&lm_table[132], c); }
static void lmt_133(ppc_cpu *c) { lmacc_run(&lm_table[133], c); }
static void lmt_134(ppc_cpu *c) { lmacc_run(&lm_table[134], c); }
static void lmt_135(ppc_cpu *c) { lmacc_run(&lm_table[135], c); }
static void lmt_136(ppc_cpu *c) { lmacc_run(&lm_table[136], c); }
static void lmt_137(ppc_cpu *c) { lmacc_run(&lm_table[137], c); }
static void lmt_138(ppc_cpu *c) { lmacc_run(&lm_table[138], c); }
static void lmt_139(ppc_cpu *c) { lmacc_run(&lm_table[139], c); }
static void lmt_140(ppc_cpu *c) { lmacc_run(&lm_table[140], c); }
static void lmt_141(ppc_cpu *c) { lmacc_run(&lm_table[141], c); }
static void lmt_142(ppc_cpu *c) { lmacc_run(&lm_table[142], c); }
static void lmt_143(ppc_cpu *c) { lmacc_run(&lm_table[143], c); }
static void lmt_144(ppc_cpu *c) { lmacc_run(&lm_table[144], c); }
static void lmt_145(ppc_cpu *c) { lmacc_run(&lm_table[145], c); }
static void lmt_146(ppc_cpu *c) { lmacc_run(&lm_table[146], c); }
static void lmt_147(ppc_cpu *c) { lmacc_run(&lm_table[147], c); }
static void lmt_148(ppc_cpu *c) { lmacc_run(&lm_table[148], c); }
static void lmt_149(ppc_cpu *c) { lmacc_run(&lm_table[149], c); }
static void lmt_150(ppc_cpu *c) { lmacc_run(&lm_table[150], c); }
static void lmt_151(ppc_cpu *c) { lmacc_run(&lm_table[151], c); }
static void lmt_152(ppc_cpu *c) { lmacc_run(&lm_table[152], c); }
static void lmt_153(ppc_cpu *c) { lmacc_run(&lm_table[153], c); }
static void lmt_154(ppc_cpu *c) { lmacc_run(&lm_table[154], c); }
static void lmt_155(ppc_cpu *c) { lmacc_run(&lm_table[155], c); }
static void lmt_156(ppc_cpu *c) { lmacc_run(&lm_table[156], c); }
static void lmt_157(ppc_cpu *c) { lmacc_run(&lm_table[157], c); }
static void lmt_158(ppc_cpu *c) { lmacc_run(&lm_table[158], c); }
static void lmt_159(ppc_cpu *c) { lmacc_run(&lm_table[159], c); }
static void lmt_160(ppc_cpu *c) { lmacc_run(&lm_table[160], c); }
static void lmt_161(ppc_cpu *c) { lmacc_run(&lm_table[161], c); }
static void lmt_162(ppc_cpu *c) { lmacc_run(&lm_table[162], c); }
static void lmt_163(ppc_cpu *c) { lmacc_run(&lm_table[163], c); }
static void lmt_164(ppc_cpu *c) { lmacc_run(&lm_table[164], c); }
static void lmt_165(ppc_cpu *c) { lmacc_run(&lm_table[165], c); }
static void lmt_166(ppc_cpu *c) { lmacc_run(&lm_table[166], c); }
static void lmt_167(ppc_cpu *c) { lmacc_run(&lm_table[167], c); }
static void lmt_168(ppc_cpu *c) { lmacc_run(&lm_table[168], c); }
static void lmt_169(ppc_cpu *c) { lmacc_run(&lm_table[169], c); }
static void lmt_170(ppc_cpu *c) { lmacc_run(&lm_table[170], c); }
static void lmt_171(ppc_cpu *c) { lmacc_run(&lm_table[171], c); }
static void lmt_172(ppc_cpu *c) { lmacc_run(&lm_table[172], c); }
static void lmt_173(ppc_cpu *c) { lmacc_run(&lm_table[173], c); }
static void lmt_174(ppc_cpu *c) { lmacc_run(&lm_table[174], c); }
static void lmt_175(ppc_cpu *c) { lmacc_run(&lm_table[175], c); }
static void lmt_176(ppc_cpu *c) { lmacc_run(&lm_table[176], c); }
static void lmt_177(ppc_cpu *c) { lmacc_run(&lm_table[177], c); }
static void lmt_178(ppc_cpu *c) { lmacc_run(&lm_table[178], c); }
static void lmt_179(ppc_cpu *c) { lmacc_run(&lm_table[179], c); }
static void lmt_180(ppc_cpu *c) { lmacc_run(&lm_table[180], c); }
static void lmt_181(ppc_cpu *c) { lmacc_run(&lm_table[181], c); }
static void lmt_182(ppc_cpu *c) { lmacc_run(&lm_table[182], c); }
static void lmt_183(ppc_cpu *c) { lmacc_run(&lm_table[183], c); }
static void lmt_184(ppc_cpu *c) { lmacc_run(&lm_table[184], c); }
static void lmt_185(ppc_cpu *c) { lmacc_run(&lm_table[185], c); }
static void lmt_186(ppc_cpu *c) { lmacc_run(&lm_table[186], c); }
static void lmt_187(ppc_cpu *c) { lmacc_run(&lm_table[187], c); }
static void lmt_188(ppc_cpu *c) { lmacc_run(&lm_table[188], c); }
static void lmt_189(ppc_cpu *c) { lmacc_run(&lm_table[189], c); }
static void lmt_190(ppc_cpu *c) { lmacc_run(&lm_table[190], c); }
static void lmt_191(ppc_cpu *c) { lmacc_run(&lm_table[191], c); }
static void lmt_192(ppc_cpu *c) { lmacc_run(&lm_table[192], c); }
static void lmt_193(ppc_cpu *c) { lmacc_run(&lm_table[193], c); }
static void lmt_194(ppc_cpu *c) { lmacc_run(&lm_table[194], c); }
static void lmt_195(ppc_cpu *c) { lmacc_run(&lm_table[195], c); }
static void lmt_196(ppc_cpu *c) { lmacc_run(&lm_table[196], c); }
static void lmt_197(ppc_cpu *c) { lmacc_run(&lm_table[197], c); }
static void lmt_198(ppc_cpu *c) { lmacc_run(&lm_table[198], c); }
static void lmt_199(ppc_cpu *c) { lmacc_run(&lm_table[199], c); }
static void lmt_200(ppc_cpu *c) { lmacc_run(&lm_table[200], c); }
static void lmt_201(ppc_cpu *c) { lmacc_run(&lm_table[201], c); }
static void lmt_202(ppc_cpu *c) { lmacc_run(&lm_table[202], c); }
static void lmt_203(ppc_cpu *c) { lmacc_run(&lm_table[203], c); }
static void lmt_204(ppc_cpu *c) { lmacc_run(&lm_table[204], c); }
static void lmt_205(ppc_cpu *c) { lmacc_run(&lm_table[205], c); }
static void lmt_206(ppc_cpu *c) { lmacc_run(&lm_table[206], c); }
static void lmt_207(ppc_cpu *c) { lmacc_run(&lm_table[207], c); }
static void lmt_208(ppc_cpu *c) { lmacc_run(&lm_table[208], c); }
static void lmt_209(ppc_cpu *c) { lmacc_run(&lm_table[209], c); }
static void lmt_210(ppc_cpu *c) { lmacc_run(&lm_table[210], c); }
static void lmt_211(ppc_cpu *c) { lmacc_run(&lm_table[211], c); }
static void lmt_212(ppc_cpu *c) { lmacc_run(&lm_table[212], c); }
static void lmt_213(ppc_cpu *c) { lmacc_run(&lm_table[213], c); }
static void lmt_214(ppc_cpu *c) { lmacc_run(&lm_table[214], c); }
static void lmt_215(ppc_cpu *c) { lmacc_run(&lm_table[215], c); }
static void lmt_216(ppc_cpu *c) { lmacc_run(&lm_table[216], c); }
static void lmt_217(ppc_cpu *c) { lmacc_run(&lm_table[217], c); }
static void lmt_218(ppc_cpu *c) { lmacc_run(&lm_table[218], c); }
static void lmt_219(ppc_cpu *c) { lmacc_run(&lm_table[219], c); }
static void lmt_220(ppc_cpu *c) { lmacc_run(&lm_table[220], c); }
static void lmt_221(ppc_cpu *c) { lmacc_run(&lm_table[221], c); }
static void lmt_222(ppc_cpu *c) { lmacc_run(&lm_table[222], c); }
static void lmt_223(ppc_cpu *c) { lmacc_run(&lm_table[223], c); }
static void lmt_224(ppc_cpu *c) { lmacc_run(&lm_table[224], c); }
static void lmt_225(ppc_cpu *c) { lmacc_run(&lm_table[225], c); }
static void lmt_226(ppc_cpu *c) { lmacc_run(&lm_table[226], c); }
static void lmt_227(ppc_cpu *c) { lmacc_run(&lm_table[227], c); }
static void lmt_228(ppc_cpu *c) { lmacc_run(&lm_table[228], c); }
static void lmt_229(ppc_cpu *c) { lmacc_run(&lm_table[229], c); }
static void lmt_230(ppc_cpu *c) { lmacc_run(&lm_table[230], c); }
static void lmt_231(ppc_cpu *c) { lmacc_run(&lm_table[231], c); }
static void lmt_232(ppc_cpu *c) { lmacc_run(&lm_table[232], c); }
static void lmt_233(ppc_cpu *c) { lmacc_run(&lm_table[233], c); }
static void lmt_234(ppc_cpu *c) { lmacc_run(&lm_table[234], c); }
static void lmt_235(ppc_cpu *c) { lmacc_run(&lm_table[235], c); }
static void lmt_236(ppc_cpu *c) { lmacc_run(&lm_table[236], c); }
static void lmt_237(ppc_cpu *c) { lmacc_run(&lm_table[237], c); }
static void lmt_238(ppc_cpu *c) { lmacc_run(&lm_table[238], c); }
static void lmt_239(ppc_cpu *c) { lmacc_run(&lm_table[239], c); }
static void lmt_240(ppc_cpu *c) { lmacc_run(&lm_table[240], c); }
static void lmt_241(ppc_cpu *c) { lmacc_run(&lm_table[241], c); }
static void lmt_242(ppc_cpu *c) { lmacc_run(&lm_table[242], c); }
static void lmt_243(ppc_cpu *c) { lmacc_run(&lm_table[243], c); }
static void lmt_244(ppc_cpu *c) { lmacc_run(&lm_table[244], c); }
static void lmt_245(ppc_cpu *c) { lmacc_run(&lm_table[245], c); }
static void lmt_246(ppc_cpu *c) { lmacc_run(&lm_table[246], c); }
static void lmt_247(ppc_cpu *c) { lmacc_run(&lm_table[247], c); }
static void lmt_248(ppc_cpu *c) { lmacc_run(&lm_table[248], c); }
static void lmt_249(ppc_cpu *c) { lmacc_run(&lm_table[249], c); }
static void lmt_250(ppc_cpu *c) { lmacc_run(&lm_table[250], c); }
static void lmt_251(ppc_cpu *c) { lmacc_run(&lm_table[251], c); }
static void lmt_252(ppc_cpu *c) { lmacc_run(&lm_table[252], c); }
static void lmt_253(ppc_cpu *c) { lmacc_run(&lm_table[253], c); }
static void lmt_254(ppc_cpu *c) { lmacc_run(&lm_table[254], c); }
static void lmt_255(ppc_cpu *c) { lmacc_run(&lm_table[255], c); }
static void lmt_256(ppc_cpu *c) { lmacc_run(&lm_table[256], c); }
static void lmt_257(ppc_cpu *c) { lmacc_run(&lm_table[257], c); }
static void lmt_258(ppc_cpu *c) { lmacc_run(&lm_table[258], c); }
static void lmt_259(ppc_cpu *c) { lmacc_run(&lm_table[259], c); }
static void lmt_260(ppc_cpu *c) { lmacc_run(&lm_table[260], c); }
static void lmt_261(ppc_cpu *c) { lmacc_run(&lm_table[261], c); }
static void lmt_262(ppc_cpu *c) { lmacc_run(&lm_table[262], c); }
static void lmt_263(ppc_cpu *c) { lmacc_run(&lm_table[263], c); }
static void lmt_264(ppc_cpu *c) { lmacc_run(&lm_table[264], c); }
static void lmt_265(ppc_cpu *c) { lmacc_run(&lm_table[265], c); }
static void lmt_266(ppc_cpu *c) { lmacc_run(&lm_table[266], c); }
static void lmt_267(ppc_cpu *c) { lmacc_run(&lm_table[267], c); }
static void lmt_268(ppc_cpu *c) { lmacc_run(&lm_table[268], c); }
static void lmt_269(ppc_cpu *c) { lmacc_run(&lm_table[269], c); }
static void lmt_270(ppc_cpu *c) { lmacc_run(&lm_table[270], c); }
static void lmt_271(ppc_cpu *c) { lmacc_run(&lm_table[271], c); }
static void lmt_272(ppc_cpu *c) { lmacc_run(&lm_table[272], c); }
static void lmt_273(ppc_cpu *c) { lmacc_run(&lm_table[273], c); }
static void lmt_274(ppc_cpu *c) { lmacc_run(&lm_table[274], c); }
static void lmt_275(ppc_cpu *c) { lmacc_run(&lm_table[275], c); }
static void lmt_276(ppc_cpu *c) { lmacc_run(&lm_table[276], c); }
static void lmt_277(ppc_cpu *c) { lmacc_run(&lm_table[277], c); }
static void lmt_278(ppc_cpu *c) { lmacc_run(&lm_table[278], c); }
static void lmt_279(ppc_cpu *c) { lmacc_run(&lm_table[279], c); }
static void lmt_280(ppc_cpu *c) { lmacc_run(&lm_table[280], c); }
static void lmt_281(ppc_cpu *c) { lmacc_run(&lm_table[281], c); }
static void lmt_282(ppc_cpu *c) { lmacc_run(&lm_table[282], c); }
static void lmt_283(ppc_cpu *c) { lmacc_run(&lm_table[283], c); }
static void lmt_284(ppc_cpu *c) { lmacc_run(&lm_table[284], c); }
static void lmt_285(ppc_cpu *c) { lmacc_run(&lm_table[285], c); }
static void lmt_286(ppc_cpu *c) { lmacc_run(&lm_table[286], c); }
static void lmt_287(ppc_cpu *c) { lmacc_run(&lm_table[287], c); }
static void lmt_288(ppc_cpu *c) { lmacc_run(&lm_table[288], c); }
static void lmt_289(ppc_cpu *c) { lmacc_run(&lm_table[289], c); }
static void lmt_290(ppc_cpu *c) { lmacc_run(&lm_table[290], c); }
static void lmt_291(ppc_cpu *c) { lmacc_run(&lm_table[291], c); }
static void lmt_292(ppc_cpu *c) { lmacc_run(&lm_table[292], c); }
static void lmt_293(ppc_cpu *c) { lmacc_run(&lm_table[293], c); }
static void lmt_294(ppc_cpu *c) { lmacc_run(&lm_table[294], c); }
static void lmt_295(ppc_cpu *c) { lmacc_run(&lm_table[295], c); }
static void lmt_296(ppc_cpu *c) { lmacc_run(&lm_table[296], c); }
static void lmt_297(ppc_cpu *c) { lmacc_run(&lm_table[297], c); }
static void lmt_298(ppc_cpu *c) { lmacc_run(&lm_table[298], c); }
static void lmt_299(ppc_cpu *c) { lmacc_run(&lm_table[299], c); }
static void lmt_300(ppc_cpu *c) { lmacc_run(&lm_table[300], c); }
static void lmt_301(ppc_cpu *c) { lmacc_run(&lm_table[301], c); }
static void lmt_302(ppc_cpu *c) { lmacc_run(&lm_table[302], c); }
static void lmt_303(ppc_cpu *c) { lmacc_run(&lm_table[303], c); }
static void lmt_304(ppc_cpu *c) { lmacc_run(&lm_table[304], c); }
static void lmt_305(ppc_cpu *c) { lmacc_run(&lm_table[305], c); }
static void lmt_306(ppc_cpu *c) { lmacc_run(&lm_table[306], c); }
static void lmt_307(ppc_cpu *c) { lmacc_run(&lm_table[307], c); }
static void lmt_308(ppc_cpu *c) { lmacc_run(&lm_table[308], c); }
static void lmt_309(ppc_cpu *c) { lmacc_run(&lm_table[309], c); }
static void lmt_310(ppc_cpu *c) { lmacc_run(&lm_table[310], c); }
static void lmt_311(ppc_cpu *c) { lmacc_run(&lm_table[311], c); }
static void lmt_312(ppc_cpu *c) { lmacc_run(&lm_table[312], c); }
static void lmt_313(ppc_cpu *c) { lmacc_run(&lm_table[313], c); }
static void lmt_314(ppc_cpu *c) { lmacc_run(&lm_table[314], c); }
static void lmt_315(ppc_cpu *c) { lmacc_run(&lm_table[315], c); }
static void lmt_316(ppc_cpu *c) { lmacc_run(&lm_table[316], c); }
static void lmt_317(ppc_cpu *c) { lmacc_run(&lm_table[317], c); }
static void lmt_318(ppc_cpu *c) { lmacc_run(&lm_table[318], c); }
static void lmt_319(ppc_cpu *c) { lmacc_run(&lm_table[319], c); }
static void lmt_320(ppc_cpu *c) { lmacc_run(&lm_table[320], c); }
static void lmt_321(ppc_cpu *c) { lmacc_run(&lm_table[321], c); }
static void lmt_322(ppc_cpu *c) { lmacc_run(&lm_table[322], c); }
static void lmt_323(ppc_cpu *c) { lmacc_run(&lm_table[323], c); }
static void lmt_324(ppc_cpu *c) { lmacc_run(&lm_table[324], c); }
static void lmt_325(ppc_cpu *c) { lmacc_run(&lm_table[325], c); }
static void lmt_326(ppc_cpu *c) { lmacc_run(&lm_table[326], c); }
static void lmt_327(ppc_cpu *c) { lmacc_run(&lm_table[327], c); }
static void lmt_328(ppc_cpu *c) { lmacc_run(&lm_table[328], c); }
static void lmt_329(ppc_cpu *c) { lmacc_run(&lm_table[329], c); }
static void lmt_330(ppc_cpu *c) { lmacc_run(&lm_table[330], c); }
static void lmt_331(ppc_cpu *c) { lmacc_run(&lm_table[331], c); }
static void lmt_332(ppc_cpu *c) { lmacc_run(&lm_table[332], c); }
static void lmt_333(ppc_cpu *c) { lmacc_run(&lm_table[333], c); }
static void lmt_334(ppc_cpu *c) { lmacc_run(&lm_table[334], c); }
static void lmt_335(ppc_cpu *c) { lmacc_run(&lm_table[335], c); }
static void lmt_336(ppc_cpu *c) { lmacc_run(&lm_table[336], c); }
static void lmt_337(ppc_cpu *c) { lmacc_run(&lm_table[337], c); }
static void lmt_338(ppc_cpu *c) { lmacc_run(&lm_table[338], c); }
static void lmt_339(ppc_cpu *c) { lmacc_run(&lm_table[339], c); }
static void lmt_340(ppc_cpu *c) { lmacc_run(&lm_table[340], c); }
static void lmt_341(ppc_cpu *c) { lmacc_run(&lm_table[341], c); }
static void lmt_342(ppc_cpu *c) { lmacc_run(&lm_table[342], c); }
static void lmt_343(ppc_cpu *c) { lmacc_run(&lm_table[343], c); }
static void lmt_344(ppc_cpu *c) { lmacc_run(&lm_table[344], c); }
static void lmt_345(ppc_cpu *c) { lmacc_run(&lm_table[345], c); }
static void lmt_346(ppc_cpu *c) { lmacc_run(&lm_table[346], c); }
static void lmt_347(ppc_cpu *c) { lmacc_run(&lm_table[347], c); }
static void lmt_348(ppc_cpu *c) { lmacc_run(&lm_table[348], c); }
static void lmt_349(ppc_cpu *c) { lmacc_run(&lm_table[349], c); }
static void lmt_350(ppc_cpu *c) { lmacc_run(&lm_table[350], c); }
static void lmt_351(ppc_cpu *c) { lmacc_run(&lm_table[351], c); }
static void lmt_352(ppc_cpu *c) { lmacc_run(&lm_table[352], c); }
static void lmt_353(ppc_cpu *c) { lmacc_run(&lm_table[353], c); }
static void lmt_354(ppc_cpu *c) { lmacc_run(&lm_table[354], c); }
static void lmt_355(ppc_cpu *c) { lmacc_run(&lm_table[355], c); }
static void lmt_356(ppc_cpu *c) { lmacc_run(&lm_table[356], c); }
static void lmt_357(ppc_cpu *c) { lmacc_run(&lm_table[357], c); }
static void lmt_358(ppc_cpu *c) { lmacc_run(&lm_table[358], c); }
static void lmt_359(ppc_cpu *c) { lmacc_run(&lm_table[359], c); }
static void lmt_360(ppc_cpu *c) { lmacc_run(&lm_table[360], c); }
static void lmt_361(ppc_cpu *c) { lmacc_run(&lm_table[361], c); }
static void lmt_362(ppc_cpu *c) { lmacc_run(&lm_table[362], c); }
static void lmt_363(ppc_cpu *c) { lmacc_run(&lm_table[363], c); }
static void lmt_364(ppc_cpu *c) { lmacc_run(&lm_table[364], c); }
static void lmt_365(ppc_cpu *c) { lmacc_run(&lm_table[365], c); }
static void lmt_366(ppc_cpu *c) { lmacc_run(&lm_table[366], c); }
static void lmt_367(ppc_cpu *c) { lmacc_run(&lm_table[367], c); }
static void lmt_368(ppc_cpu *c) { lmacc_run(&lm_table[368], c); }
static void lmt_369(ppc_cpu *c) { lmacc_run(&lm_table[369], c); }
static void lmt_370(ppc_cpu *c) { lmacc_run(&lm_table[370], c); }
static void lmt_371(ppc_cpu *c) { lmacc_run(&lm_table[371], c); }
static void lmt_372(ppc_cpu *c) { lmacc_run(&lm_table[372], c); }
static void lmt_373(ppc_cpu *c) { lmacc_run(&lm_table[373], c); }
static void lmt_374(ppc_cpu *c) { lmacc_run(&lm_table[374], c); }
static void lmt_375(ppc_cpu *c) { lmacc_run(&lm_table[375], c); }
static void lmt_376(ppc_cpu *c) { lmacc_run(&lm_table[376], c); }
static void lmt_377(ppc_cpu *c) { lmacc_run(&lm_table[377], c); }
static void lmt_378(ppc_cpu *c) { lmacc_run(&lm_table[378], c); }
static void lmt_379(ppc_cpu *c) { lmacc_run(&lm_table[379], c); }
static void lmt_380(ppc_cpu *c) { lmacc_run(&lm_table[380], c); }
static void lmt_381(ppc_cpu *c) { lmacc_run(&lm_table[381], c); }
static void lmt_382(ppc_cpu *c) { lmacc_run(&lm_table[382], c); }
static void lmt_383(ppc_cpu *c) { lmacc_run(&lm_table[383], c); }
static void lmt_384(ppc_cpu *c) { lmacc_run(&lm_table[384], c); }
static void lmt_385(ppc_cpu *c) { lmacc_run(&lm_table[385], c); }
static void lmt_386(ppc_cpu *c) { lmacc_run(&lm_table[386], c); }
static void lmt_387(ppc_cpu *c) { lmacc_run(&lm_table[387], c); }
static void lmt_388(ppc_cpu *c) { lmacc_run(&lm_table[388], c); }
static void lmt_389(ppc_cpu *c) { lmacc_run(&lm_table[389], c); }
static void lmt_390(ppc_cpu *c) { lmacc_run(&lm_table[390], c); }
static void lmt_391(ppc_cpu *c) { lmacc_run(&lm_table[391], c); }
static void lmt_392(ppc_cpu *c) { lmacc_run(&lm_table[392], c); }
static void lmt_393(ppc_cpu *c) { lmacc_run(&lm_table[393], c); }
static void lmt_394(ppc_cpu *c) { lmacc_run(&lm_table[394], c); }
static void lmt_395(ppc_cpu *c) { lmacc_run(&lm_table[395], c); }
static void lmt_396(ppc_cpu *c) { lmacc_run(&lm_table[396], c); }
static void lmt_397(ppc_cpu *c) { lmacc_run(&lm_table[397], c); }
static void lmt_398(ppc_cpu *c) { lmacc_run(&lm_table[398], c); }
static void lmt_399(ppc_cpu *c) { lmacc_run(&lm_table[399], c); }
static void lmt_400(ppc_cpu *c) { lmacc_run(&lm_table[400], c); }
static void lmt_401(ppc_cpu *c) { lmacc_run(&lm_table[401], c); }
static void lmt_402(ppc_cpu *c) { lmacc_run(&lm_table[402], c); }
static void lmt_403(ppc_cpu *c) { lmacc_run(&lm_table[403], c); }
static void lmt_404(ppc_cpu *c) { lmacc_run(&lm_table[404], c); }
static void lmt_405(ppc_cpu *c) { lmacc_run(&lm_table[405], c); }
static void lmt_406(ppc_cpu *c) { lmacc_run(&lm_table[406], c); }
static void lmt_407(ppc_cpu *c) { lmacc_run(&lm_table[407], c); }
static void lmt_408(ppc_cpu *c) { lmacc_run(&lm_table[408], c); }
static void lmt_409(ppc_cpu *c) { lmacc_run(&lm_table[409], c); }
static void lmt_410(ppc_cpu *c) { lmacc_run(&lm_table[410], c); }
static void lmt_411(ppc_cpu *c) { lmacc_run(&lm_table[411], c); }
static void lmt_412(ppc_cpu *c) { lmacc_run(&lm_table[412], c); }
static void lmt_413(ppc_cpu *c) { lmacc_run(&lm_table[413], c); }
static void lmt_414(ppc_cpu *c) { lmacc_run(&lm_table[414], c); }
static void lmt_415(ppc_cpu *c) { lmacc_run(&lm_table[415], c); }
static void lmt_416(ppc_cpu *c) { lmacc_run(&lm_table[416], c); }
static void lmt_417(ppc_cpu *c) { lmacc_run(&lm_table[417], c); }
static void lmt_418(ppc_cpu *c) { lmacc_run(&lm_table[418], c); }
static void lmt_419(ppc_cpu *c) { lmacc_run(&lm_table[419], c); }
static void lmt_420(ppc_cpu *c) { lmacc_run(&lm_table[420], c); }
static void lmt_421(ppc_cpu *c) { lmacc_run(&lm_table[421], c); }
static void lmt_422(ppc_cpu *c) { lmacc_run(&lm_table[422], c); }
static void lmt_423(ppc_cpu *c) { lmacc_run(&lm_table[423], c); }
static void lmt_424(ppc_cpu *c) { lmacc_run(&lm_table[424], c); }
static void lmt_425(ppc_cpu *c) { lmacc_run(&lm_table[425], c); }
static void lmt_426(ppc_cpu *c) { lmacc_run(&lm_table[426], c); }
static void lmt_427(ppc_cpu *c) { lmacc_run(&lm_table[427], c); }
static void lmt_428(ppc_cpu *c) { lmacc_run(&lm_table[428], c); }
static void lmt_429(ppc_cpu *c) { lmacc_run(&lm_table[429], c); }
static void lmt_430(ppc_cpu *c) { lmacc_run(&lm_table[430], c); }
static void lmt_431(ppc_cpu *c) { lmacc_run(&lm_table[431], c); }
static void lmt_432(ppc_cpu *c) { lmacc_run(&lm_table[432], c); }
static void lmt_433(ppc_cpu *c) { lmacc_run(&lm_table[433], c); }
static void lmt_434(ppc_cpu *c) { lmacc_run(&lm_table[434], c); }
static void lmt_435(ppc_cpu *c) { lmacc_run(&lm_table[435], c); }
static void lmt_436(ppc_cpu *c) { lmacc_run(&lm_table[436], c); }
static void lmt_437(ppc_cpu *c) { lmacc_run(&lm_table[437], c); }
static void lmt_438(ppc_cpu *c) { lmacc_run(&lm_table[438], c); }
static void lmt_439(ppc_cpu *c) { lmacc_run(&lm_table[439], c); }
static void lmt_440(ppc_cpu *c) { lmacc_run(&lm_table[440], c); }
static void lmt_441(ppc_cpu *c) { lmacc_run(&lm_table[441], c); }
static void lmt_442(ppc_cpu *c) { lmacc_run(&lm_table[442], c); }
static void lmt_443(ppc_cpu *c) { lmacc_run(&lm_table[443], c); }
static void lmt_444(ppc_cpu *c) { lmacc_run(&lm_table[444], c); }
static void lmt_445(ppc_cpu *c) { lmacc_run(&lm_table[445], c); }
static void lmt_446(ppc_cpu *c) { lmacc_run(&lm_table[446], c); }
static void lmt_447(ppc_cpu *c) { lmacc_run(&lm_table[447], c); }
static void lmt_448(ppc_cpu *c) { lmacc_run(&lm_table[448], c); }
static void lmt_449(ppc_cpu *c) { lmacc_run(&lm_table[449], c); }
static void lmt_450(ppc_cpu *c) { lmacc_run(&lm_table[450], c); }
static void lmt_451(ppc_cpu *c) { lmacc_run(&lm_table[451], c); }
static void lmt_452(ppc_cpu *c) { lmacc_run(&lm_table[452], c); }
static void lmt_453(ppc_cpu *c) { lmacc_run(&lm_table[453], c); }
static void lmt_454(ppc_cpu *c) { lmacc_run(&lm_table[454], c); }
static void lmt_455(ppc_cpu *c) { lmacc_run(&lm_table[455], c); }
static void lmt_456(ppc_cpu *c) { lmacc_run(&lm_table[456], c); }
static void lmt_457(ppc_cpu *c) { lmacc_run(&lm_table[457], c); }
static void lmt_458(ppc_cpu *c) { lmacc_run(&lm_table[458], c); }
static void lmt_459(ppc_cpu *c) { lmacc_run(&lm_table[459], c); }
static void lmt_460(ppc_cpu *c) { lmacc_run(&lm_table[460], c); }
static void lmt_461(ppc_cpu *c) { lmacc_run(&lm_table[461], c); }
static void lmt_462(ppc_cpu *c) { lmacc_run(&lm_table[462], c); }
static void lmt_463(ppc_cpu *c) { lmacc_run(&lm_table[463], c); }
static void lmt_464(ppc_cpu *c) { lmacc_run(&lm_table[464], c); }
static void lmt_465(ppc_cpu *c) { lmacc_run(&lm_table[465], c); }
static void lmt_466(ppc_cpu *c) { lmacc_run(&lm_table[466], c); }
static void lmt_467(ppc_cpu *c) { lmacc_run(&lm_table[467], c); }
static void lmt_468(ppc_cpu *c) { lmacc_run(&lm_table[468], c); }
static void lmt_469(ppc_cpu *c) { lmacc_run(&lm_table[469], c); }
static void lmt_470(ppc_cpu *c) { lmacc_run(&lm_table[470], c); }
static void lmt_471(ppc_cpu *c) { lmacc_run(&lm_table[471], c); }
static void lmt_472(ppc_cpu *c) { lmacc_run(&lm_table[472], c); }
static void lmt_473(ppc_cpu *c) { lmacc_run(&lm_table[473], c); }
static void lmt_474(ppc_cpu *c) { lmacc_run(&lm_table[474], c); }
static void lmt_475(ppc_cpu *c) { lmacc_run(&lm_table[475], c); }
static void lmt_476(ppc_cpu *c) { lmacc_run(&lm_table[476], c); }
static void lmt_477(ppc_cpu *c) { lmacc_run(&lm_table[477], c); }
static void lmt_478(ppc_cpu *c) { lmacc_run(&lm_table[478], c); }
static void lmt_479(ppc_cpu *c) { lmacc_run(&lm_table[479], c); }
static void lmt_480(ppc_cpu *c) { lmacc_run(&lm_table[480], c); }
static void lmt_481(ppc_cpu *c) { lmacc_run(&lm_table[481], c); }
static void lmt_482(ppc_cpu *c) { lmacc_run(&lm_table[482], c); }
static void lmt_483(ppc_cpu *c) { lmacc_run(&lm_table[483], c); }
static void lmt_484(ppc_cpu *c) { lmacc_run(&lm_table[484], c); }
static void lmt_485(ppc_cpu *c) { lmacc_run(&lm_table[485], c); }
static void lmt_486(ppc_cpu *c) { lmacc_run(&lm_table[486], c); }
static void lmt_487(ppc_cpu *c) { lmacc_run(&lm_table[487], c); }
static void lmt_488(ppc_cpu *c) { lmacc_run(&lm_table[488], c); }
static void lmt_489(ppc_cpu *c) { lmacc_run(&lm_table[489], c); }
static void lmt_490(ppc_cpu *c) { lmacc_run(&lm_table[490], c); }
static void lmt_491(ppc_cpu *c) { lmacc_run(&lm_table[491], c); }
static void lmt_492(ppc_cpu *c) { lmacc_run(&lm_table[492], c); }
static void lmt_493(ppc_cpu *c) { lmacc_run(&lm_table[493], c); }
static void lmt_494(ppc_cpu *c) { lmacc_run(&lm_table[494], c); }
static void lmt_495(ppc_cpu *c) { lmacc_run(&lm_table[495], c); }
static void lmt_496(ppc_cpu *c) { lmacc_run(&lm_table[496], c); }
static void lmt_497(ppc_cpu *c) { lmacc_run(&lm_table[497], c); }
static void lmt_498(ppc_cpu *c) { lmacc_run(&lm_table[498], c); }
static void lmt_499(ppc_cpu *c) { lmacc_run(&lm_table[499], c); }
static void lmt_500(ppc_cpu *c) { lmacc_run(&lm_table[500], c); }
static void lmt_501(ppc_cpu *c) { lmacc_run(&lm_table[501], c); }
static void lmt_502(ppc_cpu *c) { lmacc_run(&lm_table[502], c); }
static void lmt_503(ppc_cpu *c) { lmacc_run(&lm_table[503], c); }
static void lmt_504(ppc_cpu *c) { lmacc_run(&lm_table[504], c); }
static void lmt_505(ppc_cpu *c) { lmacc_run(&lm_table[505], c); }
static void lmt_506(ppc_cpu *c) { lmacc_run(&lm_table[506], c); }
static void lmt_507(ppc_cpu *c) { lmacc_run(&lm_table[507], c); }
static void lmt_508(ppc_cpu *c) { lmacc_run(&lm_table[508], c); }
static void lmt_509(ppc_cpu *c) { lmacc_run(&lm_table[509], c); }
static void lmt_510(ppc_cpu *c) { lmacc_run(&lm_table[510], c); }
static void lmt_511(ppc_cpu *c) { lmacc_run(&lm_table[511], c); }
static void lmt_512(ppc_cpu *c) { lmacc_run(&lm_table[512], c); }
static void lmt_513(ppc_cpu *c) { lmacc_run(&lm_table[513], c); }
static void lmt_514(ppc_cpu *c) { lmacc_run(&lm_table[514], c); }
static void lmt_515(ppc_cpu *c) { lmacc_run(&lm_table[515], c); }
static void lmt_516(ppc_cpu *c) { lmacc_run(&lm_table[516], c); }
static void lmt_517(ppc_cpu *c) { lmacc_run(&lm_table[517], c); }
static void lmt_518(ppc_cpu *c) { lmacc_run(&lm_table[518], c); }
static void lmt_519(ppc_cpu *c) { lmacc_run(&lm_table[519], c); }
static void lmt_520(ppc_cpu *c) { lmacc_run(&lm_table[520], c); }
static void lmt_521(ppc_cpu *c) { lmacc_run(&lm_table[521], c); }
static void lmt_522(ppc_cpu *c) { lmacc_run(&lm_table[522], c); }
static void lmt_523(ppc_cpu *c) { lmacc_run(&lm_table[523], c); }
static void lmt_524(ppc_cpu *c) { lmacc_run(&lm_table[524], c); }
static void lmt_525(ppc_cpu *c) { lmacc_run(&lm_table[525], c); }
static void lmt_526(ppc_cpu *c) { lmacc_run(&lm_table[526], c); }
static void lmt_527(ppc_cpu *c) { lmacc_run(&lm_table[527], c); }
static void lmt_528(ppc_cpu *c) { lmacc_run(&lm_table[528], c); }
static void lmt_529(ppc_cpu *c) { lmacc_run(&lm_table[529], c); }
static void lmt_530(ppc_cpu *c) { lmacc_run(&lm_table[530], c); }
static void lmt_531(ppc_cpu *c) { lmacc_run(&lm_table[531], c); }
static void lmt_532(ppc_cpu *c) { lmacc_run(&lm_table[532], c); }
static void lmt_533(ppc_cpu *c) { lmacc_run(&lm_table[533], c); }
static void lmt_534(ppc_cpu *c) { lmacc_run(&lm_table[534], c); }
static void lmt_535(ppc_cpu *c) { lmacc_run(&lm_table[535], c); }
static void lmt_536(ppc_cpu *c) { lmacc_run(&lm_table[536], c); }
static void lmt_537(ppc_cpu *c) { lmacc_run(&lm_table[537], c); }
static void lmt_538(ppc_cpu *c) { lmacc_run(&lm_table[538], c); }
static void lmt_539(ppc_cpu *c) { lmacc_run(&lm_table[539], c); }
static void lmt_540(ppc_cpu *c) { lmacc_run(&lm_table[540], c); }
static void lmt_541(ppc_cpu *c) { lmacc_run(&lm_table[541], c); }
static void lmt_542(ppc_cpu *c) { lmacc_run(&lm_table[542], c); }
static void lmt_543(ppc_cpu *c) { lmacc_run(&lm_table[543], c); }
static void lmt_544(ppc_cpu *c) { lmacc_run(&lm_table[544], c); }
static void lmt_545(ppc_cpu *c) { lmacc_run(&lm_table[545], c); }
static void lmt_546(ppc_cpu *c) { lmacc_run(&lm_table[546], c); }
static void lmt_547(ppc_cpu *c) { lmacc_run(&lm_table[547], c); }
static void lmt_548(ppc_cpu *c) { lmacc_run(&lm_table[548], c); }
static void lmt_549(ppc_cpu *c) { lmacc_run(&lm_table[549], c); }
static void lmt_550(ppc_cpu *c) { lmacc_run(&lm_table[550], c); }
static void lmt_551(ppc_cpu *c) { lmacc_run(&lm_table[551], c); }
static void lmt_552(ppc_cpu *c) { lmacc_run(&lm_table[552], c); }
static void lmt_553(ppc_cpu *c) { lmacc_run(&lm_table[553], c); }
static void lmt_554(ppc_cpu *c) { lmacc_run(&lm_table[554], c); }
static void lmt_555(ppc_cpu *c) { lmacc_run(&lm_table[555], c); }
static void lmt_556(ppc_cpu *c) { lmacc_run(&lm_table[556], c); }
static void lmt_557(ppc_cpu *c) { lmacc_run(&lm_table[557], c); }
static void lmt_558(ppc_cpu *c) { lmacc_run(&lm_table[558], c); }
static void lmt_559(ppc_cpu *c) { lmacc_run(&lm_table[559], c); }
static void lmt_560(ppc_cpu *c) { lmacc_run(&lm_table[560], c); }
static void lmt_561(ppc_cpu *c) { lmacc_run(&lm_table[561], c); }
static void lmt_562(ppc_cpu *c) { lmacc_run(&lm_table[562], c); }
static void lmt_563(ppc_cpu *c) { lmacc_run(&lm_table[563], c); }
static void lmt_564(ppc_cpu *c) { lmacc_run(&lm_table[564], c); }
static void lmt_565(ppc_cpu *c) { lmacc_run(&lm_table[565], c); }
static void lmt_566(ppc_cpu *c) { lmacc_run(&lm_table[566], c); }
static void lmt_567(ppc_cpu *c) { lmacc_run(&lm_table[567], c); }
static void lmt_568(ppc_cpu *c) { lmacc_run(&lm_table[568], c); }
static void lmt_569(ppc_cpu *c) { lmacc_run(&lm_table[569], c); }
static void lmt_570(ppc_cpu *c) { lmacc_run(&lm_table[570], c); }
static void lmt_571(ppc_cpu *c) { lmacc_run(&lm_table[571], c); }
static void lmt_572(ppc_cpu *c) { lmacc_run(&lm_table[572], c); }
static void lmt_573(ppc_cpu *c) { lmacc_run(&lm_table[573], c); }
static void lmt_574(ppc_cpu *c) { lmacc_run(&lm_table[574], c); }
static void lmt_575(ppc_cpu *c) { lmacc_run(&lm_table[575], c); }
static void lmt_576(ppc_cpu *c) { lmacc_run(&lm_table[576], c); }
static void lmt_577(ppc_cpu *c) { lmacc_run(&lm_table[577], c); }
static void lmt_578(ppc_cpu *c) { lmacc_run(&lm_table[578], c); }
static void lmt_579(ppc_cpu *c) { lmacc_run(&lm_table[579], c); }
static void lmt_580(ppc_cpu *c) { lmacc_run(&lm_table[580], c); }
static void lmt_581(ppc_cpu *c) { lmacc_run(&lm_table[581], c); }
static void lmt_582(ppc_cpu *c) { lmacc_run(&lm_table[582], c); }
static void lmt_583(ppc_cpu *c) { lmacc_run(&lm_table[583], c); }
static void lmt_584(ppc_cpu *c) { lmacc_run(&lm_table[584], c); }
static void lmt_585(ppc_cpu *c) { lmacc_run(&lm_table[585], c); }
static void lmt_586(ppc_cpu *c) { lmacc_run(&lm_table[586], c); }
static void lmt_587(ppc_cpu *c) { lmacc_run(&lm_table[587], c); }
static void lmt_588(ppc_cpu *c) { lmacc_run(&lm_table[588], c); }
static void lmt_589(ppc_cpu *c) { lmacc_run(&lm_table[589], c); }
static void lmt_590(ppc_cpu *c) { lmacc_run(&lm_table[590], c); }
static void lmt_591(ppc_cpu *c) { lmacc_run(&lm_table[591], c); }
static void lmt_592(ppc_cpu *c) { lmacc_run(&lm_table[592], c); }
static void lmt_593(ppc_cpu *c) { lmacc_run(&lm_table[593], c); }
static void lmt_594(ppc_cpu *c) { lmacc_run(&lm_table[594], c); }
static void lmt_595(ppc_cpu *c) { lmacc_run(&lm_table[595], c); }
static void lmt_596(ppc_cpu *c) { lmacc_run(&lm_table[596], c); }
static void lmt_597(ppc_cpu *c) { lmacc_run(&lm_table[597], c); }
static void lmt_598(ppc_cpu *c) { lmacc_run(&lm_table[598], c); }
static void lmt_599(ppc_cpu *c) { lmacc_run(&lm_table[599], c); }
static void lmt_600(ppc_cpu *c) { lmacc_run(&lm_table[600], c); }
static void lmt_601(ppc_cpu *c) { lmacc_run(&lm_table[601], c); }
static void lmt_602(ppc_cpu *c) { lmacc_run(&lm_table[602], c); }
static void lmt_603(ppc_cpu *c) { lmacc_run(&lm_table[603], c); }
static void lmt_604(ppc_cpu *c) { lmacc_run(&lm_table[604], c); }
static void lmt_605(ppc_cpu *c) { lmacc_run(&lm_table[605], c); }
static void lmt_606(ppc_cpu *c) { lmacc_run(&lm_table[606], c); }
static void lmt_607(ppc_cpu *c) { lmacc_run(&lm_table[607], c); }
static void lmt_608(ppc_cpu *c) { lmacc_run(&lm_table[608], c); }
static void lmt_609(ppc_cpu *c) { lmacc_run(&lm_table[609], c); }
static void lmt_610(ppc_cpu *c) { lmacc_run(&lm_table[610], c); }
static void lmt_611(ppc_cpu *c) { lmacc_run(&lm_table[611], c); }
static void lmt_612(ppc_cpu *c) { lmacc_run(&lm_table[612], c); }
static void lmt_613(ppc_cpu *c) { lmacc_run(&lm_table[613], c); }
static void lmt_614(ppc_cpu *c) { lmacc_run(&lm_table[614], c); }
static void lmt_615(ppc_cpu *c) { lmacc_run(&lm_table[615], c); }
static void lmt_616(ppc_cpu *c) { lmacc_run(&lm_table[616], c); }
static void lmt_617(ppc_cpu *c) { lmacc_run(&lm_table[617], c); }
static void lmt_618(ppc_cpu *c) { lmacc_run(&lm_table[618], c); }
static void lmt_619(ppc_cpu *c) { lmacc_run(&lm_table[619], c); }
static void lmt_620(ppc_cpu *c) { lmacc_run(&lm_table[620], c); }
static void lmt_621(ppc_cpu *c) { lmacc_run(&lm_table[621], c); }
static void lmt_622(ppc_cpu *c) { lmacc_run(&lm_table[622], c); }
static void lmt_623(ppc_cpu *c) { lmacc_run(&lm_table[623], c); }
static void lmt_624(ppc_cpu *c) { lmacc_run(&lm_table[624], c); }
static void lmt_625(ppc_cpu *c) { lmacc_run(&lm_table[625], c); }
static void lmt_626(ppc_cpu *c) { lmacc_run(&lm_table[626], c); }
static void lmt_627(ppc_cpu *c) { lmacc_run(&lm_table[627], c); }
static void lmt_628(ppc_cpu *c) { lmacc_run(&lm_table[628], c); }
static void lmt_629(ppc_cpu *c) { lmacc_run(&lm_table[629], c); }
static void lmt_630(ppc_cpu *c) { lmacc_run(&lm_table[630], c); }
static void lmt_631(ppc_cpu *c) { lmacc_run(&lm_table[631], c); }
static void lmt_632(ppc_cpu *c) { lmacc_run(&lm_table[632], c); }
static void lmt_633(ppc_cpu *c) { lmacc_run(&lm_table[633], c); }
static void lmt_634(ppc_cpu *c) { lmacc_run(&lm_table[634], c); }
static void lmt_635(ppc_cpu *c) { lmacc_run(&lm_table[635], c); }

const ppc_export lmacc_exports[] = {
    { "LMGetADBBase", lmt_0 },
    { "LMGetAGBHandle", lmt_1 },
    { "LMGetASCBase", lmt_2 },
    { "LMGetATalkHk1", lmt_3 },
    { "LMGetAddrMapFlags", lmt_4 },
    { "LMGetAddressError", lmt_5 },
    { "LMGetAlarmState", lmt_6 },
    { "LMGetApp2Packs", lmt_7 },
    { "LMGetAt2MenuBottom", lmt_8 },
    { "LMGetAutoInt1", lmt_9 },
    { "LMGetAutoInt2", lmt_10 },
    { "LMGetAutoInt3", lmt_11 },
    { "LMGetAutoInt4", lmt_12 },
    { "LMGetAutoInt5", lmt_13 },
    { "LMGetAutoInt6", lmt_14 },
    { "LMGetAutoInt7", lmt_15 },
    { "LMGetBackgrounderVars", lmt_16 },
    { "LMGetBootGlobPtr", lmt_17 },
    { "LMGetBootMask", lmt_18 },
    { "LMGetBootTmp8", lmt_19 },
    { "LMGetBoxFlag", lmt_20 },
    { "LMGetBrianBits", lmt_21 },
    { "LMGetBtDskRfn", lmt_22 },
    { "LMGetBusError", lmt_23 },
    { "LMGetCHKError", lmt_24 },
    { "LMGetCMVector", lmt_25 },
    { "LMGetCQDGlobals", lmt_26 },
    { "LMGetCarlByte", lmt_27 },
    { "LMGetChooserBits", lmt_28 },
    { "LMGetChunkyDepth", lmt_29 },
    { "LMGetCkdDB", lmt_30 },
    { "LMGetCloseOrnHook", lmt_31 },
    { "LMGetCoProcessor", lmt_32 },
    { "LMGetColLines", lmt_33 },
    { "LMGetCoreEditVars", lmt_34 },
    { "LMGetCrsrAddr", lmt_35 },
    { "LMGetCrsrBase", lmt_36 },
    { "LMGetCrsrCouple", lmt_37 },
    { "LMGetCrsrDevice", lmt_38 },
    { "LMGetCrsrNew", lmt_39 },
    { "LMGetCrsrObscure", lmt_40 },
    { "LMGetCrsrPtr", lmt_41 },
    { "LMGetCrsrRect", lmt_42 },
    { "LMGetCrsrRow", lmt_43 },
    { "LMGetCrsrScale", lmt_44 },
    { "LMGetCurDeKind", lmt_45 },
    { "LMGetCurDragAction", lmt_46 },
    { "LMGetCurFMInput", lmt_47 },
    { "LMGetCurLayer", lmt_48 },
    { "LMGetDSCtrAdj", lmt_49 },
    { "LMGetDSDrawProc", lmt_50 },
    { "LMGetDSPGlobals", lmt_51 },
    { "LMGetDSWndUpdate", lmt_52 },
    { "LMGetDeskPatEnable", lmt_53 },
    { "LMGetDeskPort", lmt_54 },
    { "LMGetDiskVars", lmt_55 },
    { "LMGetDivideByZero", lmt_56 },
    { "LMGetDockingGlobals", lmt_57 },
    { "LMGetDrMstrBlk", lmt_58 },
    { "LMGetDragFlag", lmt_59 },
    { "LMGetDrawCrsrVector", lmt_60 },
    { "LMGetDskErr", lmt_61 },
    { "LMGetDskRtnAdr", lmt_62 },
    { "LMGetDskSwtchHook", lmt_63 },
    { "LMGetDskVerify", lmt_64 },
    { "LMGetEgretGlobals", lmt_65 },
    { "LMGetEndSRTPtr", lmt_66 },
    { "LMGetErCode", lmt_67 },
    { "LMGetEraseCrsrVector", lmt_68 },
    { "LMGetEvtBufCnt", lmt_69 },
    { "LMGetExpandMem", lmt_70 },
    { "LMGetExtFSHook", lmt_71 },
    { "LMGetExtraPrint", lmt_72 },
    { "LMGetFDevDisable", lmt_73 },
    { "LMGetFLckUnlck", lmt_74 },
    { "LMGetFMDefaultSize", lmt_75 },
    { "LMGetFMDotsPerInch", lmt_76 },
    { "LMGetFMExist", lmt_77 },
    { "LMGetFMStyleTab", lmt_78 },
    { "LMGetFMSynth", lmt_79 },
    { "LMGetFMgrOutRec", lmt_80 },
    { "LMGetFONDID", lmt_81 },
    { "LMGetFP040Vects", lmt_82 },
    { "LMGetFP68881", lmt_83 },
    { "LMGetFPState", lmt_84 },
    { "LMGetFSMVars", lmt_85 },
    { "LMGetFSQueueHook", lmt_86 },
    { "LMGetFSTemp4", lmt_87 },
    { "LMGetFSTemp8", lmt_88 },
    { "LMGetFScaleHFact", lmt_89 },
    { "LMGetFScaleVFact", lmt_90 },
    { "LMGetFileShareVars", lmt_91 },
    { "LMGetFinder", lmt_92 },
    { "LMGetFlushOnly", lmt_93 },
    { "LMGetFondState", lmt_94 },
    { "LMGetFontFlag", lmt_95 },
    { "LMGetFormatError", lmt_96 },
    { "LMGetFourDHack", lmt_97 },
    { "LMGetFrcSync", lmt_98 },
    { "LMGetGZRootPtr", lmt_99 },
    { "LMGetHFSFlags", lmt_100 },
    { "LMGetHFSStkTop", lmt_101 },
    { "LMGetHSCHndl", lmt_102 },
    { "LMGetHiHeapMark", lmt_103 },
    { "LMGetHpChk", lmt_104 },
    { "LMGetIOPMgrVars", lmt_105 },
    { "LMGetIconBitmap", lmt_106 },
    { "LMGetIconTLAddr", lmt_107 },
    { "LMGetIllegalInstruction", lmt_108 },
    { "LMGetIntFlag", lmt_109 },
    { "LMGetJAllocCrsr", lmt_110 },
    { "LMGetJCheckLoad", lmt_111 },
    { "LMGetJDCDReset", lmt_112 },
    { "LMGetJDiskSel", lmt_113 },
    { "LMGetJDtrmV2", lmt_114 },
    { "LMGetJDtrmV3", lmt_115 },
    { "LMGetJFileOpen", lmt_116 },
    { "LMGetJFindDrive", lmt_117 },
    { "LMGetJFlushCache", lmt_118 },
    { "LMGetJKybdTask", lmt_119 },
    { "LMGetJOpcodeProc", lmt_120 },
    { "LMGetJSendCmd", lmt_121 },
    { "LMGetJSetCCrsr", lmt_122 },
    { "LMGetJShell", lmt_123 },
    { "LMGetJSwapMMU", lmt_124 },
    { "LMGetKbdVars", lmt_125 },
    { "LMGetKeyLast", lmt_126 },
    { "LMGetKeyMVars", lmt_127 },
    { "LMGetKeyRepTime", lmt_128 },
    { "LMGetLastBG", lmt_129 },
    { "LMGetLastBinPat", lmt_130 },
    { "LMGetLastDepth", lmt_131 },
    { "LMGetLastFore", lmt_132 },
    { "LMGetLastMode", lmt_133 },
    { "LMGetLastTxGDevice", lmt_134 },
    { "LMGetLaunchFlag", lmt_135 },
    { "LMGetLayerPalette", lmt_136 },
    { "LMGetLine1010", lmt_137 },
    { "LMGetLine1111", lmt_138 },
    { "LMGetLoadFiller", lmt_139 },
    { "LMGetLoadTrap", lmt_140 },
    { "LMGetLoaderPBlock", lmt_141 },
    { "LMGetLockMemCt", lmt_142 },
    { "LMGetMAErrProc", lmt_143 },
    { "LMGetMASuperTab", lmt_144 },
    { "LMGetMBFlash", lmt_145 },
    { "LMGetMBProcHndl", lmt_146 },
    { "LMGetMMDefFlags", lmt_147 },
    { "LMGetMMFlags", lmt_148 },
    { "LMGetMMU24Tbl", lmt_149 },
    { "LMGetMMUFlags", lmt_150 },
    { "LMGetMMUTblSize", lmt_151 },
    { "LMGetMacJmp", lmt_152 },
    { "LMGetMacJmpFlag", lmt_153 },
    { "LMGetMacZero", lmt_154 },
    { "LMGetMacsBugPC", lmt_155 },
    { "LMGetMacsBugSP", lmt_156 },
    { "LMGetMacsBugSR", lmt_157 },
    { "LMGetMaxDB", lmt_158 },
    { "LMGetMem2Vectors24", lmt_159 },
    { "LMGetMem2Vectors32", lmt_160 },
    { "LMGetMemVectors24", lmt_161 },
    { "LMGetMemVectors32", lmt_162 },
    { "LMGetMenuLayer", lmt_163 },
    { "LMGetMickeyBytes", lmt_164 },
    { "LMGetMimeGlobs", lmt_165 },
    { "LMGetMmInOK", lmt_166 },
    { "LMGetMonkeyLives", lmt_167 },
    { "LMGetMouse", lmt_168 },
    { "LMGetMouseMask", lmt_169 },
    { "LMGetMouseOffset", lmt_170 },
    { "LMGetNMIFlag", lmt_171 },
    { "LMGetNMQHdr", lmt_172 },
    { "LMGetNTSC", lmt_173 },
    { "LMGetNewMount", lmt_174 },
    { "LMGetNewUnused", lmt_175 },
    { "LMGetNxtDB", lmt_176 },
    { "LMGetOSTable", lmt_177 },
    { "LMGetOSTableSplit", lmt_178 },
    { "LMGetOSTableSplit2", lmt_179 },
    { "LMGetOSTableSplit3", lmt_180 },
    { "LMGetOSTableSplit4", lmt_181 },
    { "LMGetOSTableSplit5", lmt_182 },
    { "LMGetOldTimeSCSIDB", lmt_183 },
    { "LMGetOmegaSANE", lmt_184 },
    { "LMGetPMMU", lmt_185 },
    { "LMGetPMSPPtr", lmt_186 },
    { "LMGetPSCIntTbl", lmt_187 },
    { "LMGetPWMBuf1", lmt_188 },
    { "LMGetPWMBuf2", lmt_189 },
    { "LMGetPWMValue", lmt_190 },
    { "LMGetPadRsrc", lmt_191 },
    { "LMGetPadRsrc2", lmt_192 },
    { "LMGetParams", lmt_193 },
    { "LMGetPhys2Log", lmt_194 },
    { "LMGetPhysMemTop", lmt_195 },
    { "LMGetPmgrHandle", lmt_196 },
    { "LMGetPollProc", lmt_197 },
    { "LMGetPollRtnAddr", lmt_198 },
    { "LMGetPollStack", lmt_199 },
    { "LMGetPortAUse", lmt_200 },
    { "LMGetPowerMgrVars", lmt_201 },
    { "LMGetPrivilegeViolation", lmt_202 },
    { "LMGetQDErr", lmt_203 },
    { "LMGetRGBBlack", lmt_204 },
    { "LMGetRGBWhite", lmt_205 },
    { "LMGetRMGRHiVars", lmt_206 },
    { "LMGetRMGRPerm", lmt_207 },
    { "LMGetROMMapHndl", lmt_208 },
    { "LMGetRSDHndl", lmt_209 },
    { "LMGetRawMouse", lmt_210 },
    { "LMGetRealMemTop", lmt_211 },
    { "LMGetRegRsrc", lmt_212 },
    { "LMGetReqstVol", lmt_213 },
    { "LMGetResOneDeep", lmt_214 },
    { "LMGetResReadOnly", lmt_215 },
    { "LMGetReserved", lmt_216 },
    { "LMGetReserved2", lmt_217 },
    { "LMGetReset", lmt_218 },
    { "LMGetRowBits", lmt_219 },
    { "LMGetSCCASts", lmt_220 },
    { "LMGetSCCBSts", lmt_221 },
    { "LMGetSCCIOPFlag", lmt_222 },
    { "LMGetSCSI2Base", lmt_223 },
    { "LMGetSCSIBase", lmt_224 },
    { "LMGetSCSIDMA", lmt_225 },
    { "LMGetSCSIDrvrs", lmt_226 },
    { "LMGetSCSIGlobals", lmt_227 },
    { "LMGetSCSIHsk", lmt_228 },
    { "LMGetSCSIPoll", lmt_229 },
    { "LMGetSDMJmpTblPtr", lmt_230 },
    { "LMGetSEVarBase", lmt_231 },
    { "LMGetSEVarBase0", lmt_232 },
    { "LMGetSEVarBase1", lmt_233 },
    { "LMGetSEVarBase2", lmt_234 },
    { "LMGetSEVarBase3", lmt_235 },
    { "LMGetSEVarBase4", lmt_236 },
    { "LMGetSEVarBase5", lmt_237 },
    { "LMGetSInitFlags", lmt_238 },
    { "LMGetSMGlobals", lmt_239 },
    { "LMGetSPMisc1", lmt_240 },
    { "LMGetSaneTrapAddr", lmt_241 },
    { "LMGetSaveFondFlags", lmt_242 },
    { "LMGetSaveSP", lmt_243 },
    { "LMGetSaveSegHandle", lmt_244 },
    { "LMGetSavedHandle", lmt_245 },
    { "LMGetSavedHilite", lmt_246 },
    { "LMGetScrDmpType", lmt_247 },
    { "LMGetScrapTag", lmt_248 },
    { "LMGetScreenBytes", lmt_249 },
    { "LMGetScrnVBLPtr", lmt_250 },
    { "LMGetSegHiEnable", lmt_251 },
    { "LMGetSerialVars", lmt_252 },
    { "LMGetSetOSDefKey", lmt_253 },
    { "LMGetShieldDepth", lmt_254 },
    { "LMGetShieldRect", lmt_255 },
    { "LMGetShutDownQHdr", lmt_256 },
    { "LMGetSlotPrTbl", lmt_257 },
    { "LMGetSlotQDT", lmt_258 },
    { "LMGetSlotTicks", lmt_259 },
    { "LMGetSlotVBLQ", lmt_260 },
    { "LMGetSonyVars", lmt_261 },
    { "LMGetSoundDCE", lmt_262 },
    { "LMGetSoundGlue", lmt_263 },
    { "LMGetSoundVBL", lmt_264 },
    { "LMGetSpuriousInterrupt", lmt_265 },
    { "LMGetSrcDevice", lmt_266 },
    { "LMGetStkLowPt", lmt_267 },
    { "LMGetSwitcher", lmt_268 },
    { "LMGetSysEvtBuf", lmt_269 },
    { "LMGetSysVolCPtr", lmt_270 },
    { "LMGetSystemInfo", lmt_271 },
    { "LMGetSystemMenuList", lmt_272 },
    { "LMGetT1Arbitrate", lmt_273 },
    { "LMGetTEWdBreak", lmt_274 },
    { "LMGetTRAPError", lmt_275 },
    { "LMGetTRAPTable", lmt_276 },
    { "LMGetTableSeed", lmt_277 },
    { "LMGetTagData", lmt_278 },
    { "LMGetTaskLock", lmt_279 },
    { "LMGetTempRect", lmt_280 },
    { "LMGetTheCrsr", lmt_281 },
    { "LMGetTimeVars", lmt_282 },
    { "LMGetTimeViaDB", lmt_283 },
    { "LMGetTocks", lmt_284 },
    { "LMGetToolTable", lmt_285 },
    { "LMGetTop2MenuItem", lmt_286 },
    { "LMGetTrace", lmt_287 },
    { "LMGetTrapAgain", lmt_288 },
    { "LMGetTwitcher2", lmt_289 },
    { "LMGetUnassigned", lmt_290 },
    { "LMGetUnassigned2", lmt_291 },
    { "LMGetUninited", lmt_292 },
    { "LMGetUnivInfoPtr", lmt_293 },
    { "LMGetUnivROMFlags", lmt_294 },
    { "LMGetUsedFWidths", lmt_295 },
    { "LMGetVIA2DT", lmt_296 },
    { "LMGetVMGlobals", lmt_297 },
    { "LMGetVMVectors", lmt_298 },
    { "LMGetVectorPtr", lmt_299 },
    { "LMGetVectors", lmt_300 },
    { "LMGetVertRRate", lmt_301 },
    { "LMGetVia2Rbv", lmt_302 },
    { "LMGetVidMode", lmt_303 },
    { "LMGetVidType", lmt_304 },
    { "LMGetVideoInfoOK", lmt_305 },
    { "LMGetWMgrUpdate", lmt_306 },
    { "LMGetWarhol", lmt_307 },
    { "LMGetWarmStart", lmt_308 },
    { "LMGetXRgSvArea1", lmt_309 },
    { "LMGetXRgSvArea2", lmt_310 },
    { "LMGetXRgSvArea3", lmt_311 },
    { "LMGetXRgSvArea5", lmt_312 },
    { "LMGetXRgSvArea6", lmt_313 },
    { "LMGetmbSaveLoc", lmt_314 },
    { "LMGetsInfoPtr", lmt_315 },
    { "LMGetsRsrcTblPtr", lmt_316 },
    { "LMGetsdmBusErr", lmt_317 },
    { "LMSetADBBase", lmt_318 },
    { "LMSetAGBHandle", lmt_319 },
    { "LMSetASCBase", lmt_320 },
    { "LMSetATalkHk1", lmt_321 },
    { "LMSetAddrMapFlags", lmt_322 },
    { "LMSetAddressError", lmt_323 },
    { "LMSetAlarmState", lmt_324 },
    { "LMSetApp2Packs", lmt_325 },
    { "LMSetAt2MenuBottom", lmt_326 },
    { "LMSetAutoInt1", lmt_327 },
    { "LMSetAutoInt2", lmt_328 },
    { "LMSetAutoInt3", lmt_329 },
    { "LMSetAutoInt4", lmt_330 },
    { "LMSetAutoInt5", lmt_331 },
    { "LMSetAutoInt6", lmt_332 },
    { "LMSetAutoInt7", lmt_333 },
    { "LMSetBackgrounderVars", lmt_334 },
    { "LMSetBootGlobPtr", lmt_335 },
    { "LMSetBootMask", lmt_336 },
    { "LMSetBootTmp8", lmt_337 },
    { "LMSetBoxFlag", lmt_338 },
    { "LMSetBrianBits", lmt_339 },
    { "LMSetBtDskRfn", lmt_340 },
    { "LMSetBusError", lmt_341 },
    { "LMSetCHKError", lmt_342 },
    { "LMSetCMVector", lmt_343 },
    { "LMSetCQDGlobals", lmt_344 },
    { "LMSetCarlByte", lmt_345 },
    { "LMSetChooserBits", lmt_346 },
    { "LMSetChunkyDepth", lmt_347 },
    { "LMSetCkdDB", lmt_348 },
    { "LMSetCloseOrnHook", lmt_349 },
    { "LMSetCoProcessor", lmt_350 },
    { "LMSetColLines", lmt_351 },
    { "LMSetCoreEditVars", lmt_352 },
    { "LMSetCrsrAddr", lmt_353 },
    { "LMSetCrsrBase", lmt_354 },
    { "LMSetCrsrCouple", lmt_355 },
    { "LMSetCrsrDevice", lmt_356 },
    { "LMSetCrsrNew", lmt_357 },
    { "LMSetCrsrObscure", lmt_358 },
    { "LMSetCrsrPtr", lmt_359 },
    { "LMSetCrsrRect", lmt_360 },
    { "LMSetCrsrRow", lmt_361 },
    { "LMSetCrsrScale", lmt_362 },
    { "LMSetCurDeKind", lmt_363 },
    { "LMSetCurDragAction", lmt_364 },
    { "LMSetCurFMInput", lmt_365 },
    { "LMSetCurLayer", lmt_366 },
    { "LMSetDSCtrAdj", lmt_367 },
    { "LMSetDSDrawProc", lmt_368 },
    { "LMSetDSPGlobals", lmt_369 },
    { "LMSetDSWndUpdate", lmt_370 },
    { "LMSetDeskPatEnable", lmt_371 },
    { "LMSetDeskPort", lmt_372 },
    { "LMSetDiskVars", lmt_373 },
    { "LMSetDivideByZero", lmt_374 },
    { "LMSetDockingGlobals", lmt_375 },
    { "LMSetDrMstrBlk", lmt_376 },
    { "LMSetDragFlag", lmt_377 },
    { "LMSetDrawCrsrVector", lmt_378 },
    { "LMSetDskErr", lmt_379 },
    { "LMSetDskRtnAdr", lmt_380 },
    { "LMSetDskSwtchHook", lmt_381 },
    { "LMSetDskVerify", lmt_382 },
    { "LMSetEgretGlobals", lmt_383 },
    { "LMSetEndSRTPtr", lmt_384 },
    { "LMSetErCode", lmt_385 },
    { "LMSetEraseCrsrVector", lmt_386 },
    { "LMSetEvtBufCnt", lmt_387 },
    { "LMSetExpandMem", lmt_388 },
    { "LMSetExtFSHook", lmt_389 },
    { "LMSetExtraPrint", lmt_390 },
    { "LMSetFDevDisable", lmt_391 },
    { "LMSetFLckUnlck", lmt_392 },
    { "LMSetFMDefaultSize", lmt_393 },
    { "LMSetFMDotsPerInch", lmt_394 },
    { "LMSetFMExist", lmt_395 },
    { "LMSetFMStyleTab", lmt_396 },
    { "LMSetFMSynth", lmt_397 },
    { "LMSetFMgrOutRec", lmt_398 },
    { "LMSetFONDID", lmt_399 },
    { "LMSetFP040Vects", lmt_400 },
    { "LMSetFP68881", lmt_401 },
    { "LMSetFPState", lmt_402 },
    { "LMSetFSMVars", lmt_403 },
    { "LMSetFSQueueHook", lmt_404 },
    { "LMSetFSTemp4", lmt_405 },
    { "LMSetFSTemp8", lmt_406 },
    { "LMSetFScaleHFact", lmt_407 },
    { "LMSetFScaleVFact", lmt_408 },
    { "LMSetFileShareVars", lmt_409 },
    { "LMSetFinder", lmt_410 },
    { "LMSetFlushOnly", lmt_411 },
    { "LMSetFondState", lmt_412 },
    { "LMSetFontFlag", lmt_413 },
    { "LMSetFormatError", lmt_414 },
    { "LMSetFourDHack", lmt_415 },
    { "LMSetFrcSync", lmt_416 },
    { "LMSetGZRootPtr", lmt_417 },
    { "LMSetHFSFlags", lmt_418 },
    { "LMSetHFSStkTop", lmt_419 },
    { "LMSetHSCHndl", lmt_420 },
    { "LMSetHiHeapMark", lmt_421 },
    { "LMSetHpChk", lmt_422 },
    { "LMSetIOPMgrVars", lmt_423 },
    { "LMSetIconBitmap", lmt_424 },
    { "LMSetIconTLAddr", lmt_425 },
    { "LMSetIllegalInstruction", lmt_426 },
    { "LMSetIntFlag", lmt_427 },
    { "LMSetJAllocCrsr", lmt_428 },
    { "LMSetJCheckLoad", lmt_429 },
    { "LMSetJDCDReset", lmt_430 },
    { "LMSetJDiskSel", lmt_431 },
    { "LMSetJDtrmV2", lmt_432 },
    { "LMSetJDtrmV3", lmt_433 },
    { "LMSetJFileOpen", lmt_434 },
    { "LMSetJFindDrive", lmt_435 },
    { "LMSetJFlushCache", lmt_436 },
    { "LMSetJKybdTask", lmt_437 },
    { "LMSetJOpcodeProc", lmt_438 },
    { "LMSetJSendCmd", lmt_439 },
    { "LMSetJSetCCrsr", lmt_440 },
    { "LMSetJShell", lmt_441 },
    { "LMSetJSwapMMU", lmt_442 },
    { "LMSetKbdVars", lmt_443 },
    { "LMSetKeyLast", lmt_444 },
    { "LMSetKeyMVars", lmt_445 },
    { "LMSetKeyRepTime", lmt_446 },
    { "LMSetLastBG", lmt_447 },
    { "LMSetLastBinPat", lmt_448 },
    { "LMSetLastDepth", lmt_449 },
    { "LMSetLastFore", lmt_450 },
    { "LMSetLastMode", lmt_451 },
    { "LMSetLastTxGDevice", lmt_452 },
    { "LMSetLaunchFlag", lmt_453 },
    { "LMSetLayerPalette", lmt_454 },
    { "LMSetLine1010", lmt_455 },
    { "LMSetLine1111", lmt_456 },
    { "LMSetLoadFiller", lmt_457 },
    { "LMSetLoadTrap", lmt_458 },
    { "LMSetLoaderPBlock", lmt_459 },
    { "LMSetLockMemCt", lmt_460 },
    { "LMSetMAErrProc", lmt_461 },
    { "LMSetMASuperTab", lmt_462 },
    { "LMSetMBFlash", lmt_463 },
    { "LMSetMBProcHndl", lmt_464 },
    { "LMSetMMDefFlags", lmt_465 },
    { "LMSetMMFlags", lmt_466 },
    { "LMSetMMU24Tbl", lmt_467 },
    { "LMSetMMUFlags", lmt_468 },
    { "LMSetMMUTblSize", lmt_469 },
    { "LMSetMacJmp", lmt_470 },
    { "LMSetMacJmpFlag", lmt_471 },
    { "LMSetMacZero", lmt_472 },
    { "LMSetMacsBugPC", lmt_473 },
    { "LMSetMacsBugSP", lmt_474 },
    { "LMSetMacsBugSR", lmt_475 },
    { "LMSetMaxDB", lmt_476 },
    { "LMSetMem2Vectors24", lmt_477 },
    { "LMSetMem2Vectors32", lmt_478 },
    { "LMSetMemVectors24", lmt_479 },
    { "LMSetMemVectors32", lmt_480 },
    { "LMSetMenuLayer", lmt_481 },
    { "LMSetMickeyBytes", lmt_482 },
    { "LMSetMimeGlobs", lmt_483 },
    { "LMSetMmInOK", lmt_484 },
    { "LMSetMonkeyLives", lmt_485 },
    { "LMSetMouse", lmt_486 },
    { "LMSetMouseMask", lmt_487 },
    { "LMSetMouseOffset", lmt_488 },
    { "LMSetNMIFlag", lmt_489 },
    { "LMSetNMQHdr", lmt_490 },
    { "LMSetNTSC", lmt_491 },
    { "LMSetNewMount", lmt_492 },
    { "LMSetNewUnused", lmt_493 },
    { "LMSetNxtDB", lmt_494 },
    { "LMSetOSTable", lmt_495 },
    { "LMSetOSTableSplit", lmt_496 },
    { "LMSetOSTableSplit2", lmt_497 },
    { "LMSetOSTableSplit3", lmt_498 },
    { "LMSetOSTableSplit4", lmt_499 },
    { "LMSetOSTableSplit5", lmt_500 },
    { "LMSetOldTimeSCSIDB", lmt_501 },
    { "LMSetOmegaSANE", lmt_502 },
    { "LMSetPMMU", lmt_503 },
    { "LMSetPMSPPtr", lmt_504 },
    { "LMSetPSCIntTbl", lmt_505 },
    { "LMSetPWMBuf1", lmt_506 },
    { "LMSetPWMBuf2", lmt_507 },
    { "LMSetPWMValue", lmt_508 },
    { "LMSetPadRsrc", lmt_509 },
    { "LMSetPadRsrc2", lmt_510 },
    { "LMSetParams", lmt_511 },
    { "LMSetPhys2Log", lmt_512 },
    { "LMSetPhysMemTop", lmt_513 },
    { "LMSetPmgrHandle", lmt_514 },
    { "LMSetPollProc", lmt_515 },
    { "LMSetPollRtnAddr", lmt_516 },
    { "LMSetPollStack", lmt_517 },
    { "LMSetPortAUse", lmt_518 },
    { "LMSetPowerMgrVars", lmt_519 },
    { "LMSetPrivilegeViolation", lmt_520 },
    { "LMSetQDErr", lmt_521 },
    { "LMSetRGBBlack", lmt_522 },
    { "LMSetRGBWhite", lmt_523 },
    { "LMSetRMGRHiVars", lmt_524 },
    { "LMSetRMGRPerm", lmt_525 },
    { "LMSetROMMapHndl", lmt_526 },
    { "LMSetRSDHndl", lmt_527 },
    { "LMSetRawMouse", lmt_528 },
    { "LMSetRealMemTop", lmt_529 },
    { "LMSetRegRsrc", lmt_530 },
    { "LMSetReqstVol", lmt_531 },
    { "LMSetResOneDeep", lmt_532 },
    { "LMSetResReadOnly", lmt_533 },
    { "LMSetReserved", lmt_534 },
    { "LMSetReserved2", lmt_535 },
    { "LMSetReset", lmt_536 },
    { "LMSetRowBits", lmt_537 },
    { "LMSetSCCASts", lmt_538 },
    { "LMSetSCCBSts", lmt_539 },
    { "LMSetSCCIOPFlag", lmt_540 },
    { "LMSetSCSI2Base", lmt_541 },
    { "LMSetSCSIBase", lmt_542 },
    { "LMSetSCSIDMA", lmt_543 },
    { "LMSetSCSIDrvrs", lmt_544 },
    { "LMSetSCSIGlobals", lmt_545 },
    { "LMSetSCSIHsk", lmt_546 },
    { "LMSetSCSIPoll", lmt_547 },
    { "LMSetSDMJmpTblPtr", lmt_548 },
    { "LMSetSEVarBase", lmt_549 },
    { "LMSetSEVarBase0", lmt_550 },
    { "LMSetSEVarBase1", lmt_551 },
    { "LMSetSEVarBase2", lmt_552 },
    { "LMSetSEVarBase3", lmt_553 },
    { "LMSetSEVarBase4", lmt_554 },
    { "LMSetSEVarBase5", lmt_555 },
    { "LMSetSInitFlags", lmt_556 },
    { "LMSetSMGlobals", lmt_557 },
    { "LMSetSPMisc1", lmt_558 },
    { "LMSetSaneTrapAddr", lmt_559 },
    { "LMSetSaveFondFlags", lmt_560 },
    { "LMSetSaveSP", lmt_561 },
    { "LMSetSaveSegHandle", lmt_562 },
    { "LMSetSavedHandle", lmt_563 },
    { "LMSetSavedHilite", lmt_564 },
    { "LMSetScrDmpType", lmt_565 },
    { "LMSetScrapTag", lmt_566 },
    { "LMSetScreenBytes", lmt_567 },
    { "LMSetScrnVBLPtr", lmt_568 },
    { "LMSetSegHiEnable", lmt_569 },
    { "LMSetSerialVars", lmt_570 },
    { "LMSetSetOSDefKey", lmt_571 },
    { "LMSetShieldDepth", lmt_572 },
    { "LMSetShieldRect", lmt_573 },
    { "LMSetShutDownQHdr", lmt_574 },
    { "LMSetSlotPrTbl", lmt_575 },
    { "LMSetSlotQDT", lmt_576 },
    { "LMSetSlotTicks", lmt_577 },
    { "LMSetSlotVBLQ", lmt_578 },
    { "LMSetSonyVars", lmt_579 },
    { "LMSetSoundDCE", lmt_580 },
    { "LMSetSoundGlue", lmt_581 },
    { "LMSetSoundVBL", lmt_582 },
    { "LMSetSpuriousInterrupt", lmt_583 },
    { "LMSetSrcDevice", lmt_584 },
    { "LMSetStkLowPt", lmt_585 },
    { "LMSetSwitcher", lmt_586 },
    { "LMSetSysEvtBuf", lmt_587 },
    { "LMSetSysVolCPtr", lmt_588 },
    { "LMSetSystemInfo", lmt_589 },
    { "LMSetSystemMenuList", lmt_590 },
    { "LMSetT1Arbitrate", lmt_591 },
    { "LMSetTEWdBreak", lmt_592 },
    { "LMSetTRAPError", lmt_593 },
    { "LMSetTRAPTable", lmt_594 },
    { "LMSetTableSeed", lmt_595 },
    { "LMSetTagData", lmt_596 },
    { "LMSetTaskLock", lmt_597 },
    { "LMSetTempRect", lmt_598 },
    { "LMSetTheCrsr", lmt_599 },
    { "LMSetTimeVars", lmt_600 },
    { "LMSetTimeViaDB", lmt_601 },
    { "LMSetTocks", lmt_602 },
    { "LMSetToolTable", lmt_603 },
    { "LMSetTop2MenuItem", lmt_604 },
    { "LMSetTrace", lmt_605 },
    { "LMSetTrapAgain", lmt_606 },
    { "LMSetTwitcher2", lmt_607 },
    { "LMSetUnassigned", lmt_608 },
    { "LMSetUnassigned2", lmt_609 },
    { "LMSetUninited", lmt_610 },
    { "LMSetUnivInfoPtr", lmt_611 },
    { "LMSetUnivROMFlags", lmt_612 },
    { "LMSetUsedFWidths", lmt_613 },
    { "LMSetVIA2DT", lmt_614 },
    { "LMSetVMGlobals", lmt_615 },
    { "LMSetVMVectors", lmt_616 },
    { "LMSetVectorPtr", lmt_617 },
    { "LMSetVectors", lmt_618 },
    { "LMSetVertRRate", lmt_619 },
    { "LMSetVia2Rbv", lmt_620 },
    { "LMSetVidMode", lmt_621 },
    { "LMSetVidType", lmt_622 },
    { "LMSetVideoInfoOK", lmt_623 },
    { "LMSetWMgrUpdate", lmt_624 },
    { "LMSetWarhol", lmt_625 },
    { "LMSetWarmStart", lmt_626 },
    { "LMSetXRgSvArea1", lmt_627 },
    { "LMSetXRgSvArea2", lmt_628 },
    { "LMSetXRgSvArea3", lmt_629 },
    { "LMSetXRgSvArea5", lmt_630 },
    { "LMSetXRgSvArea6", lmt_631 },
    { "LMSetmbSaveLoc", lmt_632 },
    { "LMSetsInfoPtr", lmt_633 },
    { "LMSetsRsrcTblPtr", lmt_634 },
    { "LMSetsdmBusErr", lmt_635 },
};
const unsigned lmacc_export_count = sizeof lmacc_exports / sizeof lmacc_exports[0];
