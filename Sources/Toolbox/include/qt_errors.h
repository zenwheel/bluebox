/* QuickTime's error codes (MacErrors.h). */
#ifndef QT_ERRORS_H
#define QT_ERRORS_H

enum {
    couldNotResolveDataRef = -2000, badImageDescription = -2001, badPublicMovieAtom = -2002, cantFindHandler = -2003,
    cantOpenHandler = -2004, badComponentType = -2005, noMediaHandler = -2006, noDataHandler = -2007,
    invalidMedia = -2008, invalidTrack = -2009, invalidMovie = -2010, invalidSampleTable = -2011,
    invalidDataRef = -2012, invalidHandler = -2013, invalidDuration = -2014, invalidTime = -2015,
    cantPutPublicMovieAtom = -2016, badEditList = -2017, mediaTypesDontMatch = -2018, progressProcAborted = -2019,
    movieToolboxUninitialized = -2020, cantCreateSingleForkFile = -2022, invalidEditState = -2023,
    nonMatchingEditState = -2024, staleEditState = -2025, userDataItemNotFound = -2026,
    maxSizeToGrowTooSmall = -2027, badTrackIndex = -2028, trackIDNotFound = -2029, trackNotInMovie = -2030,
    timeNotInTrack = -2031, timeNotInMedia = -2032, badEditIndex = -2033, internalQuickTimeError = -2034,
    cantEnableTrack = -2035, invalidRect = -2036, invalidSampleNum = -2037, invalidChunkNum = -2038,
    invalidSampleDescIndex = -2039, invalidSampleDescription = -2041, dataNotOpenForRead = -2042,
    dataNotOpenForWrite = -2043, dataAlreadyOpenForWrite = -2044, dataAlreadyClosed = -2045,
    endOfDataReached = -2046, dataNoDataRef = -2047, noMovieFound = -2048, invalidDataRefContainer = -2049,
    badDataRefIndex = -2050, noDefaultDataRef = -2051, couldNotUseAnExistingSample = -2052,
    featureUnsupported = -2053, unsupportedAuxiliaryImportData = -2057, auxiliaryExportDataUnavailable = -2058,
    samplesAlreadyInMediaErr = -2059, movieTextNotFoundErr = -2062, missingRequiredParameterErr = -2063,
    invalidSpriteWorldPropertyErr = -2064, invalidSpritePropertyErr = -2065, gWorldsNotSameDepthAndSizeErr = -2066,
    invalidSpriteIndexErr = -2067, invalidImageIndexErr = -2068, invalidSpriteIDErr = -2069,
    internalComponentErr = -2070, notImplementedMusicOSErr = -2071, cantSendToSynthesizerOSErr = -2072,
    illegalPartOSErr = -2075, illegalInstrumentOSErr = -2079, noExportProcAvailableErr = -2089,
    videoOutputInUseErr = -2090,
    cannotFindAtomErr = -2101, notLeafAtomErr = -2102, atomsNotOfSameTypeErr = -2103, atomIndexInvalidErr = -2104,
    duplicateAtomTypeAndIDErr = -2105, invalidAtomErr = -2106, invalidAtomContainerErr = -2107,
    invalidAtomTypeErr = -2108, cannotBeLeafAtomErr = -2109,
    codecErr = -8960, noCodecErr = -8961, codecUnimpErr = -8962, codecSizeErr = -8963, codecScreenBufErr = -8964,
    codecImageBufErr = -8965, codecSpoolErr = -8966, codecAbortErr = -8967, codecWouldOffscreenErr = -8968,
    codecBadDataErr = -8969, codecDataVersErr = -8970, codecExtensionNotFoundErr = -8971,
    codecConditionErr = -8972, codecOpenErr = -8973, codecCantWhenErr = -8974, codecCantQueueErr = -8975,
    codecNothingToBlitErr = -8976, codecDisabledErr = -8978, codecNeedAccessKeyErr = -8987,
    codecDroppedFrameErr = -8989,
};

#endif
