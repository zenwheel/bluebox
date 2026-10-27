/*
 * QuickTimeLib's calls to the sequence grabber ('barg'), its channels
 * ('sgch'), video digitizers ('vdig') and movie import and export
 * components: each a call to the instance through the Component Manager,
 * with its selector and the sizes of its parameters.  Blue Box has no
 * such components, so the calls answer as for an instance that isn't
 * there (badComponentInstance); a program that opened none finds that
 * out rather than calling address 0 (VideoIN, BigEye, MacCRO and Marine
 * Aquarium, which import them weakly).
 */
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])

/* The instance in r3, then the parameters (4, 2, or 1 for a Boolean or a
 * byte, pushed as a word with the byte high), the parameter size and the
 * selector: _ComponentDispatch with D0 = 0. */
static void instance_call(ppc_cpu *c, int16_t what, const char *args)
{
    ppc_trap_begin();
    ppc_trap_push(4, 0);
    ppc_trap_push(4, R(0));
    uint32_t size = 0;
    for (int i = 0; args[i]; i++) {
        int n = args[i] - '0';
        ppc_trap_push(n, R(1 + i));
        size += n == 4 ? 4 : 2;
    }
    ppc_trap_push(4, size << 16 | (uint16_t)what);
    ppc_trap_set(0, 0);
    ppc_trap(0xA82A);
    uint32_t r = ppc_trap_result(4);
    ppc_trap_end();
    c->gpr[3] = r;
    c->pc = c->lr;
}

#define CCALL(name, what, args) static void cc_##name(ppc_cpu *c) { instance_call(c, what, args); }

CCALL(MovieExportAddDataSource, 0x8A, "444444")
CCALL(MovieExportDisposeGetDataAndPropertiesProcs, 0x101, "444")
CCALL(MovieExportDoUserDialog, 0x86, "44444")
CCALL(MovieExportFromProceduresToDataRef, 0x89, "44")
CCALL(MovieExportGetAuxiliaryData, 0x83, "44")
CCALL(MovieExportGetCreatorType, 0x87, "4")
CCALL(MovieExportGetFileNameExtension, 0x8E, "4")
CCALL(MovieExportGetSettingsAsAtomContainer, 0x8C, "4")
CCALL(MovieExportGetShortFileTypeString, 0x8F, "4")
CCALL(MovieExportGetSourceMediaType, 0x90, "4")
CCALL(MovieExportNewGetDataAndPropertiesProcs, 0x100, "444444444")
CCALL(MovieExportSetGetMoviePropertyProc, 0x91, "44")
CCALL(MovieExportSetProgressProc, 0x84, "44")
CCALL(MovieExportSetSampleDescription, 0x85, "44")
CCALL(MovieExportSetSettingsFromAtomContainer, 0x8D, "4")
CCALL(MovieExportToDataRef, 0x88, "444444")
CCALL(MovieExportToFile, 0x81, "44444")
CCALL(MovieExportToHandle, 0x80, "44444")
CCALL(MovieExportValidate, 0x8B, "444")
CCALL(MovieImportDataRef, 0x10, "444444444")
CCALL(MovieImportDoUserDialog, 0xB, "444")
CCALL(MovieImportEstimateCompletionTime, 0x1B, "4")
CCALL(MovieImportFile, 0x2, "44444444")
CCALL(MovieImportGetAuxiliaryDataType, 0xD, "4")
CCALL(MovieImportGetDontBlock, 0x1D, "4")
CCALL(MovieImportGetFileType, 0xF, "4")
CCALL(MovieImportGetLoadState, 0x19, "4")
CCALL(MovieImportGetMIMETypeList, 0x12, "4")
CCALL(MovieImportGetMaxLoadedTime, 0x1A, "4")
CCALL(MovieImportGetSampleDescription, 0x11, "44")
CCALL(MovieImportGetSettingsAsAtomContainer, 0x14, "4")
CCALL(MovieImportHandle, 0x1, "44444444")
CCALL(MovieImportIdle, 0x17, "44")
CCALL(MovieImportSetAuxiliaryData, 0x9, "44")
CCALL(MovieImportSetChunkSize, 0x7, "4")
CCALL(MovieImportSetDimensions, 0x6, "44")
CCALL(MovieImportSetDontBlock, 0x1C, "1")
CCALL(MovieImportSetDuration, 0xC, "4")
CCALL(MovieImportSetFromScrap, 0xA, "1")
CCALL(MovieImportSetMediaFile, 0x5, "4")
CCALL(MovieImportSetOffsetAndLimit, 0x13, "44")
CCALL(MovieImportSetOffsetAndLimit64, 0x16, "44")
CCALL(MovieImportSetProgressProc, 0x8, "44")
CCALL(MovieImportSetSampleDescription, 0x4, "44")
CCALL(MovieImportSetSampleDuration, 0x3, "44")
CCALL(MovieImportSetSettingsFromAtomContainer, 0x15, "4")
CCALL(MovieImportValidate, 0xE, "444")
CCALL(MovieImportValidateDataRef, 0x18, "444")
CCALL(SGAddExtendedFrameReference, 0x107, "4")
CCALL(SGAddExtendedMovieData, 0x109, "44444424")
CCALL(SGAddFrame, 0x111, "2444")
CCALL(SGAddFrameReference, 0x101, "4")
CCALL(SGAddMovieData, 0x105, "4444442")
CCALL(SGAddOutputDataRefToMedia, 0x10A, "444")
CCALL(SGAlignChannelRect, 0x183, "4")
CCALL(SGAppendDeviceListToMenu, 0x23, "44")
CCALL(SGChangedSource, 0x106, "4")
CCALL(SGChannelGetCodecSettings, 0x9A, "4")
CCALL(SGChannelGetDataSourceName, 0x98, "44")
CCALL(SGChannelGetRequestedDataRate, 0x96, "4")
CCALL(SGChannelPutPicture, 0x94, "")
CCALL(SGChannelSetCodecSettings, 0x99, "4")
CCALL(SGChannelSetDataSourceName, 0x97, "42")
CCALL(SGChannelSetRequestedDataRate, 0x95, "4")
CCALL(SGCompressFrame, 0x10F, "2")
CCALL(SGCompressFrameComplete, 0x110, "244")
CCALL(SGDisplayCompress, 0x119, "4444")
CCALL(SGDisplayFrame, 0x10E, "244")
CCALL(SGDisposeChannel, 0x7, "4")
CCALL(SGDisposeDeviceList, 0x22, "4")
CCALL(SGDisposeOutput, 0x31, "4")
CCALL(SGGetAdditionalSoundRates, 0x10A, "4")
CCALL(SGGetAlignmentProc, 0x2A, "4")
CCALL(SGGetBufferInfo, 0x115, "24444")
CCALL(SGGetChannelBounds, 0x83, "4")
CCALL(SGGetChannelClip, 0x8D, "4")
CCALL(SGGetChannelDeviceList, 0x8F, "44")
CCALL(SGGetChannelInfo, 0x86, "4")
CCALL(SGGetChannelMatrix, 0x92, "4")
CCALL(SGGetChannelMaxFrames, 0x8A, "4")
CCALL(SGGetChannelPlayFlags, 0x88, "4")
CCALL(SGGetChannelSampleDescription, 0x8E, "4")
CCALL(SGGetChannelSettings, 0x2C, "444")
CCALL(SGGetChannelTimeBase, 0x9B, "4")
CCALL(SGGetChannelTimeScale, 0x93, "4")
CCALL(SGGetChannelUsage, 0x81, "4")
CCALL(SGGetChannelVolume, 0x85, "4")
CCALL(SGGetCompressBuffer, 0x114, "44")
CCALL(SGGetDataOutput, 0x3, "44")
CCALL(SGGetDataOutputStorageSpaceRemaining, 0x34, "44")
CCALL(SGGetDataOutputStorageSpaceRemaining64, 0x3D, "44")
CCALL(SGGetDataRate, 0x182, "4")
CCALL(SGGetDataRef, 0x2F, "444")
CCALL(SGGetFlags, 0x1F, "4")
CCALL(SGGetFrameRate, 0x11B, "4")
CCALL(SGGetGWorld, 0x5, "44")
CCALL(SGGetIndChannel, 0x26, "244")
CCALL(SGGetInstrument, 0x100, "4")
CCALL(SGGetLastMovieResID, 0x1D, "4")
CCALL(SGGetMaximumRecordTime, 0x19, "4")
CCALL(SGGetMode, 0x2D, "44")
CCALL(SGGetMovie, 0x17, "")
CCALL(SGGetNextExtendedFrameReference, 0x108, "444")
CCALL(SGGetNextFrameReference, 0x102, "444")
CCALL(SGGetOutputDataReference, 0x3A, "444")
CCALL(SGGetOutputMaximumOffset, 0x39, "44")
CCALL(SGGetOutputNextOutput, 0x37, "44")
CCALL(SGGetPause, 0x28, "4")
CCALL(SGGetPreferredPacketSize, 0x122, "4")
CCALL(SGGetSettings, 0x25, "44")
CCALL(SGGetSoundInputDriver, 0x101, "")
CCALL(SGGetSoundInputParameters, 0x108, "444")
CCALL(SGGetSoundInputRate, 0x106, "")
CCALL(SGGetSoundRecordChunkSize, 0x104, "")
CCALL(SGGetSrcVideoBounds, 0x100, "4")
CCALL(SGGetStorageSpaceRemaining, 0x1A, "4")
CCALL(SGGetStorageSpaceRemaining64, 0x3C, "4")
CCALL(SGGetTextReturnToSpaceValue, 0x105, "4")
CCALL(SGGetTimeBase, 0x103, "4")
CCALL(SGGetTimeRemaining, 0x1B, "4")
CCALL(SGGetUseScreenBuffer, 0x117, "4")
CCALL(SGGetUserVideoCompressorList, 0x124, "4")
CCALL(SGGetVideoBottlenecks, 0x10B, "4")
CCALL(SGGetVideoCompressor, 0x106, "44444")
CCALL(SGGetVideoCompressorType, 0x103, "4")
CCALL(SGGetVideoDigitizerComponent, 0x107, "")
CCALL(SGGetVideoRect, 0x102, "4")
CCALL(SGGrabCompressComplete, 0x118, "444")
CCALL(SGGrabFrame, 0x10C, "2")
CCALL(SGGrabFrameComplete, 0x10D, "24")
CCALL(SGGrabPict, 0x1C, "4424")
CCALL(SGHandleUpdateEvent, 0x35, "44")
CCALL(SGIdle, 0x12, "")
CCALL(SGInitChannel, 0x180, "4")
CCALL(SGInitialize, 0x1, "")
CCALL(SGNewChannel, 0x6, "44")
CCALL(SGNewChannelFromComponent, 0x21, "44")
CCALL(SGNewOutput, 0x30, "4444")
CCALL(SGPanelCanRun, 0x202, "4")
CCALL(SGPanelEvent, 0x204, "442444")
CCALL(SGPanelGetDitl, 0x200, "4")
CCALL(SGPanelGetSettings, 0x209, "444")
CCALL(SGPanelGetTitle, 0x201, "4")
CCALL(SGPanelInstall, 0x203, "442")
CCALL(SGPanelItem, 0x205, "4422")
CCALL(SGPanelRemove, 0x206, "442")
CCALL(SGPanelSetEventFilter, 0x20C, "44")
CCALL(SGPanelSetGrabber, 0x207, "4")
CCALL(SGPanelSetResFile, 0x208, "2")
CCALL(SGPanelSetSettings, 0x20A, "444")
CCALL(SGPanelValidateInput, 0x20B, "4")
CCALL(SGPause, 0x14, "1")
CCALL(SGPrepare, 0x15, "11")
CCALL(SGRelease, 0x16, "")
CCALL(SGSetAdditionalSoundRates, 0x109, "4")
CCALL(SGSetChannelBounds, 0x82, "4")
CCALL(SGSetChannelClip, 0x8C, "4")
CCALL(SGSetChannelDevice, 0x90, "4")
CCALL(SGSetChannelMatrix, 0x91, "4")
CCALL(SGSetChannelMaxFrames, 0x89, "4")
CCALL(SGSetChannelOutput, 0x33, "44")
CCALL(SGSetChannelPlayFlags, 0x87, "4")
CCALL(SGSetChannelRefCon, 0x8B, "4")
CCALL(SGSetChannelSettings, 0x2B, "444")
CCALL(SGSetChannelUsage, 0x80, "4")
CCALL(SGSetChannelVolume, 0x84, "2")
CCALL(SGSetCompressBuffer, 0x113, "24")
CCALL(SGSetDataOutput, 0x2, "44")
CCALL(SGSetDataProc, 0x20, "44")
CCALL(SGSetDataRef, 0x2E, "444")
CCALL(SGSetFlags, 0x1E, "4")
CCALL(SGSetFontName, 0x100, "4")
CCALL(SGSetFontSize, 0x101, "2")
CCALL(SGSetFrameRate, 0x11A, "4")
CCALL(SGSetGWorld, 0x4, "44")
CCALL(SGSetInstrument, 0x101, "4")
CCALL(SGSetJustification, 0x104, "2")
CCALL(SGSetMaximumRecordTime, 0x18, "4")
CCALL(SGSetOutputFlags, 0x32, "44")
CCALL(SGSetOutputMaximumOffset, 0x38, "44")
CCALL(SGSetOutputNextOutput, 0x36, "44")
CCALL(SGSetPreferredPacketSize, 0x121, "4")
CCALL(SGSetSettings, 0x24, "44")
CCALL(SGSetSoundInputDriver, 0x100, "4")
CCALL(SGSetSoundInputParameters, 0x107, "224")
CCALL(SGSetSoundInputRate, 0x105, "4")
CCALL(SGSetSoundRecordChunkSize, 0x103, "4")
CCALL(SGSetTextBackColor, 0x103, "4")
CCALL(SGSetTextForeColor, 0x102, "4")
CCALL(SGSetTextReturnToSpaceValue, 0x106, "2")
CCALL(SGSetUseScreenBuffer, 0x116, "1")
CCALL(SGSetUserVideoCompressorList, 0x123, "4")
CCALL(SGSetVideoBottlenecks, 0x10A, "4")
CCALL(SGSetVideoCompressor, 0x105, "24444")
CCALL(SGSetVideoCompressorType, 0x104, "4")
CCALL(SGSetVideoDigitizerComponent, 0x108, "4")
CCALL(SGSetVideoRect, 0x101, "4")
CCALL(SGSettingsDialog, 0x29, "424444")
CCALL(SGSortDeviceList, 0x104, "4")
CCALL(SGSoundInputDriverChanged, 0x102, "")
CCALL(SGStartPreview, 0x10, "")
CCALL(SGStartRecord, 0x11, "")
CCALL(SGStop, 0x13, "")
CCALL(SGTransferFrameForCompress, 0x112, "244")
CCALL(SGUpdate, 0x27, "4")
CCALL(SGVideoDigitizerChanged, 0x109, "")
CCALL(SGWriteExtendedMovieData, 0x3B, "44444")
CCALL(SGWriteMovieData, 0x100, "4444")
CCALL(SGWriteSamples, 0x181, "44")
CCALL(VDAddKeyColor, 0x1D, "4")
CCALL(VDClearClipRgn, 0x27, "4")
CCALL(VDCompressDone, 0x46, "44444")
CCALL(VDCompressOneFrameAsync, 0x45, "")
CCALL(VDDone, 0x43, "2")
CCALL(VDGetActiveSrcRect, 0x2, "24")
CCALL(VDGetBlackLevelValue, 0x38, "4")
CCALL(VDGetBrightness, 0xD, "4")
CCALL(VDGetCLUTInUse, 0x28, "4")
CCALL(VDGetClipState, 0x25, "4")
CCALL(VDGetCompressionTime, 0x58, "424444")
CCALL(VDGetCompressionTypes, 0x4B, "4")
CCALL(VDGetContrast, 0x12, "4")
CCALL(VDGetCurrentFlags, 0x1A, "44")
CCALL(VDGetDMADepths, 0x50, "44")
CCALL(VDGetDataRate, 0x4E, "444")
CCALL(VDGetDigitizerInfo, 0x19, "4")
CCALL(VDGetDigitizerRect, 0x4, "4")
CCALL(VDGetFieldPreference, 0x30, "4")
CCALL(VDGetHue, 0x13, "4")
CCALL(VDGetImageDescription, 0x48, "4")
CCALL(VDGetInput, 0x3F, "4")
CCALL(VDGetInputColorSpaceMode, 0x23, "4")
CCALL(VDGetInputFormat, 0x3D, "24")
CCALL(VDGetInputGammaRecord, 0x36, "4")
CCALL(VDGetInputGammaValue, 0xB, "444")
CCALL(VDGetInputName, 0x5C, "44")
CCALL(VDGetKeyColor, 0x1C, "4")
CCALL(VDGetKeyColorRange, 0x20, "44")
CCALL(VDGetMaskPixMap, 0x6, "4")
CCALL(VDGetMaskandValue, 0x2B, "244")
CCALL(VDGetMaxAuxBuffer, 0x17, "44")
CCALL(VDGetMaxSrcRect, 0x1, "24")
CCALL(VDGetNextKeyColor, 0x1E, "4")
CCALL(VDGetNumberOfInputs, 0x3C, "4")
CCALL(VDGetPLLFilterType, 0x2A, "4")
CCALL(VDGetPlayThruDestination, 0x8, "4444")
CCALL(VDGetPreferredImageDimensions, 0x5B, "44")
CCALL(VDGetPreferredTimeScale, 0x51, "4")
CCALL(VDGetSaturation, 0x15, "4")
CCALL(VDGetSharpness, 0x14, "4")
CCALL(VDGetSoundInputDriver, 0x4F, "4")
CCALL(VDGetSoundInputSource, 0x57, "44")
CCALL(VDGetTimeCode, 0x55, "444")
CCALL(VDGetVBlankRect, 0x5, "24")
CCALL(VDGetVideoDefaults, 0x3B, "4444444")
CCALL(VDGetWhiteLevelValue, 0x3A, "4")
CCALL(VDGrabOneFrame, 0x16, "")
CCALL(VDGrabOneFrameAsync, 0x42, "2")
CCALL(VDPreflightDestination, 0x32, "4444")
CCALL(VDPreflightGlobalRect, 0x33, "44")
CCALL(VDReleaseAsyncBuffers, 0x52, "")
CCALL(VDReleaseCompressBuffer, 0x47, "4")
CCALL(VDResetCompressSequence, 0x49, "")
CCALL(VDSetBlackLevelValue, 0x37, "4")
CCALL(VDSetBrightness, 0xC, "4")
CCALL(VDSetClipRgn, 0x26, "4")
CCALL(VDSetClipState, 0x24, "2")
CCALL(VDSetCompression, 0x44, "424444")
CCALL(VDSetCompressionOnOff, 0x4A, "1")
CCALL(VDSetContrast, 0xE, "4")
CCALL(VDSetDataRate, 0x54, "4")
CCALL(VDSetDestinationPort, 0x5D, "4")
CCALL(VDSetDigitizerRect, 0x3, "4")
CCALL(VDSetDigitizerUserInterrupt, 0x21, "444")
CCALL(VDSetFieldPreference, 0x2F, "2")
CCALL(VDSetFrameRate, 0x4D, "4")
CCALL(VDSetHue, 0xF, "4")
CCALL(VDSetInput, 0x3E, "2")
CCALL(VDSetInputColorSpaceMode, 0x22, "2")
CCALL(VDSetInputGammaRecord, 0x35, "4")
CCALL(VDSetInputGammaValue, 0xA, "444")
CCALL(VDSetInputStandard, 0x40, "2")
CCALL(VDSetKeyColor, 0x1B, "4")
CCALL(VDSetKeyColorRange, 0x1F, "44")
CCALL(VDSetMasterBlendLevel, 0x2C, "4")
CCALL(VDSetPLLFilterType, 0x29, "2")
CCALL(VDSetPlayThruDestination, 0x2D, "4444")
CCALL(VDSetPlayThruGlobalRect, 0x34, "44")
CCALL(VDSetPlayThruOnOff, 0x2E, "2")
CCALL(VDSetPreferredImageDimensions, 0x5A, "44")
CCALL(VDSetPreferredPacketSize, 0x59, "4")
CCALL(VDSetSaturation, 0x11, "4")
CCALL(VDSetSharpness, 0x10, "4")
CCALL(VDSetTimeBase, 0x4C, "4")
CCALL(VDSetWhiteLevelValue, 0x39, "4")
CCALL(VDSetupBuffers, 0x41, "4")
CCALL(VDUseSafeBuffers, 0x56, "1")
CCALL(VDUseThisCLUT, 0x9, "4")
#undef CCALL

const ppc_export qtcapture_exports[] = {
    { "MovieExportAddDataSource", cc_MovieExportAddDataSource },
    { "MovieExportDisposeGetDataAndPropertiesProcs", cc_MovieExportDisposeGetDataAndPropertiesProcs },
    { "MovieExportDoUserDialog", cc_MovieExportDoUserDialog },
    { "MovieExportFromProceduresToDataRef", cc_MovieExportFromProceduresToDataRef },
    { "MovieExportGetAuxiliaryData", cc_MovieExportGetAuxiliaryData },
    { "MovieExportGetCreatorType", cc_MovieExportGetCreatorType },
    { "MovieExportGetFileNameExtension", cc_MovieExportGetFileNameExtension },
    { "MovieExportGetSettingsAsAtomContainer", cc_MovieExportGetSettingsAsAtomContainer },
    { "MovieExportGetShortFileTypeString", cc_MovieExportGetShortFileTypeString },
    { "MovieExportGetSourceMediaType", cc_MovieExportGetSourceMediaType },
    { "MovieExportNewGetDataAndPropertiesProcs", cc_MovieExportNewGetDataAndPropertiesProcs },
    { "MovieExportSetGetMoviePropertyProc", cc_MovieExportSetGetMoviePropertyProc },
    { "MovieExportSetProgressProc", cc_MovieExportSetProgressProc },
    { "MovieExportSetSampleDescription", cc_MovieExportSetSampleDescription },
    { "MovieExportSetSettingsFromAtomContainer", cc_MovieExportSetSettingsFromAtomContainer },
    { "MovieExportToDataRef", cc_MovieExportToDataRef },
    { "MovieExportToFile", cc_MovieExportToFile },
    { "MovieExportToHandle", cc_MovieExportToHandle },
    { "MovieExportValidate", cc_MovieExportValidate },
    { "MovieImportDataRef", cc_MovieImportDataRef },
    { "MovieImportDoUserDialog", cc_MovieImportDoUserDialog },
    { "MovieImportEstimateCompletionTime", cc_MovieImportEstimateCompletionTime },
    { "MovieImportFile", cc_MovieImportFile },
    { "MovieImportGetAuxiliaryDataType", cc_MovieImportGetAuxiliaryDataType },
    { "MovieImportGetDontBlock", cc_MovieImportGetDontBlock },
    { "MovieImportGetFileType", cc_MovieImportGetFileType },
    { "MovieImportGetLoadState", cc_MovieImportGetLoadState },
    { "MovieImportGetMIMETypeList", cc_MovieImportGetMIMETypeList },
    { "MovieImportGetMaxLoadedTime", cc_MovieImportGetMaxLoadedTime },
    { "MovieImportGetSampleDescription", cc_MovieImportGetSampleDescription },
    { "MovieImportGetSettingsAsAtomContainer", cc_MovieImportGetSettingsAsAtomContainer },
    { "MovieImportHandle", cc_MovieImportHandle },
    { "MovieImportIdle", cc_MovieImportIdle },
    { "MovieImportSetAuxiliaryData", cc_MovieImportSetAuxiliaryData },
    { "MovieImportSetChunkSize", cc_MovieImportSetChunkSize },
    { "MovieImportSetDimensions", cc_MovieImportSetDimensions },
    { "MovieImportSetDontBlock", cc_MovieImportSetDontBlock },
    { "MovieImportSetDuration", cc_MovieImportSetDuration },
    { "MovieImportSetFromScrap", cc_MovieImportSetFromScrap },
    { "MovieImportSetMediaFile", cc_MovieImportSetMediaFile },
    { "MovieImportSetOffsetAndLimit", cc_MovieImportSetOffsetAndLimit },
    { "MovieImportSetOffsetAndLimit64", cc_MovieImportSetOffsetAndLimit64 },
    { "MovieImportSetProgressProc", cc_MovieImportSetProgressProc },
    { "MovieImportSetSampleDescription", cc_MovieImportSetSampleDescription },
    { "MovieImportSetSampleDuration", cc_MovieImportSetSampleDuration },
    { "MovieImportSetSettingsFromAtomContainer", cc_MovieImportSetSettingsFromAtomContainer },
    { "MovieImportValidate", cc_MovieImportValidate },
    { "MovieImportValidateDataRef", cc_MovieImportValidateDataRef },
    { "SGAddExtendedFrameReference", cc_SGAddExtendedFrameReference },
    { "SGAddExtendedMovieData", cc_SGAddExtendedMovieData },
    { "SGAddFrame", cc_SGAddFrame },
    { "SGAddFrameReference", cc_SGAddFrameReference },
    { "SGAddMovieData", cc_SGAddMovieData },
    { "SGAddOutputDataRefToMedia", cc_SGAddOutputDataRefToMedia },
    { "SGAlignChannelRect", cc_SGAlignChannelRect },
    { "SGAppendDeviceListToMenu", cc_SGAppendDeviceListToMenu },
    { "SGChangedSource", cc_SGChangedSource },
    { "SGChannelGetCodecSettings", cc_SGChannelGetCodecSettings },
    { "SGChannelGetDataSourceName", cc_SGChannelGetDataSourceName },
    { "SGChannelGetRequestedDataRate", cc_SGChannelGetRequestedDataRate },
    { "SGChannelPutPicture", cc_SGChannelPutPicture },
    { "SGChannelSetCodecSettings", cc_SGChannelSetCodecSettings },
    { "SGChannelSetDataSourceName", cc_SGChannelSetDataSourceName },
    { "SGChannelSetRequestedDataRate", cc_SGChannelSetRequestedDataRate },
    { "SGCompressFrame", cc_SGCompressFrame },
    { "SGCompressFrameComplete", cc_SGCompressFrameComplete },
    { "SGDisplayCompress", cc_SGDisplayCompress },
    { "SGDisplayFrame", cc_SGDisplayFrame },
    { "SGDisposeChannel", cc_SGDisposeChannel },
    { "SGDisposeDeviceList", cc_SGDisposeDeviceList },
    { "SGDisposeOutput", cc_SGDisposeOutput },
    { "SGGetAdditionalSoundRates", cc_SGGetAdditionalSoundRates },
    { "SGGetAlignmentProc", cc_SGGetAlignmentProc },
    { "SGGetBufferInfo", cc_SGGetBufferInfo },
    { "SGGetChannelBounds", cc_SGGetChannelBounds },
    { "SGGetChannelClip", cc_SGGetChannelClip },
    { "SGGetChannelDeviceList", cc_SGGetChannelDeviceList },
    { "SGGetChannelInfo", cc_SGGetChannelInfo },
    { "SGGetChannelMatrix", cc_SGGetChannelMatrix },
    { "SGGetChannelMaxFrames", cc_SGGetChannelMaxFrames },
    { "SGGetChannelPlayFlags", cc_SGGetChannelPlayFlags },
    { "SGGetChannelSampleDescription", cc_SGGetChannelSampleDescription },
    { "SGGetChannelSettings", cc_SGGetChannelSettings },
    { "SGGetChannelTimeBase", cc_SGGetChannelTimeBase },
    { "SGGetChannelTimeScale", cc_SGGetChannelTimeScale },
    { "SGGetChannelUsage", cc_SGGetChannelUsage },
    { "SGGetChannelVolume", cc_SGGetChannelVolume },
    { "SGGetCompressBuffer", cc_SGGetCompressBuffer },
    { "SGGetDataOutput", cc_SGGetDataOutput },
    { "SGGetDataOutputStorageSpaceRemaining", cc_SGGetDataOutputStorageSpaceRemaining },
    { "SGGetDataOutputStorageSpaceRemaining64", cc_SGGetDataOutputStorageSpaceRemaining64 },
    { "SGGetDataRate", cc_SGGetDataRate },
    { "SGGetDataRef", cc_SGGetDataRef },
    { "SGGetFlags", cc_SGGetFlags },
    { "SGGetFrameRate", cc_SGGetFrameRate },
    { "SGGetGWorld", cc_SGGetGWorld },
    { "SGGetIndChannel", cc_SGGetIndChannel },
    { "SGGetInstrument", cc_SGGetInstrument },
    { "SGGetLastMovieResID", cc_SGGetLastMovieResID },
    { "SGGetMaximumRecordTime", cc_SGGetMaximumRecordTime },
    { "SGGetMode", cc_SGGetMode },
    { "SGGetMovie", cc_SGGetMovie },
    { "SGGetNextExtendedFrameReference", cc_SGGetNextExtendedFrameReference },
    { "SGGetNextFrameReference", cc_SGGetNextFrameReference },
    { "SGGetOutputDataReference", cc_SGGetOutputDataReference },
    { "SGGetOutputMaximumOffset", cc_SGGetOutputMaximumOffset },
    { "SGGetOutputNextOutput", cc_SGGetOutputNextOutput },
    { "SGGetPause", cc_SGGetPause },
    { "SGGetPreferredPacketSize", cc_SGGetPreferredPacketSize },
    { "SGGetSettings", cc_SGGetSettings },
    { "SGGetSoundInputDriver", cc_SGGetSoundInputDriver },
    { "SGGetSoundInputParameters", cc_SGGetSoundInputParameters },
    { "SGGetSoundInputRate", cc_SGGetSoundInputRate },
    { "SGGetSoundRecordChunkSize", cc_SGGetSoundRecordChunkSize },
    { "SGGetSrcVideoBounds", cc_SGGetSrcVideoBounds },
    { "SGGetStorageSpaceRemaining", cc_SGGetStorageSpaceRemaining },
    { "SGGetStorageSpaceRemaining64", cc_SGGetStorageSpaceRemaining64 },
    { "SGGetTextReturnToSpaceValue", cc_SGGetTextReturnToSpaceValue },
    { "SGGetTimeBase", cc_SGGetTimeBase },
    { "SGGetTimeRemaining", cc_SGGetTimeRemaining },
    { "SGGetUseScreenBuffer", cc_SGGetUseScreenBuffer },
    { "SGGetUserVideoCompressorList", cc_SGGetUserVideoCompressorList },
    { "SGGetVideoBottlenecks", cc_SGGetVideoBottlenecks },
    { "SGGetVideoCompressor", cc_SGGetVideoCompressor },
    { "SGGetVideoCompressorType", cc_SGGetVideoCompressorType },
    { "SGGetVideoDigitizerComponent", cc_SGGetVideoDigitizerComponent },
    { "SGGetVideoRect", cc_SGGetVideoRect },
    { "SGGrabCompressComplete", cc_SGGrabCompressComplete },
    { "SGGrabFrame", cc_SGGrabFrame },
    { "SGGrabFrameComplete", cc_SGGrabFrameComplete },
    { "SGGrabPict", cc_SGGrabPict },
    { "SGHandleUpdateEvent", cc_SGHandleUpdateEvent },
    { "SGIdle", cc_SGIdle },
    { "SGInitChannel", cc_SGInitChannel },
    { "SGInitialize", cc_SGInitialize },
    { "SGNewChannel", cc_SGNewChannel },
    { "SGNewChannelFromComponent", cc_SGNewChannelFromComponent },
    { "SGNewOutput", cc_SGNewOutput },
    { "SGPanelCanRun", cc_SGPanelCanRun },
    { "SGPanelEvent", cc_SGPanelEvent },
    { "SGPanelGetDitl", cc_SGPanelGetDitl },
    { "SGPanelGetSettings", cc_SGPanelGetSettings },
    { "SGPanelGetTitle", cc_SGPanelGetTitle },
    { "SGPanelInstall", cc_SGPanelInstall },
    { "SGPanelItem", cc_SGPanelItem },
    { "SGPanelRemove", cc_SGPanelRemove },
    { "SGPanelSetEventFilter", cc_SGPanelSetEventFilter },
    { "SGPanelSetGrabber", cc_SGPanelSetGrabber },
    { "SGPanelSetResFile", cc_SGPanelSetResFile },
    { "SGPanelSetSettings", cc_SGPanelSetSettings },
    { "SGPanelValidateInput", cc_SGPanelValidateInput },
    { "SGPause", cc_SGPause },
    { "SGPrepare", cc_SGPrepare },
    { "SGRelease", cc_SGRelease },
    { "SGSetAdditionalSoundRates", cc_SGSetAdditionalSoundRates },
    { "SGSetChannelBounds", cc_SGSetChannelBounds },
    { "SGSetChannelClip", cc_SGSetChannelClip },
    { "SGSetChannelDevice", cc_SGSetChannelDevice },
    { "SGSetChannelMatrix", cc_SGSetChannelMatrix },
    { "SGSetChannelMaxFrames", cc_SGSetChannelMaxFrames },
    { "SGSetChannelOutput", cc_SGSetChannelOutput },
    { "SGSetChannelPlayFlags", cc_SGSetChannelPlayFlags },
    { "SGSetChannelRefCon", cc_SGSetChannelRefCon },
    { "SGSetChannelSettings", cc_SGSetChannelSettings },
    { "SGSetChannelUsage", cc_SGSetChannelUsage },
    { "SGSetChannelVolume", cc_SGSetChannelVolume },
    { "SGSetCompressBuffer", cc_SGSetCompressBuffer },
    { "SGSetDataOutput", cc_SGSetDataOutput },
    { "SGSetDataProc", cc_SGSetDataProc },
    { "SGSetDataRef", cc_SGSetDataRef },
    { "SGSetFlags", cc_SGSetFlags },
    { "SGSetFontName", cc_SGSetFontName },
    { "SGSetFontSize", cc_SGSetFontSize },
    { "SGSetFrameRate", cc_SGSetFrameRate },
    { "SGSetGWorld", cc_SGSetGWorld },
    { "SGSetInstrument", cc_SGSetInstrument },
    { "SGSetJustification", cc_SGSetJustification },
    { "SGSetMaximumRecordTime", cc_SGSetMaximumRecordTime },
    { "SGSetOutputFlags", cc_SGSetOutputFlags },
    { "SGSetOutputMaximumOffset", cc_SGSetOutputMaximumOffset },
    { "SGSetOutputNextOutput", cc_SGSetOutputNextOutput },
    { "SGSetPreferredPacketSize", cc_SGSetPreferredPacketSize },
    { "SGSetSettings", cc_SGSetSettings },
    { "SGSetSoundInputDriver", cc_SGSetSoundInputDriver },
    { "SGSetSoundInputParameters", cc_SGSetSoundInputParameters },
    { "SGSetSoundInputRate", cc_SGSetSoundInputRate },
    { "SGSetSoundRecordChunkSize", cc_SGSetSoundRecordChunkSize },
    { "SGSetTextBackColor", cc_SGSetTextBackColor },
    { "SGSetTextForeColor", cc_SGSetTextForeColor },
    { "SGSetTextReturnToSpaceValue", cc_SGSetTextReturnToSpaceValue },
    { "SGSetUseScreenBuffer", cc_SGSetUseScreenBuffer },
    { "SGSetUserVideoCompressorList", cc_SGSetUserVideoCompressorList },
    { "SGSetVideoBottlenecks", cc_SGSetVideoBottlenecks },
    { "SGSetVideoCompressor", cc_SGSetVideoCompressor },
    { "SGSetVideoCompressorType", cc_SGSetVideoCompressorType },
    { "SGSetVideoDigitizerComponent", cc_SGSetVideoDigitizerComponent },
    { "SGSetVideoRect", cc_SGSetVideoRect },
    { "SGSettingsDialog", cc_SGSettingsDialog },
    { "SGSortDeviceList", cc_SGSortDeviceList },
    { "SGSoundInputDriverChanged", cc_SGSoundInputDriverChanged },
    { "SGStartPreview", cc_SGStartPreview },
    { "SGStartRecord", cc_SGStartRecord },
    { "SGStop", cc_SGStop },
    { "SGTransferFrameForCompress", cc_SGTransferFrameForCompress },
    { "SGUpdate", cc_SGUpdate },
    { "SGVideoDigitizerChanged", cc_SGVideoDigitizerChanged },
    { "SGWriteExtendedMovieData", cc_SGWriteExtendedMovieData },
    { "SGWriteMovieData", cc_SGWriteMovieData },
    { "SGWriteSamples", cc_SGWriteSamples },
    { "VDAddKeyColor", cc_VDAddKeyColor },
    { "VDClearClipRgn", cc_VDClearClipRgn },
    { "VDCompressDone", cc_VDCompressDone },
    { "VDCompressOneFrameAsync", cc_VDCompressOneFrameAsync },
    { "VDDone", cc_VDDone },
    { "VDGetActiveSrcRect", cc_VDGetActiveSrcRect },
    { "VDGetBlackLevelValue", cc_VDGetBlackLevelValue },
    { "VDGetBrightness", cc_VDGetBrightness },
    { "VDGetCLUTInUse", cc_VDGetCLUTInUse },
    { "VDGetClipState", cc_VDGetClipState },
    { "VDGetCompressionTime", cc_VDGetCompressionTime },
    { "VDGetCompressionTypes", cc_VDGetCompressionTypes },
    { "VDGetContrast", cc_VDGetContrast },
    { "VDGetCurrentFlags", cc_VDGetCurrentFlags },
    { "VDGetDMADepths", cc_VDGetDMADepths },
    { "VDGetDataRate", cc_VDGetDataRate },
    { "VDGetDigitizerInfo", cc_VDGetDigitizerInfo },
    { "VDGetDigitizerRect", cc_VDGetDigitizerRect },
    { "VDGetFieldPreference", cc_VDGetFieldPreference },
    { "VDGetHue", cc_VDGetHue },
    { "VDGetImageDescription", cc_VDGetImageDescription },
    { "VDGetInput", cc_VDGetInput },
    { "VDGetInputColorSpaceMode", cc_VDGetInputColorSpaceMode },
    { "VDGetInputFormat", cc_VDGetInputFormat },
    { "VDGetInputGammaRecord", cc_VDGetInputGammaRecord },
    { "VDGetInputGammaValue", cc_VDGetInputGammaValue },
    { "VDGetInputName", cc_VDGetInputName },
    { "VDGetKeyColor", cc_VDGetKeyColor },
    { "VDGetKeyColorRange", cc_VDGetKeyColorRange },
    { "VDGetMaskPixMap", cc_VDGetMaskPixMap },
    { "VDGetMaskandValue", cc_VDGetMaskandValue },
    { "VDGetMaxAuxBuffer", cc_VDGetMaxAuxBuffer },
    { "VDGetMaxSrcRect", cc_VDGetMaxSrcRect },
    { "VDGetNextKeyColor", cc_VDGetNextKeyColor },
    { "VDGetNumberOfInputs", cc_VDGetNumberOfInputs },
    { "VDGetPLLFilterType", cc_VDGetPLLFilterType },
    { "VDGetPlayThruDestination", cc_VDGetPlayThruDestination },
    { "VDGetPreferredImageDimensions", cc_VDGetPreferredImageDimensions },
    { "VDGetPreferredTimeScale", cc_VDGetPreferredTimeScale },
    { "VDGetSaturation", cc_VDGetSaturation },
    { "VDGetSharpness", cc_VDGetSharpness },
    { "VDGetSoundInputDriver", cc_VDGetSoundInputDriver },
    { "VDGetSoundInputSource", cc_VDGetSoundInputSource },
    { "VDGetTimeCode", cc_VDGetTimeCode },
    { "VDGetVBlankRect", cc_VDGetVBlankRect },
    { "VDGetVideoDefaults", cc_VDGetVideoDefaults },
    { "VDGetWhiteLevelValue", cc_VDGetWhiteLevelValue },
    { "VDGrabOneFrame", cc_VDGrabOneFrame },
    { "VDGrabOneFrameAsync", cc_VDGrabOneFrameAsync },
    { "VDPreflightDestination", cc_VDPreflightDestination },
    { "VDPreflightGlobalRect", cc_VDPreflightGlobalRect },
    { "VDReleaseAsyncBuffers", cc_VDReleaseAsyncBuffers },
    { "VDReleaseCompressBuffer", cc_VDReleaseCompressBuffer },
    { "VDResetCompressSequence", cc_VDResetCompressSequence },
    { "VDSetBlackLevelValue", cc_VDSetBlackLevelValue },
    { "VDSetBrightness", cc_VDSetBrightness },
    { "VDSetClipRgn", cc_VDSetClipRgn },
    { "VDSetClipState", cc_VDSetClipState },
    { "VDSetCompression", cc_VDSetCompression },
    { "VDSetCompressionOnOff", cc_VDSetCompressionOnOff },
    { "VDSetContrast", cc_VDSetContrast },
    { "VDSetDataRate", cc_VDSetDataRate },
    { "VDSetDestinationPort", cc_VDSetDestinationPort },
    { "VDSetDigitizerRect", cc_VDSetDigitizerRect },
    { "VDSetDigitizerUserInterrupt", cc_VDSetDigitizerUserInterrupt },
    { "VDSetFieldPreference", cc_VDSetFieldPreference },
    { "VDSetFrameRate", cc_VDSetFrameRate },
    { "VDSetHue", cc_VDSetHue },
    { "VDSetInput", cc_VDSetInput },
    { "VDSetInputColorSpaceMode", cc_VDSetInputColorSpaceMode },
    { "VDSetInputGammaRecord", cc_VDSetInputGammaRecord },
    { "VDSetInputGammaValue", cc_VDSetInputGammaValue },
    { "VDSetInputStandard", cc_VDSetInputStandard },
    { "VDSetKeyColor", cc_VDSetKeyColor },
    { "VDSetKeyColorRange", cc_VDSetKeyColorRange },
    { "VDSetMasterBlendLevel", cc_VDSetMasterBlendLevel },
    { "VDSetPLLFilterType", cc_VDSetPLLFilterType },
    { "VDSetPlayThruDestination", cc_VDSetPlayThruDestination },
    { "VDSetPlayThruGlobalRect", cc_VDSetPlayThruGlobalRect },
    { "VDSetPlayThruOnOff", cc_VDSetPlayThruOnOff },
    { "VDSetPreferredImageDimensions", cc_VDSetPreferredImageDimensions },
    { "VDSetPreferredPacketSize", cc_VDSetPreferredPacketSize },
    { "VDSetSaturation", cc_VDSetSaturation },
    { "VDSetSharpness", cc_VDSetSharpness },
    { "VDSetTimeBase", cc_VDSetTimeBase },
    { "VDSetWhiteLevelValue", cc_VDSetWhiteLevelValue },
    { "VDSetupBuffers", cc_VDSetupBuffers },
    { "VDUseSafeBuffers", cc_VDUseSafeBuffers },
    { "VDUseThisCLUT", cc_VDUseThisCLUT },
};
const unsigned qtcapture_export_count = sizeof qtcapture_exports / sizeof qtcapture_exports[0];
