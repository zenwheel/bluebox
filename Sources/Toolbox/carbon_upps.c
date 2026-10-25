/*
 * CarbonLib's universal procedure pointer calls for the rest of the
 * interfaces (Apple Events and object model, QuickTime, Display Manager,
 * Open Transport, speech, sound, ColorSync, Memory Manager hooks and so
 * on): NewXxxUPP makes a routine descriptor for the program's PowerPC
 * routine with the procInfo of that UPP type (as CarbonLib did on Mac OS 8
 * and 9, so 68k code can call it too), DisposeXxxUPP disposes of it and
 * InvokeXxxUPP calls it through Mixed Mode, the routine's arguments first
 * and the UPP last.  The procInfos are those of the interfaces' uppXxx
 * enums, one line per type below.
 */
#include <stdint.h>

#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

enum { kPowerPCISA = 1, kRegisterBased = 2 };

static void new_upp(ppc_cpu *c, uint32_t procinfo) { RETURN(mm_new_descriptor(R(0), procinfo, kPowerPCISA)); }

/* NewComponentFunctionUPP(proc, procInfo): the caller says what the routine takes. */
static void new_component_function(ppc_cpu *c) { RETURN(mm_new_descriptor(R(0), R(1), kPowerPCISA)); }

static void dispose_upp(ppc_cpu *c)
{
    if (mm_is_descriptor(R(0)))
        tb_DisposePtr(R(0));
    c->pc = c->lr;
}

/* The number of arguments procInfo describes. */
static int upp_args(uint32_t pi)
{
    int n = 0;
    if ((pi & 0xF) == kRegisterBased) {
        while (n < 4 && ((pi >> (11 + 5 * n)) & 3))
            n++;
    } else {
        while (n < 13 && ((pi >> (6 + 2 * n)) & 3))
            n++;
    }
    return n;
}

/* InvokeXxxUPP(arguments..., upp): from the ninth on, the arguments are on the stack. */
static void invoke_upp(ppc_cpu *c, uint32_t pi)
{
    int n = upp_args(pi);
    uint32_t args[14];
    for (int i = 0; i <= n; i++)
        args[i] = i < 8 ? c->gpr[3 + i] : rd32(c->gpr[1] + 24 + 4 * (uint32_t)i);
    gaddr upp = args[n];
    uint32_t r = upp ? mm_call(upp, pi, args, n) : 0;
    int size = (int)((pi >> 4) & 3);
    if (size == 1)
        r = (uint32_t)(int32_t)(int8_t)r;
    else if (size == 2)
        r = (uint32_t)(int32_t)(int16_t)r;
    RETURN(r);
}

#define NEW(name, pi) static void new_##name(ppc_cpu *c) { new_upp(c, pi); }
#define INVOKE(name, pi) static void invoke_##name(ppc_cpu *c) { invoke_upp(c, pi); }

INVOKE(AECoerceDesc, 0x00003FE0)
NEW(AECoerceDesc, 0x00003FE0)
INVOKE(AECoercePtr, 0x0003FFE0)
NEW(AECoercePtr, 0x0003FFE0)
INVOKE(AEEventHandler, 0x00000FE0)
NEW(AEEventHandler, 0x00000FE0)
INVOKE(AVLCompareItems, 0x00002FF0)
NEW(AVLCompareItems, 0x00002FF0)
INVOKE(AVLDisposeItem, 0x000003C0)
NEW(AVLDisposeItem, 0x000003C0)
INVOKE(AVLItemSize, 0x000003F0)
NEW(AVLItemSize, 0x000003F0)
INVOKE(AVLWalk, 0x000FEBE0)
NEW(AVLWalk, 0x000FEBE0)
INVOKE(Actions, 0x00003FE0)
NEW(Actions, 0x00003FE0)
INVOKE(AliasFilter, 0x00000FD0)
NEW(AliasFilter, 0x00000FD0)
INVOKE(CMBitmapCallBack, 0x000003D0)
NEW(CMBitmapCallBack, 0x000003D0)
INVOKE(CMConcatCallBack, 0x000003D0)
NEW(CMConcatCallBack, 0x000003D0)
INVOKE(CMFlatten, 0x00003FE0)
NEW(CMFlatten, 0x00003FE0)
INVOKE(CMMIterate, 0x000003E0)
NEW(CMMIterate, 0x000003E0)
INVOKE(CMProfileAccess, 0x0000FFE0)
NEW(CMProfileAccess, 0x0000FFE0)
INVOKE(CMProfileFilter, 0x000003D0)
NEW(CMProfileFilter, 0x000003D0)
INVOKE(CMProfileIterate, 0x000003E0)
NEW(CMProfileIterate, 0x000003E0)
INVOKE(CalcColorTable, 0x00003EE0)
NEW(CalcColorTable, 0x00003EE0)
INVOKE(Calibrate, 0x000000E0)
NEW(Calibrate, 0x000000E0)
INVOKE(CalibrateEvent, 0x000000C0)
NEW(CalibrateEvent, 0x000000C0)
INVOKE(CanCalibrate, 0x000003D0)
NEW(CanCalibrate, 0x000003D0)
INVOKE(CharDataHandler, 0x000003F0)
NEW(CharDataHandler, 0x000003F0)
INVOKE(CollectionException, 0x000002E0)
NEW(CollectionException, 0x000002E0)
INVOKE(CollectionFlatten, 0x00000FE0)
NEW(CollectionFlatten, 0x00000FE0)
INVOKE(ColorChanged, 0x000003C0)
NEW(ColorChanged, 0x000003C0)
INVOKE(CommentHandler, 0x000003F0)
NEW(CommentHandler, 0x000003F0)
INVOKE(ComponentMPWorkFunction, 0x000003F0)
NEW(ComponentMPWorkFunction, 0x000003F0)
INVOKE(ComponentRoutine, 0x000003F0)
NEW(ComponentRoutine, 0x000003F0)
INVOKE(DMComponentListIterator, 0x00000FC0)
NEW(DMComponentListIterator, 0x00000FC0)
INVOKE(DMDisplayListIterator, 0x00000FC0)
NEW(DMDisplayListIterator, 0x00000FC0)
INVOKE(DMDisplayModeListIterator, 0x00000FC0)
NEW(DMDisplayModeListIterator, 0x00000FC0)
INVOKE(DMExtendedNotification, 0x00000EC0)
NEW(DMExtendedNotification, 0x00000EC0)
INVOKE(DMNotification, 0x000000C0)
NEW(DMNotification, 0x000000C0)
INVOKE(DMProfileListIterator, 0x00000FC0)
NEW(DMProfileListIterator, 0x00000FC0)
INVOKE(DataHCompletion, 0x00000BC0)
NEW(DataHCompletion, 0x00000BC0)
INVOKE(DebugAssertOutputHandler, 0x00FFFFC0)
NEW(DebugAssertOutputHandler, 0x00FFFFC0)
INVOKE(DebugComponentCallback, 0x00000FC0)
NEW(DebugComponentCallback, 0x00000FC0)
INVOKE(DeferredTask, 0x0000B802)
NEW(DeferredTask, 0x0000B802)
INVOKE(DisposeColorPickMethod, 0x000000E0)
NEW(DisposeColorPickMethod, 0x000000E0)
INVOKE(DoMCAction, 0x00003EE0)
NEW(DoMCAction, 0x00003EE0)
INVOKE(EndDocumentHandler, 0x000000F0)
NEW(EndDocumentHandler, 0x000000F0)
INVOKE(EndElementHandler, 0x000003F0)
NEW(EndElementHandler, 0x000003F0)
INVOKE(ExceptionHandler, 0x000000F1)
NEW(ExceptionHandler, 0x000000F1)
INVOKE(FBCCallback, 0x00000F91)
NEW(FBCCallback, 0x00000F91)
INVOKE(FMFontCallbackFilter, 0x000003F0)
NEW(FMFontCallbackFilter, 0x000003F0)
INVOKE(FMFontFamilyCallbackFilter, 0x000003B0)
NEW(FMFontFamilyCallbackFilter, 0x000003B0)
INVOKE(FolderManagerNotification, 0x00000FF0)
NEW(FolderManagerNotification, 0x00000FF0)
INVOKE(GetMissingComponentResource, 0x0000FBE0)
NEW(GetMissingComponentResource, 0x0000FBE0)
INVOKE(GetMovie, 0x00003FE0)
NEW(GetMovie, 0x00003FE0)
INVOKE(GetScrapData, 0x00000FE0)
NEW(GetScrapData, 0x00000FE0)
INVOKE(GrowZone, 0x000000F0)
NEW(GrowZone, 0x000000F0)
INVOKE(HDSpindown, 0x000000C0)
NEW(HDSpindown, 0x000000C0)
INVOKE(HRNewCFURL, 0x000037F0)
NEW(HRNewCFURL, 0x000037F0)
INVOKE(HRNewURL, 0x000037F0)
NEW(HRNewURL, 0x000037F0)
INVOKE(HRURLToFSRef, 0x0000EFF0)
NEW(HRURLToFSRef, 0x0000EFF0)
INVOKE(HRURLToFSSpec, 0x0000EFF0)
NEW(HRURLToFSSpec, 0x0000EFF0)
INVOKE(HRWasCFURLVisited, 0x000003D0)
NEW(HRWasCFURLVisited, 0x000003D0)
INVOKE(HRWasURLVisited, 0x000003D0)
NEW(HRWasURLVisited, 0x000003D0)
INVOKE(ICMAlignment, 0x000003C0)
NEW(ICMAlignment, 0x000003C0)
INVOKE(ICMCompletion, 0x00000E80)
NEW(ICMCompletion, 0x00000E80)
INVOKE(ICMConvertDataFormat, 0x003FFFE0)
NEW(ICMConvertDataFormat, 0x003FFFE0)
INVOKE(ICMCursorShielded, 0x00000FC0)
NEW(ICMCursorShielded, 0x00000FC0)
INVOKE(ICMData, 0x00000FE0)
NEW(ICMData, 0x00000FE0)
INVOKE(ICMFlush, 0x00000FE0)
NEW(ICMFlush, 0x00000FE0)
INVOKE(ICMMemoryDisposed, 0x000003C0)
NEW(ICMMemoryDisposed, 0x000003C0)
INVOKE(ICMProgress, 0x00000FA0)
NEW(ICMProgress, 0x00000FA0)
INVOKE(IOCompletion, 0x00009802)
NEW(IOCompletion, 0x00009802)
INVOKE(IconAction, 0x00000FE0)
NEW(IconAction, 0x00000FE0)
INVOKE(IconGetter, 0x000003F0)
NEW(IconGetter, 0x000003F0)
INVOKE(ImageCodecDrawBandComplete, 0x00000FC0)
NEW(ImageCodecDrawBandComplete, 0x00000FC0)
INVOKE(ImageCodecMPDrawBand, 0x000003F0)
NEW(ImageCodecMPDrawBand, 0x000003F0)
INVOKE(ImageCodecTimeTrigger, 0x000000C0)
NEW(ImageCodecTimeTrigger, 0x000000C0)
INVOKE(IndexToString, 0x00003F90)
NEW(IndexToString, 0x00003F90)
INVOKE(InitPickMethod, 0x00000FA0)
NEW(InitPickMethod, 0x00000FA0)
INVOKE(KCCallback, 0x00000FB0)
NEW(KCCallback, 0x00000FB0)
INVOKE(MCActionFilter, 0x00000FD0)
NEW(MCActionFilter, 0x00000FD0)
INVOKE(MCActionFilterWithRefCon, 0x00003ED0)
NEW(MCActionFilterWithRefCon, 0x00003ED0)
INVOKE(MovieDrawingComplete, 0x000003E0)
NEW(MovieDrawingComplete, 0x000003E0)
INVOKE(MovieExecuteWiredActions, 0x00003FE0)
NEW(MovieExecuteWiredActions, 0x00003FE0)
INVOKE(MovieExportGetData, 0x000003E0)
NEW(MovieExportGetData, 0x000003E0)
INVOKE(MovieExportGetProperty, 0x00003FE0)
NEW(MovieExportGetProperty, 0x00003FE0)
INVOKE(MoviePrePrerollComplete, 0x00000EC0)
NEW(MoviePrePrerollComplete, 0x00000EC0)
INVOKE(MoviePreviewCallOut, 0x000000D0)
NEW(MoviePreviewCallOut, 0x000000D0)
INVOKE(MovieProgress, 0x0000FAE0)
NEW(MovieProgress, 0x0000FAE0)
INVOKE(MovieRgnCover, 0x00000FE0)
NEW(MovieRgnCover, 0x00000FE0)
INVOKE(MoviesError, 0x00000380)
NEW(MoviesError, 0x00000380)
INVOKE(MusicMIDISend, 0x00000FF0)
NEW(MusicMIDISend, 0x00000FF0)
INVOKE(MusicOfflineData, 0x00000FF0)
NEW(MusicOfflineData, 0x00000FF0)
INVOKE(NColorChanged, 0x000003C0)
NEW(NColorChanged, 0x000003C0)
INVOKE(NSLClientNotify, 0x000000C0)
NEW(NSLClientNotify, 0x000000C0)
INVOKE(NSLEvent, 0x000003C0)
NEW(NSLEvent, 0x000003C0)
INVOKE(NSLMgrNotify, 0x000000C0)
NEW(NSLMgrNotify, 0x000000C0)
INVOKE(NSLURLFilter, 0x000003D0)
NEW(NSLURLFilter, 0x000003D0)
INVOKE(OSAActive, 0x000000E0)
NEW(OSAActive, 0x000000E0)
INVOKE(OSACreateAppleEvent, 0x000FEFE0)
NEW(OSACreateAppleEvent, 0x000FEFE0)
INVOKE(OSASend, 0x003FEFE0)
NEW(OSASend, 0x003FEFE0)
INVOKE(OSLAccessor, 0x000FFFE0)
NEW(OSLAccessor, 0x000FFFE0)
INVOKE(OSLAdjustMarks, 0x00000FE0)
NEW(OSLAdjustMarks, 0x00000FE0)
INVOKE(OSLCompare, 0x00003FE0)
NEW(OSLCompare, 0x00003FE0)
INVOKE(OSLCount, 0x00003FE0)
NEW(OSLCount, 0x00003FE0)
INVOKE(OSLDisposeToken, 0x000000E0)
NEW(OSLDisposeToken, 0x000000E0)
INVOKE(OSLGetErrDesc, 0x000000E0)
NEW(OSLGetErrDesc, 0x000000E0)
INVOKE(OSLGetMarkToken, 0x00000FE0)
NEW(OSLGetMarkToken, 0x00000FE0)
INVOKE(OSLMark, 0x00000FE0)
NEW(OSLMark, 0x00000FE0)
INVOKE(OTListSearch, 0x000003D1)
NEW(OTListSearch, 0x000003D1)
INVOKE(OTNotify, 0x00003FC0)
NEW(OTNotify, 0x00003FC0)
INVOKE(OTProcess, 0x000000C0)
NEW(OTProcess, 0x000000C0)
INVOKE(PMgrStateChange, 0x000003C0)
NEW(PMgrStateChange, 0x000003C0)
INVOKE(PreprocessInstructionHandler, 0x00000FF0)
NEW(PreprocessInstructionHandler, 0x00000FF0)
INVOKE(Purge, 0x000000C0)
NEW(Purge, 0x000000C0)
INVOKE(QDPix, 0x002FEFC0)
NEW(QDPix, 0x002FEFC0)
INVOKE(QTBandwidthNotification, 0x00000FE0)
NEW(QTBandwidthNotification, 0x00000FE0)
INVOKE(QTCallBack, 0x000003C0)
NEW(QTCallBack, 0x000003C0)
INVOKE(QTSModalFilter, 0x00003FD0)
NEW(QTSModalFilter, 0x00003FD0)
INVOKE(QTSNotification, 0x00003FF0)
NEW(QTSNotification, 0x00003FF0)
INVOKE(QTSyncTask, 0x000000C0)
NEW(QTSyncTask, 0x000000C0)
INVOKE(QTVRBackBufferImaging, 0x0003FBE0)
NEW(QTVRBackBufferImaging, 0x0003FBE0)
INVOKE(QTVREnteringNode, 0x00000FE0)
NEW(QTVREnteringNode, 0x00000FE0)
INVOKE(QTVRImagingComplete, 0x000003E0)
NEW(QTVRImagingComplete, 0x000003E0)
INVOKE(QTVRIntercept, 0x00003FC0)
NEW(QTVRIntercept, 0x00003FC0)
INVOKE(QTVRLeavingNode, 0x0000FFE0)
NEW(QTVRLeavingNode, 0x0000FFE0)
INVOKE(QTVRMouseOverHotSpot, 0x00003FE0)
NEW(QTVRMouseOverHotSpot, 0x00003FE0)
INVOKE(RTPMPDataRelease, 0x000003C0)
NEW(RTPMPDataRelease, 0x000003C0)
INVOKE(RTPPBCallback, 0x00000FC0)
NEW(RTPPBCallback, 0x00000FC0)
INVOKE(RecordColors, 0x00003FE0)
NEW(RecordColors, 0x00003FE0)
INVOKE(ResErr, 0x00001002)
NEW(ResErr, 0x00001002)
INVOKE(SCModalFilter, 0x00003FD0)
NEW(SCModalFilter, 0x00003FD0)
INVOKE(SCModalHook, 0x00003EE0)
NEW(SCModalHook, 0x00003EE0)
INVOKE(SCSICallback, 0x000000C0)
NEW(SCSICallback, 0x000000C0)
INVOKE(SGAddFrameBottle, 0x0003FEF0)
NEW(SGAddFrameBottle, 0x0003FEF0)
INVOKE(SGCompressBottle, 0x00000EF0)
NEW(SGCompressBottle, 0x00000EF0)
INVOKE(SGCompressCompleteBottle, 0x0000FEF0)
NEW(SGCompressCompleteBottle, 0x0000FEF0)
INVOKE(SGData, 0x003BFFE0)
NEW(SGData, 0x003BFFE0)
INVOKE(SGDisplayBottle, 0x0000FEF0)
NEW(SGDisplayBottle, 0x0000FEF0)
INVOKE(SGDisplayCompressBottle, 0x0003FFF0)
NEW(SGDisplayCompressBottle, 0x0003FFF0)
INVOKE(SGGrabBottle, 0x00000EF0)
NEW(SGGrabBottle, 0x00000EF0)
INVOKE(SGGrabCompleteBottle, 0x00003EF0)
NEW(SGGrabCompleteBottle, 0x00003EF0)
INVOKE(SGGrabCompressCompleteBottle, 0x0000FFF0)
NEW(SGGrabCompressCompleteBottle, 0x0000FFF0)
INVOKE(SGModalFilter, 0x00003FD0)
NEW(SGModalFilter, 0x00003FD0)
INVOKE(SGTransferFrameBottle, 0x0000FEF0)
NEW(SGTransferFrameBottle, 0x0000FEF0)
INVOKE(SICompletion, 0x000000C0)
NEW(SICompletion, 0x000000C0)
INVOKE(SIInterrupt, 0x1C579802)
NEW(SIInterrupt, 0x1C579802)
INVOKE(SRCallBack, 0x000000C0)
NEW(SRCallBack, 0x000000C0)
INVOKE(SelectorFunction, 0x000003E0)
NEW(SelectorFunction, 0x000003E0)
INVOKE(SleepQ, 0x00131832)
NEW(SleepQ, 0x00131832)
INVOKE(SndCallBack, 0x000003C0)
NEW(SndCallBack, 0x000003C0)
INVOKE(SoundConverterFillBufferData, 0x000003D0)
NEW(SoundConverterFillBufferData, 0x000003D0)
INVOKE(SoundParam, 0x000000D0)
NEW(SoundParam, 0x000000D0)
INVOKE(SpeechDone, 0x000003C0)
NEW(SpeechDone, 0x000003C0)
INVOKE(SpeechError, 0x00003BC0)
NEW(SpeechError, 0x00003BC0)
INVOKE(SpeechPhoneme, 0x00000BC0)
NEW(SpeechPhoneme, 0x00000BC0)
INVOKE(SpeechSync, 0x00000FC0)
NEW(SpeechSync, 0x00000FC0)
INVOKE(SpeechTextDone, 0x0000FFC0)
NEW(SpeechTextDone, 0x0000FFC0)
INVOKE(SpeechWord, 0x00002FC0)
NEW(SpeechWord, 0x00002FC0)
INVOKE(StartDocumentHandler, 0x000000F0)
NEW(StartDocumentHandler, 0x000000F0)
INVOKE(StartElementHandler, 0x00000FF0)
NEW(StartElementHandler, 0x00000FF0)
INVOKE(StdPix, 0x002FEFC0)
NEW(StdPix, 0x002FEFC0)
INVOKE(TSMTEPostUpdate, 0x000FFFC0)
NEW(TSMTEPostUpdate, 0x000FFFC0)
INVOKE(TSMTEPreUpdate, 0x000003C0)
NEW(TSMTEPreUpdate, 0x000003C0)
INVOKE(TextMedia, 0x00003FE0)
NEW(TextMedia, 0x00003FE0)
INVOKE(TrackTransfer, 0x000003E0)
NEW(TrackTransfer, 0x000003E0)
INVOKE(TuneCallBack, 0x000003C0)
NEW(TuneCallBack, 0x000003C0)
INVOKE(TunePlayCallBack, 0x00000FC0)
NEW(TunePlayCallBack, 0x00000FC0)
INVOKE(TweenerData, 0x003FFFF0)
NEW(TweenerData, 0x003FFFF0)
INVOKE(URLNotify, 0x00000FF0)
NEW(URLNotify, 0x00000FF0)
INVOKE(URLSystemEvent, 0x000003F0)
NEW(URLSystemEvent, 0x000003F0)
INVOKE(UnicodeToTextFallback, 0x003FFFF0)
NEW(UnicodeToTextFallback, 0x003FFFF0)
INVOKE(UserEvent, 0x000000D0)
NEW(UserEvent, 0x000000D0)
INVOKE(UserFn, 0x00009802)
NEW(UserFn, 0x00009802)
INVOKE(VdigInt, 0x000003C0)
NEW(VdigInt, 0x000003C0)

const ppc_export carbonupps_exports[] = {
    { "DisposeAECoerceDescUPP", dispose_upp },
    { "InvokeAECoerceDescUPP", invoke_AECoerceDesc },
    { "NewAECoerceDescUPP", new_AECoerceDesc },
    { "DisposeAECoercePtrUPP", dispose_upp },
    { "InvokeAECoercePtrUPP", invoke_AECoercePtr },
    { "NewAECoercePtrUPP", new_AECoercePtr },
    { "DisposeAEEventHandlerUPP", dispose_upp },
    { "InvokeAEEventHandlerUPP", invoke_AEEventHandler },
    { "NewAEEventHandlerUPP", new_AEEventHandler },
    { "DisposeAVLCompareItemsUPP", dispose_upp },
    { "InvokeAVLCompareItemsUPP", invoke_AVLCompareItems },
    { "NewAVLCompareItemsUPP", new_AVLCompareItems },
    { "DisposeAVLDisposeItemUPP", dispose_upp },
    { "InvokeAVLDisposeItemUPP", invoke_AVLDisposeItem },
    { "NewAVLDisposeItemUPP", new_AVLDisposeItem },
    { "DisposeAVLItemSizeUPP", dispose_upp },
    { "InvokeAVLItemSizeUPP", invoke_AVLItemSize },
    { "NewAVLItemSizeUPP", new_AVLItemSize },
    { "DisposeAVLWalkUPP", dispose_upp },
    { "InvokeAVLWalkUPP", invoke_AVLWalk },
    { "NewAVLWalkUPP", new_AVLWalk },
    { "DisposeActionsUPP", dispose_upp },
    { "InvokeActionsUPP", invoke_Actions },
    { "NewActionsUPP", new_Actions },
    { "DisposeAliasFilterUPP", dispose_upp },
    { "InvokeAliasFilterUPP", invoke_AliasFilter },
    { "NewAliasFilterUPP", new_AliasFilter },
    { "DisposeCMBitmapCallBackUPP", dispose_upp },
    { "InvokeCMBitmapCallBackUPP", invoke_CMBitmapCallBack },
    { "NewCMBitmapCallBackUPP", new_CMBitmapCallBack },
    { "DisposeCMConcatCallBackUPP", dispose_upp },
    { "InvokeCMConcatCallBackUPP", invoke_CMConcatCallBack },
    { "NewCMConcatCallBackUPP", new_CMConcatCallBack },
    { "DisposeCMFlattenUPP", dispose_upp },
    { "InvokeCMFlattenUPP", invoke_CMFlatten },
    { "NewCMFlattenUPP", new_CMFlatten },
    { "DisposeCMMIterateUPP", dispose_upp },
    { "InvokeCMMIterateUPP", invoke_CMMIterate },
    { "NewCMMIterateUPP", new_CMMIterate },
    { "DisposeCMProfileAccessUPP", dispose_upp },
    { "InvokeCMProfileAccessUPP", invoke_CMProfileAccess },
    { "NewCMProfileAccessUPP", new_CMProfileAccess },
    { "DisposeCMProfileFilterUPP", dispose_upp },
    { "InvokeCMProfileFilterUPP", invoke_CMProfileFilter },
    { "NewCMProfileFilterUPP", new_CMProfileFilter },
    { "DisposeCMProfileIterateUPP", dispose_upp },
    { "InvokeCMProfileIterateUPP", invoke_CMProfileIterate },
    { "NewCMProfileIterateUPP", new_CMProfileIterate },
    { "DisposeCalcColorTableUPP", dispose_upp },
    { "InvokeCalcColorTableUPP", invoke_CalcColorTable },
    { "NewCalcColorTableUPP", new_CalcColorTable },
    { "DisposeCalibrateUPP", dispose_upp },
    { "InvokeCalibrateUPP", invoke_Calibrate },
    { "NewCalibrateUPP", new_Calibrate },
    { "DisposeCalibrateEventUPP", dispose_upp },
    { "InvokeCalibrateEventUPP", invoke_CalibrateEvent },
    { "NewCalibrateEventUPP", new_CalibrateEvent },
    { "DisposeCanCalibrateUPP", dispose_upp },
    { "InvokeCanCalibrateUPP", invoke_CanCalibrate },
    { "NewCanCalibrateUPP", new_CanCalibrate },
    { "DisposeCharDataHandlerUPP", dispose_upp },
    { "InvokeCharDataHandlerUPP", invoke_CharDataHandler },
    { "NewCharDataHandlerUPP", new_CharDataHandler },
    { "DisposeCollectionExceptionUPP", dispose_upp },
    { "InvokeCollectionExceptionUPP", invoke_CollectionException },
    { "NewCollectionExceptionUPP", new_CollectionException },
    { "DisposeCollectionFlattenUPP", dispose_upp },
    { "InvokeCollectionFlattenUPP", invoke_CollectionFlatten },
    { "NewCollectionFlattenUPP", new_CollectionFlatten },
    { "DisposeColorChangedUPP", dispose_upp },
    { "InvokeColorChangedUPP", invoke_ColorChanged },
    { "NewColorChangedUPP", new_ColorChanged },
    { "DisposeCommentHandlerUPP", dispose_upp },
    { "InvokeCommentHandlerUPP", invoke_CommentHandler },
    { "NewCommentHandlerUPP", new_CommentHandler },
    { "NewComponentFunctionUPP", new_component_function },
    { "DisposeComponentFunctionUPP", dispose_upp },
    { "DisposeComponentMPWorkFunctionUPP", dispose_upp },
    { "InvokeComponentMPWorkFunctionUPP", invoke_ComponentMPWorkFunction },
    { "NewComponentMPWorkFunctionUPP", new_ComponentMPWorkFunction },
    { "DisposeComponentRoutineUPP", dispose_upp },
    { "InvokeComponentRoutineUPP", invoke_ComponentRoutine },
    { "NewComponentRoutineUPP", new_ComponentRoutine },
    { "DisposeDMComponentListIteratorUPP", dispose_upp },
    { "InvokeDMComponentListIteratorUPP", invoke_DMComponentListIterator },
    { "NewDMComponentListIteratorUPP", new_DMComponentListIterator },
    { "DisposeDMDisplayListIteratorUPP", dispose_upp },
    { "InvokeDMDisplayListIteratorUPP", invoke_DMDisplayListIterator },
    { "NewDMDisplayListIteratorUPP", new_DMDisplayListIterator },
    { "DisposeDMDisplayModeListIteratorUPP", dispose_upp },
    { "InvokeDMDisplayModeListIteratorUPP", invoke_DMDisplayModeListIterator },
    { "NewDMDisplayModeListIteratorUPP", new_DMDisplayModeListIterator },
    { "DisposeDMExtendedNotificationUPP", dispose_upp },
    { "InvokeDMExtendedNotificationUPP", invoke_DMExtendedNotification },
    { "NewDMExtendedNotificationUPP", new_DMExtendedNotification },
    { "DisposeDMNotificationUPP", dispose_upp },
    { "InvokeDMNotificationUPP", invoke_DMNotification },
    { "NewDMNotificationUPP", new_DMNotification },
    { "DisposeDMProfileListIteratorUPP", dispose_upp },
    { "InvokeDMProfileListIteratorUPP", invoke_DMProfileListIterator },
    { "NewDMProfileListIteratorUPP", new_DMProfileListIterator },
    { "DisposeDataHCompletionUPP", dispose_upp },
    { "InvokeDataHCompletionUPP", invoke_DataHCompletion },
    { "NewDataHCompletionUPP", new_DataHCompletion },
    { "DisposeDebugAssertOutputHandlerUPP", dispose_upp },
    { "InvokeDebugAssertOutputHandlerUPP", invoke_DebugAssertOutputHandler },
    { "NewDebugAssertOutputHandlerUPP", new_DebugAssertOutputHandler },
    { "DisposeDebugComponentCallbackUPP", dispose_upp },
    { "InvokeDebugComponentCallbackUPP", invoke_DebugComponentCallback },
    { "NewDebugComponentCallbackUPP", new_DebugComponentCallback },
    { "DisposeDeferredTaskUPP", dispose_upp },
    { "InvokeDeferredTaskUPP", invoke_DeferredTask },
    { "NewDeferredTaskUPP", new_DeferredTask },
    { "DisposeDisposeColorPickMethodUPP", dispose_upp },
    { "InvokeDisposeColorPickMethodUPP", invoke_DisposeColorPickMethod },
    { "NewDisposeColorPickMethodUPP", new_DisposeColorPickMethod },
    { "DisposeDoMCActionUPP", dispose_upp },
    { "InvokeDoMCActionUPP", invoke_DoMCAction },
    { "NewDoMCActionUPP", new_DoMCAction },
    { "DisposeEndDocumentHandlerUPP", dispose_upp },
    { "InvokeEndDocumentHandlerUPP", invoke_EndDocumentHandler },
    { "NewEndDocumentHandlerUPP", new_EndDocumentHandler },
    { "DisposeEndElementHandlerUPP", dispose_upp },
    { "InvokeEndElementHandlerUPP", invoke_EndElementHandler },
    { "NewEndElementHandlerUPP", new_EndElementHandler },
    { "DisposeExceptionHandlerUPP", dispose_upp },
    { "InvokeExceptionHandlerUPP", invoke_ExceptionHandler },
    { "NewExceptionHandlerUPP", new_ExceptionHandler },
    { "DisposeFBCCallbackUPP", dispose_upp },
    { "InvokeFBCCallbackUPP", invoke_FBCCallback },
    { "NewFBCCallbackUPP", new_FBCCallback },
    { "DisposeFMFontCallbackFilterUPP", dispose_upp },
    { "InvokeFMFontCallbackFilterUPP", invoke_FMFontCallbackFilter },
    { "NewFMFontCallbackFilterUPP", new_FMFontCallbackFilter },
    { "DisposeFMFontFamilyCallbackFilterUPP", dispose_upp },
    { "InvokeFMFontFamilyCallbackFilterUPP", invoke_FMFontFamilyCallbackFilter },
    { "NewFMFontFamilyCallbackFilterUPP", new_FMFontFamilyCallbackFilter },
    { "DisposeFolderManagerNotificationUPP", dispose_upp },
    { "InvokeFolderManagerNotificationUPP", invoke_FolderManagerNotification },
    { "NewFolderManagerNotificationUPP", new_FolderManagerNotification },
    { "DisposeGetMissingComponentResourceUPP", dispose_upp },
    { "InvokeGetMissingComponentResourceUPP", invoke_GetMissingComponentResource },
    { "NewGetMissingComponentResourceUPP", new_GetMissingComponentResource },
    { "DisposeGetMovieUPP", dispose_upp },
    { "InvokeGetMovieUPP", invoke_GetMovie },
    { "NewGetMovieUPP", new_GetMovie },
    { "DisposeGetScrapDataUPP", dispose_upp },
    { "InvokeGetScrapDataUPP", invoke_GetScrapData },
    { "NewGetScrapDataUPP", new_GetScrapData },
    { "DisposeGrowZoneUPP", dispose_upp },
    { "InvokeGrowZoneUPP", invoke_GrowZone },
    { "NewGrowZoneUPP", new_GrowZone },
    { "DisposeHDSpindownUPP", dispose_upp },
    { "InvokeHDSpindownUPP", invoke_HDSpindown },
    { "NewHDSpindownUPP", new_HDSpindown },
    { "DisposeHRNewCFURLUPP", dispose_upp },
    { "InvokeHRNewCFURLUPP", invoke_HRNewCFURL },
    { "NewHRNewCFURLUPP", new_HRNewCFURL },
    { "DisposeHRNewURLUPP", dispose_upp },
    { "InvokeHRNewURLUPP", invoke_HRNewURL },
    { "NewHRNewURLUPP", new_HRNewURL },
    { "DisposeHRURLToFSRefUPP", dispose_upp },
    { "InvokeHRURLToFSRefUPP", invoke_HRURLToFSRef },
    { "NewHRURLToFSRefUPP", new_HRURLToFSRef },
    { "DisposeHRURLToFSSpecUPP", dispose_upp },
    { "InvokeHRURLToFSSpecUPP", invoke_HRURLToFSSpec },
    { "NewHRURLToFSSpecUPP", new_HRURLToFSSpec },
    { "DisposeHRWasCFURLVisitedUPP", dispose_upp },
    { "InvokeHRWasCFURLVisitedUPP", invoke_HRWasCFURLVisited },
    { "NewHRWasCFURLVisitedUPP", new_HRWasCFURLVisited },
    { "DisposeHRWasURLVisitedUPP", dispose_upp },
    { "InvokeHRWasURLVisitedUPP", invoke_HRWasURLVisited },
    { "NewHRWasURLVisitedUPP", new_HRWasURLVisited },
    { "DisposeICMAlignmentUPP", dispose_upp },
    { "InvokeICMAlignmentUPP", invoke_ICMAlignment },
    { "NewICMAlignmentUPP", new_ICMAlignment },
    { "DisposeICMCompletionUPP", dispose_upp },
    { "InvokeICMCompletionUPP", invoke_ICMCompletion },
    { "NewICMCompletionUPP", new_ICMCompletion },
    { "DisposeICMConvertDataFormatUPP", dispose_upp },
    { "InvokeICMConvertDataFormatUPP", invoke_ICMConvertDataFormat },
    { "NewICMConvertDataFormatUPP", new_ICMConvertDataFormat },
    { "DisposeICMCursorShieldedUPP", dispose_upp },
    { "InvokeICMCursorShieldedUPP", invoke_ICMCursorShielded },
    { "NewICMCursorShieldedUPP", new_ICMCursorShielded },
    { "DisposeICMDataUPP", dispose_upp },
    { "InvokeICMDataUPP", invoke_ICMData },
    { "NewICMDataUPP", new_ICMData },
    { "DisposeICMFlushUPP", dispose_upp },
    { "InvokeICMFlushUPP", invoke_ICMFlush },
    { "NewICMFlushUPP", new_ICMFlush },
    { "DisposeICMMemoryDisposedUPP", dispose_upp },
    { "InvokeICMMemoryDisposedUPP", invoke_ICMMemoryDisposed },
    { "NewICMMemoryDisposedUPP", new_ICMMemoryDisposed },
    { "DisposeICMProgressUPP", dispose_upp },
    { "InvokeICMProgressUPP", invoke_ICMProgress },
    { "NewICMProgressUPP", new_ICMProgress },
    { "DisposeIOCompletionUPP", dispose_upp },
    { "InvokeIOCompletionUPP", invoke_IOCompletion },
    { "NewIOCompletionUPP", new_IOCompletion },
    { "DisposeIconActionUPP", dispose_upp },
    { "InvokeIconActionUPP", invoke_IconAction },
    { "NewIconActionUPP", new_IconAction },
    { "DisposeIconGetterUPP", dispose_upp },
    { "InvokeIconGetterUPP", invoke_IconGetter },
    { "NewIconGetterUPP", new_IconGetter },
    { "DisposeImageCodecDrawBandCompleteUPP", dispose_upp },
    { "InvokeImageCodecDrawBandCompleteUPP", invoke_ImageCodecDrawBandComplete },
    { "NewImageCodecDrawBandCompleteUPP", new_ImageCodecDrawBandComplete },
    { "DisposeImageCodecMPDrawBandUPP", dispose_upp },
    { "InvokeImageCodecMPDrawBandUPP", invoke_ImageCodecMPDrawBand },
    { "NewImageCodecMPDrawBandUPP", new_ImageCodecMPDrawBand },
    { "DisposeImageCodecTimeTriggerUPP", dispose_upp },
    { "InvokeImageCodecTimeTriggerUPP", invoke_ImageCodecTimeTrigger },
    { "NewImageCodecTimeTriggerUPP", new_ImageCodecTimeTrigger },
    { "DisposeIndexToStringUPP", dispose_upp },
    { "InvokeIndexToStringUPP", invoke_IndexToString },
    { "NewIndexToStringUPP", new_IndexToString },
    { "DisposeInitPickMethodUPP", dispose_upp },
    { "InvokeInitPickMethodUPP", invoke_InitPickMethod },
    { "NewInitPickMethodUPP", new_InitPickMethod },
    { "DisposeKCCallbackUPP", dispose_upp },
    { "InvokeKCCallbackUPP", invoke_KCCallback },
    { "NewKCCallbackUPP", new_KCCallback },
    { "DisposeMCActionFilterUPP", dispose_upp },
    { "InvokeMCActionFilterUPP", invoke_MCActionFilter },
    { "NewMCActionFilterUPP", new_MCActionFilter },
    { "DisposeMCActionFilterWithRefConUPP", dispose_upp },
    { "InvokeMCActionFilterWithRefConUPP", invoke_MCActionFilterWithRefCon },
    { "NewMCActionFilterWithRefConUPP", new_MCActionFilterWithRefCon },
    { "DisposeMovieDrawingCompleteUPP", dispose_upp },
    { "InvokeMovieDrawingCompleteUPP", invoke_MovieDrawingComplete },
    { "NewMovieDrawingCompleteUPP", new_MovieDrawingComplete },
    { "DisposeMovieExecuteWiredActionsUPP", dispose_upp },
    { "InvokeMovieExecuteWiredActionsUPP", invoke_MovieExecuteWiredActions },
    { "NewMovieExecuteWiredActionsUPP", new_MovieExecuteWiredActions },
    { "DisposeMovieExportGetDataUPP", dispose_upp },
    { "InvokeMovieExportGetDataUPP", invoke_MovieExportGetData },
    { "NewMovieExportGetDataUPP", new_MovieExportGetData },
    { "DisposeMovieExportGetPropertyUPP", dispose_upp },
    { "InvokeMovieExportGetPropertyUPP", invoke_MovieExportGetProperty },
    { "NewMovieExportGetPropertyUPP", new_MovieExportGetProperty },
    { "DisposeMoviePrePrerollCompleteUPP", dispose_upp },
    { "InvokeMoviePrePrerollCompleteUPP", invoke_MoviePrePrerollComplete },
    { "NewMoviePrePrerollCompleteUPP", new_MoviePrePrerollComplete },
    { "DisposeMoviePreviewCallOutUPP", dispose_upp },
    { "InvokeMoviePreviewCallOutUPP", invoke_MoviePreviewCallOut },
    { "NewMoviePreviewCallOutUPP", new_MoviePreviewCallOut },
    { "DisposeMovieProgressUPP", dispose_upp },
    { "InvokeMovieProgressUPP", invoke_MovieProgress },
    { "NewMovieProgressUPP", new_MovieProgress },
    { "DisposeMovieRgnCoverUPP", dispose_upp },
    { "InvokeMovieRgnCoverUPP", invoke_MovieRgnCover },
    { "NewMovieRgnCoverUPP", new_MovieRgnCover },
    { "DisposeMoviesErrorUPP", dispose_upp },
    { "InvokeMoviesErrorUPP", invoke_MoviesError },
    { "NewMoviesErrorUPP", new_MoviesError },
    { "DisposeMusicMIDISendUPP", dispose_upp },
    { "InvokeMusicMIDISendUPP", invoke_MusicMIDISend },
    { "NewMusicMIDISendUPP", new_MusicMIDISend },
    { "DisposeMusicOfflineDataUPP", dispose_upp },
    { "InvokeMusicOfflineDataUPP", invoke_MusicOfflineData },
    { "NewMusicOfflineDataUPP", new_MusicOfflineData },
    { "DisposeNColorChangedUPP", dispose_upp },
    { "InvokeNColorChangedUPP", invoke_NColorChanged },
    { "NewNColorChangedUPP", new_NColorChanged },
    { "DisposeNSLClientNotifyUPP", dispose_upp },
    { "InvokeNSLClientNotifyUPP", invoke_NSLClientNotify },
    { "NewNSLClientNotifyUPP", new_NSLClientNotify },
    { "DisposeNSLEventUPP", dispose_upp },
    { "InvokeNSLEventUPP", invoke_NSLEvent },
    { "NewNSLEventUPP", new_NSLEvent },
    { "DisposeNSLMgrNotifyUPP", dispose_upp },
    { "InvokeNSLMgrNotifyUPP", invoke_NSLMgrNotify },
    { "NewNSLMgrNotifyUPP", new_NSLMgrNotify },
    { "DisposeNSLURLFilterUPP", dispose_upp },
    { "InvokeNSLURLFilterUPP", invoke_NSLURLFilter },
    { "NewNSLURLFilterUPP", new_NSLURLFilter },
    { "DisposeOSAActiveUPP", dispose_upp },
    { "InvokeOSAActiveUPP", invoke_OSAActive },
    { "NewOSAActiveUPP", new_OSAActive },
    { "DisposeOSACreateAppleEventUPP", dispose_upp },
    { "InvokeOSACreateAppleEventUPP", invoke_OSACreateAppleEvent },
    { "NewOSACreateAppleEventUPP", new_OSACreateAppleEvent },
    { "DisposeOSASendUPP", dispose_upp },
    { "InvokeOSASendUPP", invoke_OSASend },
    { "NewOSASendUPP", new_OSASend },
    { "DisposeOSLAccessorUPP", dispose_upp },
    { "InvokeOSLAccessorUPP", invoke_OSLAccessor },
    { "NewOSLAccessorUPP", new_OSLAccessor },
    { "DisposeOSLAdjustMarksUPP", dispose_upp },
    { "InvokeOSLAdjustMarksUPP", invoke_OSLAdjustMarks },
    { "NewOSLAdjustMarksUPP", new_OSLAdjustMarks },
    { "DisposeOSLCompareUPP", dispose_upp },
    { "InvokeOSLCompareUPP", invoke_OSLCompare },
    { "NewOSLCompareUPP", new_OSLCompare },
    { "DisposeOSLCountUPP", dispose_upp },
    { "InvokeOSLCountUPP", invoke_OSLCount },
    { "NewOSLCountUPP", new_OSLCount },
    { "DisposeOSLDisposeTokenUPP", dispose_upp },
    { "InvokeOSLDisposeTokenUPP", invoke_OSLDisposeToken },
    { "NewOSLDisposeTokenUPP", new_OSLDisposeToken },
    { "DisposeOSLGetErrDescUPP", dispose_upp },
    { "InvokeOSLGetErrDescUPP", invoke_OSLGetErrDesc },
    { "NewOSLGetErrDescUPP", new_OSLGetErrDesc },
    { "DisposeOSLGetMarkTokenUPP", dispose_upp },
    { "InvokeOSLGetMarkTokenUPP", invoke_OSLGetMarkToken },
    { "NewOSLGetMarkTokenUPP", new_OSLGetMarkToken },
    { "DisposeOSLMarkUPP", dispose_upp },
    { "InvokeOSLMarkUPP", invoke_OSLMark },
    { "NewOSLMarkUPP", new_OSLMark },
    { "DisposeOTListSearchUPP", dispose_upp },
    { "InvokeOTListSearchUPP", invoke_OTListSearch },
    { "NewOTListSearchUPP", new_OTListSearch },
    { "DisposeOTNotifyUPP", dispose_upp },
    { "InvokeOTNotifyUPP", invoke_OTNotify },
    { "NewOTNotifyUPP", new_OTNotify },
    { "DisposeOTProcessUPP", dispose_upp },
    { "InvokeOTProcessUPP", invoke_OTProcess },
    { "NewOTProcessUPP", new_OTProcess },
    { "DisposePMgrStateChangeUPP", dispose_upp },
    { "InvokePMgrStateChangeUPP", invoke_PMgrStateChange },
    { "NewPMgrStateChangeUPP", new_PMgrStateChange },
    { "DisposePreprocessInstructionHandlerUPP", dispose_upp },
    { "InvokePreprocessInstructionHandlerUPP", invoke_PreprocessInstructionHandler },
    { "NewPreprocessInstructionHandlerUPP", new_PreprocessInstructionHandler },
    { "DisposePurgeUPP", dispose_upp },
    { "InvokePurgeUPP", invoke_Purge },
    { "NewPurgeUPP", new_Purge },
    { "DisposeQDPixUPP", dispose_upp },
    { "InvokeQDPixUPP", invoke_QDPix },
    { "NewQDPixUPP", new_QDPix },
    { "DisposeQTBandwidthNotificationUPP", dispose_upp },
    { "InvokeQTBandwidthNotificationUPP", invoke_QTBandwidthNotification },
    { "NewQTBandwidthNotificationUPP", new_QTBandwidthNotification },
    { "DisposeQTCallBackUPP", dispose_upp },
    { "InvokeQTCallBackUPP", invoke_QTCallBack },
    { "NewQTCallBackUPP", new_QTCallBack },
    { "DisposeQTSModalFilterUPP", dispose_upp },
    { "InvokeQTSModalFilterUPP", invoke_QTSModalFilter },
    { "NewQTSModalFilterUPP", new_QTSModalFilter },
    { "DisposeQTSNotificationUPP", dispose_upp },
    { "InvokeQTSNotificationUPP", invoke_QTSNotification },
    { "NewQTSNotificationUPP", new_QTSNotification },
    { "DisposeQTSyncTaskUPP", dispose_upp },
    { "InvokeQTSyncTaskUPP", invoke_QTSyncTask },
    { "NewQTSyncTaskUPP", new_QTSyncTask },
    { "DisposeQTVRBackBufferImagingUPP", dispose_upp },
    { "InvokeQTVRBackBufferImagingUPP", invoke_QTVRBackBufferImaging },
    { "NewQTVRBackBufferImagingUPP", new_QTVRBackBufferImaging },
    { "DisposeQTVREnteringNodeUPP", dispose_upp },
    { "InvokeQTVREnteringNodeUPP", invoke_QTVREnteringNode },
    { "NewQTVREnteringNodeUPP", new_QTVREnteringNode },
    { "DisposeQTVRImagingCompleteUPP", dispose_upp },
    { "InvokeQTVRImagingCompleteUPP", invoke_QTVRImagingComplete },
    { "NewQTVRImagingCompleteUPP", new_QTVRImagingComplete },
    { "DisposeQTVRInterceptUPP", dispose_upp },
    { "InvokeQTVRInterceptUPP", invoke_QTVRIntercept },
    { "NewQTVRInterceptUPP", new_QTVRIntercept },
    { "DisposeQTVRLeavingNodeUPP", dispose_upp },
    { "InvokeQTVRLeavingNodeUPP", invoke_QTVRLeavingNode },
    { "NewQTVRLeavingNodeUPP", new_QTVRLeavingNode },
    { "DisposeQTVRMouseOverHotSpotUPP", dispose_upp },
    { "InvokeQTVRMouseOverHotSpotUPP", invoke_QTVRMouseOverHotSpot },
    { "NewQTVRMouseOverHotSpotUPP", new_QTVRMouseOverHotSpot },
    { "DisposeRTPMPDataReleaseUPP", dispose_upp },
    { "InvokeRTPMPDataReleaseUPP", invoke_RTPMPDataRelease },
    { "NewRTPMPDataReleaseUPP", new_RTPMPDataRelease },
    { "DisposeRTPPBCallbackUPP", dispose_upp },
    { "InvokeRTPPBCallbackUPP", invoke_RTPPBCallback },
    { "NewRTPPBCallbackUPP", new_RTPPBCallback },
    { "DisposeRecordColorsUPP", dispose_upp },
    { "InvokeRecordColorsUPP", invoke_RecordColors },
    { "NewRecordColorsUPP", new_RecordColors },
    { "DisposeResErrUPP", dispose_upp },
    { "InvokeResErrUPP", invoke_ResErr },
    { "NewResErrUPP", new_ResErr },
    { "DisposeSCModalFilterUPP", dispose_upp },
    { "InvokeSCModalFilterUPP", invoke_SCModalFilter },
    { "NewSCModalFilterUPP", new_SCModalFilter },
    { "DisposeSCModalHookUPP", dispose_upp },
    { "InvokeSCModalHookUPP", invoke_SCModalHook },
    { "NewSCModalHookUPP", new_SCModalHook },
    { "DisposeSCSICallbackUPP", dispose_upp },
    { "InvokeSCSICallbackUPP", invoke_SCSICallback },
    { "NewSCSICallbackUPP", new_SCSICallback },
    { "DisposeSGAddFrameBottleUPP", dispose_upp },
    { "InvokeSGAddFrameBottleUPP", invoke_SGAddFrameBottle },
    { "NewSGAddFrameBottleUPP", new_SGAddFrameBottle },
    { "DisposeSGCompressBottleUPP", dispose_upp },
    { "InvokeSGCompressBottleUPP", invoke_SGCompressBottle },
    { "NewSGCompressBottleUPP", new_SGCompressBottle },
    { "DisposeSGCompressCompleteBottleUPP", dispose_upp },
    { "InvokeSGCompressCompleteBottleUPP", invoke_SGCompressCompleteBottle },
    { "NewSGCompressCompleteBottleUPP", new_SGCompressCompleteBottle },
    { "DisposeSGDataUPP", dispose_upp },
    { "InvokeSGDataUPP", invoke_SGData },
    { "NewSGDataUPP", new_SGData },
    { "DisposeSGDisplayBottleUPP", dispose_upp },
    { "InvokeSGDisplayBottleUPP", invoke_SGDisplayBottle },
    { "NewSGDisplayBottleUPP", new_SGDisplayBottle },
    { "DisposeSGDisplayCompressBottleUPP", dispose_upp },
    { "InvokeSGDisplayCompressBottleUPP", invoke_SGDisplayCompressBottle },
    { "NewSGDisplayCompressBottleUPP", new_SGDisplayCompressBottle },
    { "DisposeSGGrabBottleUPP", dispose_upp },
    { "InvokeSGGrabBottleUPP", invoke_SGGrabBottle },
    { "NewSGGrabBottleUPP", new_SGGrabBottle },
    { "DisposeSGGrabCompleteBottleUPP", dispose_upp },
    { "InvokeSGGrabCompleteBottleUPP", invoke_SGGrabCompleteBottle },
    { "NewSGGrabCompleteBottleUPP", new_SGGrabCompleteBottle },
    { "DisposeSGGrabCompressCompleteBottleUPP", dispose_upp },
    { "InvokeSGGrabCompressCompleteBottleUPP", invoke_SGGrabCompressCompleteBottle },
    { "NewSGGrabCompressCompleteBottleUPP", new_SGGrabCompressCompleteBottle },
    { "DisposeSGModalFilterUPP", dispose_upp },
    { "InvokeSGModalFilterUPP", invoke_SGModalFilter },
    { "NewSGModalFilterUPP", new_SGModalFilter },
    { "DisposeSGTransferFrameBottleUPP", dispose_upp },
    { "InvokeSGTransferFrameBottleUPP", invoke_SGTransferFrameBottle },
    { "NewSGTransferFrameBottleUPP", new_SGTransferFrameBottle },
    { "DisposeSICompletionUPP", dispose_upp },
    { "InvokeSICompletionUPP", invoke_SICompletion },
    { "NewSICompletionUPP", new_SICompletion },
    { "DisposeSIInterruptUPP", dispose_upp },
    { "InvokeSIInterruptUPP", invoke_SIInterrupt },
    { "NewSIInterruptUPP", new_SIInterrupt },
    { "DisposeSRCallBackUPP", dispose_upp },
    { "InvokeSRCallBackUPP", invoke_SRCallBack },
    { "NewSRCallBackUPP", new_SRCallBack },
    { "DisposeSelectorFunctionUPP", dispose_upp },
    { "InvokeSelectorFunctionUPP", invoke_SelectorFunction },
    { "NewSelectorFunctionUPP", new_SelectorFunction },
    { "DisposeSleepQUPP", dispose_upp },
    { "InvokeSleepQUPP", invoke_SleepQ },
    { "NewSleepQUPP", new_SleepQ },
    { "DisposeSndCallBackUPP", dispose_upp },
    { "InvokeSndCallBackUPP", invoke_SndCallBack },
    { "NewSndCallBackUPP", new_SndCallBack },
    { "DisposeSoundConverterFillBufferDataUPP", dispose_upp },
    { "InvokeSoundConverterFillBufferDataUPP", invoke_SoundConverterFillBufferData },
    { "NewSoundConverterFillBufferDataUPP", new_SoundConverterFillBufferData },
    { "DisposeSoundParamUPP", dispose_upp },
    { "InvokeSoundParamUPP", invoke_SoundParam },
    { "NewSoundParamUPP", new_SoundParam },
    { "DisposeSpeechDoneUPP", dispose_upp },
    { "InvokeSpeechDoneUPP", invoke_SpeechDone },
    { "NewSpeechDoneUPP", new_SpeechDone },
    { "DisposeSpeechErrorUPP", dispose_upp },
    { "InvokeSpeechErrorUPP", invoke_SpeechError },
    { "NewSpeechErrorUPP", new_SpeechError },
    { "DisposeSpeechPhonemeUPP", dispose_upp },
    { "InvokeSpeechPhonemeUPP", invoke_SpeechPhoneme },
    { "NewSpeechPhonemeUPP", new_SpeechPhoneme },
    { "DisposeSpeechSyncUPP", dispose_upp },
    { "InvokeSpeechSyncUPP", invoke_SpeechSync },
    { "NewSpeechSyncUPP", new_SpeechSync },
    { "DisposeSpeechTextDoneUPP", dispose_upp },
    { "InvokeSpeechTextDoneUPP", invoke_SpeechTextDone },
    { "NewSpeechTextDoneUPP", new_SpeechTextDone },
    { "DisposeSpeechWordUPP", dispose_upp },
    { "InvokeSpeechWordUPP", invoke_SpeechWord },
    { "NewSpeechWordUPP", new_SpeechWord },
    { "DisposeStartDocumentHandlerUPP", dispose_upp },
    { "InvokeStartDocumentHandlerUPP", invoke_StartDocumentHandler },
    { "NewStartDocumentHandlerUPP", new_StartDocumentHandler },
    { "DisposeStartElementHandlerUPP", dispose_upp },
    { "InvokeStartElementHandlerUPP", invoke_StartElementHandler },
    { "NewStartElementHandlerUPP", new_StartElementHandler },
    { "DisposeStdPixUPP", dispose_upp },
    { "InvokeStdPixUPP", invoke_StdPix },
    { "NewStdPixUPP", new_StdPix },
    { "DisposeTSMTEPostUpdateUPP", dispose_upp },
    { "InvokeTSMTEPostUpdateUPP", invoke_TSMTEPostUpdate },
    { "NewTSMTEPostUpdateUPP", new_TSMTEPostUpdate },
    { "DisposeTSMTEPreUpdateUPP", dispose_upp },
    { "InvokeTSMTEPreUpdateUPP", invoke_TSMTEPreUpdate },
    { "NewTSMTEPreUpdateUPP", new_TSMTEPreUpdate },
    { "DisposeTextMediaUPP", dispose_upp },
    { "InvokeTextMediaUPP", invoke_TextMedia },
    { "NewTextMediaUPP", new_TextMedia },
    { "DisposeTrackTransferUPP", dispose_upp },
    { "InvokeTrackTransferUPP", invoke_TrackTransfer },
    { "NewTrackTransferUPP", new_TrackTransfer },
    { "DisposeTuneCallBackUPP", dispose_upp },
    { "InvokeTuneCallBackUPP", invoke_TuneCallBack },
    { "NewTuneCallBackUPP", new_TuneCallBack },
    { "DisposeTunePlayCallBackUPP", dispose_upp },
    { "InvokeTunePlayCallBackUPP", invoke_TunePlayCallBack },
    { "NewTunePlayCallBackUPP", new_TunePlayCallBack },
    { "DisposeTweenerDataUPP", dispose_upp },
    { "InvokeTweenerDataUPP", invoke_TweenerData },
    { "NewTweenerDataUPP", new_TweenerData },
    { "DisposeURLNotifyUPP", dispose_upp },
    { "InvokeURLNotifyUPP", invoke_URLNotify },
    { "NewURLNotifyUPP", new_URLNotify },
    { "DisposeURLSystemEventUPP", dispose_upp },
    { "InvokeURLSystemEventUPP", invoke_URLSystemEvent },
    { "NewURLSystemEventUPP", new_URLSystemEvent },
    { "DisposeUnicodeToTextFallbackUPP", dispose_upp },
    { "InvokeUnicodeToTextFallbackUPP", invoke_UnicodeToTextFallback },
    { "NewUnicodeToTextFallbackUPP", new_UnicodeToTextFallback },
    { "DisposeUserEventUPP", dispose_upp },
    { "InvokeUserEventUPP", invoke_UserEvent },
    { "NewUserEventUPP", new_UserEvent },
    { "DisposeUserFnUPP", dispose_upp },
    { "InvokeUserFnUPP", invoke_UserFn },
    { "NewUserFnUPP", new_UserFn },
    { "DisposeVdigIntUPP", dispose_upp },
    { "InvokeVdigIntUPP", invoke_VdigInt },
    { "NewVdigIntUPP", new_VdigInt },
};
const unsigned carbonupps_export_count = sizeof carbonupps_exports / sizeof carbonupps_exports[0];
