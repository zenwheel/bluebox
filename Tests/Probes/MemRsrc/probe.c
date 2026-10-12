/*
 * MemRsrc probe: Memory Manager and Resource Manager behaviour.
 *
 * Runs on real System 7.x (the oracle) and in Classic; the two outputs are
 * diffed. Each result is one line of "name value..." with no addresses or
 * other run-dependent values, so the files compare byte for byte.
 *
 * Output: "MemRsrc.out" (TEXT/ttxt, CR line ends) in the folder holding the
 * application. If a file named "AutoShutDown" is in that folder, the probe
 * calls ShutDwnPower when it is done; Basilisk II then quits.
 *
 * The probe uses only the Memory, Resource, Segment Loader and File Managers
 * (no InitGraf), so it runs under Classic's M0 trap set.
 */
#include <Files.h>
#include <Memory.h>
#include <OSUtils.h>
#include <Resources.h>
#include <SegLoad.h>
#include <Multiverse.h> /* ShutDwnPower has no per-manager header */
#include <TextUtils.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define PROBE_TYPE 0x50524273 /* 'PRBs' */

static char out[8192];
static size_t outLen;

static void line(const char *fmt, ...)
{
	va_list ap;
	int n;

	if (outLen >= sizeof out - 1)
		return;
	va_start(ap, fmt);
	n = vsnprintf(out + outLen, sizeof out - outLen - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	outLen += (size_t)n < sizeof out - outLen - 1 ? (size_t)n : sizeof out - outLen - 2;
	out[outLen++] = '\r';
}

/* Pascal string to C string (static buffer, one at a time). */
static const char *pstr(const unsigned char *p)
{
	static char buf[256];

	memcpy(buf, p + 1, p[0]);
	buf[p[0]] = '\0';
	return buf;
}

static const char *ostype(ResType t)
{
	static char buf[5];

	buf[0] = (char)(t >> 24);
	buf[1] = (char)(t >> 16);
	buf[2] = (char)(t >> 8);
	buf[3] = (char)t;
	buf[4] = '\0';
	return buf;
}

/*
 * Every trap result and the error it leaves in MemErr/ResErr are captured
 * into locals in program order before formatting: C leaves the evaluation
 * order of function arguments unspecified, and most of these calls change
 * MemErr themselves.
 */
#define STATE(h) ((unsigned)(unsigned char)HGetState(h))

static void probeMemory(void)
{
	Handle h, h2, h3;
	Ptr p, p2;
	long sz;
	short err, e2;
	unsigned st;
	int ok;

	line("== memory");

	h = NewHandle(100);
	err = MemError();
	line("NewHandle(100) nil=%d MemErr=%d", h == NULL, err);
	sz = GetHandleSize(h);
	err = MemError();
	line("GetHandleSize %ld MemErr=%d", sz, err);
	st = STATE(h);
	err = MemError();
	line("HGetState %02x MemErr=%d", st, err);

	SetHandleSize(h, 200);
	err = MemError();
	sz = GetHandleSize(h);
	line("SetHandleSize(200) MemErr=%d size=%ld", err, sz);
	SetHandleSize(h, 10);
	err = MemError();
	sz = GetHandleSize(h);
	line("SetHandleSize(10) MemErr=%d size=%ld", err, sz);
	SetHandleSize(h, 0);
	err = MemError();
	sz = GetHandleSize(h);
	line("SetHandleSize(0) MemErr=%d size=%ld", err, sz);
	SetHandleSize(h, 0x7FFFFFF0L);
	err = MemError();
	sz = GetHandleSize(h);
	line("SetHandleSize(huge) MemErr=%d size=%ld", err, sz);
	SetHandleSize(h, 64);

	HLock(h);
	err = MemError();
	st = STATE(h);
	line("HLock MemErr=%d HGetState %02x", err, st);
	HUnlock(h);
	err = MemError();
	st = STATE(h);
	line("HUnlock MemErr=%d HGetState %02x", err, st);
	HPurge(h);
	err = MemError();
	st = STATE(h);
	line("HPurge MemErr=%d HGetState %02x", err, st);
	HNoPurge(h);
	HSetRBit(h);
	err = MemError();
	st = STATE(h);
	line("HSetRBit MemErr=%d HGetState %02x", err, st);
	HClrRBit(h);
	HSetState(h, (SignedByte)0xE0);
	err = MemError();
	st = STATE(h);
	line("HSetState(e0) MemErr=%d HGetState %02x", err, st);
	HSetState(h, 0);
	st = STATE(h);
	line("HSetState(00) HGetState %02x", st);

	/* Contents survive a resize, and RecoverHandle finds the master pointer. */
	memset(*h, 0x5A, 64);
	SetHandleSize(h, 4000);
	err = MemError();
	line("SetHandleSize(4000) MemErr=%d keeps data %d", err, (*h)[0] == 0x5A && (*h)[63] == 0x5A);
	ok = RecoverHandle(*h) == h;
	err = MemError();
	line("RecoverHandle same %d MemErr=%d", ok, err);

	h2 = h;
	err = HandToHand(&h2);
	sz = GetHandleSize(h2);
	line("HandToHand err=%d size=%ld distinct %d", err, sz, h2 != h);
	DisposeHandle(h2);

	EmptyHandle(h);
	err = MemError();
	line("EmptyHandle MemErr=%d master nil %d", err, *h == NULL);
	sz = GetHandleSize(h);
	err = MemError();
	line("GetHandleSize(empty) %ld MemErr=%d", sz, err);
	st = STATE(h);
	line("HGetState(empty) %02x", st);
	ReallocateHandle(h, 32);
	err = MemError();
	sz = GetHandleSize(h);
	line("ReallocateHandle(32) MemErr=%d size=%ld", err, sz);
	DisposeHandle(h);
	err = MemError();
	line("DisposeHandle MemErr=%d", err);

	h3 = NewHandleClear(16);
	err = MemError();
	line("NewHandleClear(16) MemErr=%d zero %d", err, h3 && (*h3)[0] == 0 && (*h3)[15] == 0);
	DisposeHandle(h3);

	h3 = NewHandle(0);
	err = MemError();
	sz = GetHandleSize(h3);
	line("NewHandle(0) nil=%d MemErr=%d size=%ld", h3 == NULL, err, sz);
	DisposeHandle(h3);

	h3 = NewHandle(0x7FFFFFF0L);
	err = MemError();
	line("NewHandle(huge) nil=%d MemErr=%d", h3 == NULL, err);

	p = NewPtr(64);
	err = MemError();
	line("NewPtr(64) nil=%d MemErr=%d even %d", p == NULL, err, ((unsigned long)p & 1) == 0);
	sz = GetPtrSize(p);
	err = MemError();
	line("GetPtrSize %ld MemErr=%d", sz, err);
	SetPtrSize(p, 32);
	err = MemError();
	sz = GetPtrSize(p);
	line("SetPtrSize(32) MemErr=%d size=%ld", err, sz);
	DisposePtr(p);
	err = MemError();
	line("DisposePtr MemErr=%d", err);

	p2 = NewPtrClear(10);
	err = MemError();
	sz = GetPtrSize(p2);
	line("NewPtrClear(10) MemErr=%d size=%ld zero %d", err, sz, p2 && p2[0] == 0 && p2[9] == 0);
	h3 = NULL;
	e2 = PtrToHand(p2, &h3, 10);
	sz = GetHandleSize(h3);
	line("PtrToHand err=%d size=%ld", e2, sz);
	DisposeHandle(h3);
	DisposePtr(p2);

	p2 = NewPtr(0x7FFFFFF0L);
	err = MemError();
	line("NewPtr(huge) nil=%d MemErr=%d", p2 == NULL, err);
}

static void probeResources(short appRef)
{
	Handle h, h2;
	short id, n, i, err;
	short attrs, home;
	long sz;
	ResType type;
	Str255 name;
	StringHandle sh;

	line("== resources");
	line("CurResFile is app %d", CurResFile() == appRef);

	n = Count1Resources(PROBE_TYPE);
	err = ResError();
	line("Count1Resources(PRBs) %d ResError=%d", n, err);
	n = Count1Resources(0x51515151); /* 'QQQQ' */
	err = ResError();
	line("Count1Resources(QQQQ) %d ResError=%d", n, err);

	h = Get1Resource(PROBE_TYPE, 128);
	err = ResError();
	line("Get1Resource(PRBs,128) nil=%d ResError=%d", h == NULL, err);
	if (h != NULL) {
		sz = GetHandleSize(h);
		line("size %ld bytes %02x %02x %02x %02x", sz,
		    (unsigned char)(*h)[0], (unsigned char)(*h)[1],
		    (unsigned char)(*h)[2], (unsigned char)(*h)[3]);
		GetResInfo(h, &id, &type, name);
		err = ResError();
		line("GetResInfo id=%d type=%s name=\"%s\" ResError=%d", id, ostype(type), pstr(name), err);
		attrs = GetResAttrs(h);
		err = ResError();
		line("GetResAttrs %02x ResError=%d", (unsigned)(attrs & 0xFF), err);
		home = HomeResFile(h);
		err = ResError();
		line("HomeResFile is app %d ResError=%d", home == appRef, err);
		sz = GetResourceSizeOnDisk(h);
		err = ResError();
		line("GetResourceSizeOnDisk %ld ResError=%d", sz, err);
		line("HGetState %02x", STATE(h));
		h2 = Get1Resource(PROBE_TYPE, 128);
		line("Get1Resource again same %d", h2 == h);
	}

	h = Get1Resource(PROBE_TYPE, 129);
	if (h != NULL) {
		GetResInfo(h, &id, &type, name);
		err = ResError();
		sz = GetHandleSize(h);
		attrs = GetResAttrs(h);
		line("Get1Resource(129) id=%d name=\"%s\" size=%ld attrs=%02x ResError=%d", id, pstr(name),
		    sz, (unsigned)(attrs & 0xFF), err);
		line("HGetState %02x", STATE(h));
	}

	h = Get1NamedResource(PROBE_TYPE, (ConstStringPtr) "\pSecond");
	err = ResError();
	line("Get1NamedResource(Second) nil=%d ResError=%d", h == NULL, err);
	if (h != NULL) {
		GetResInfo(h, &id, &type, name);
		line("  id=%d", id);
	}

	h = Get1Resource(PROBE_TYPE, 999);
	err = ResError();
	line("Get1Resource(PRBs,999) nil=%d ResError=%d", h == NULL, err);
	h = GetResource(0x51515151, 1);
	err = ResError();
	line("GetResource(QQQQ,1) nil=%d ResError=%d", h == NULL, err);

	/* Before the Get1IndResource loop below, which loads every PRBs. */
	SetResLoad(false);
	h = Get1Resource(PROBE_TYPE, 131);
	err = ResError();
	SetResLoad(true);
	line("SetResLoad(false) Get1Resource nil=%d master nil=%d ResError=%d",
	    h == NULL, h ? *h == NULL : -1, err);
	if (h != NULL) {
		LoadResource(h);
		err = ResError();
		sz = GetHandleSize(h);
		line("LoadResource ResError=%d size=%ld byte %02x", err, sz,
		    *h ? (unsigned char)(*h)[0] : 0);
	}

	n = Count1Resources(PROBE_TYPE);
	for (i = 1; i <= n; i++) {
		h = Get1IndResource(PROBE_TYPE, i);
		err = ResError();
		if (h == NULL) {
			line("Get1IndResource(%d) nil ResError=%d", i, err);
			continue;
		}
		GetResInfo(h, &id, &type, name);
		line("Get1IndResource(%d) id=%d name=\"%s\" ResError=%d", i, id, pstr(name), err);
	}
	h = Get1IndResource(PROBE_TYPE, 99);
	err = ResError();
	line("Get1IndResource(99) nil=%d ResError=%d", h == NULL, err);

	sh = GetString(128);
	err = ResError();
	line("GetString(128) nil=%d \"%s\" ResError=%d", sh == NULL, sh ? pstr(*sh) : "", err);

	h = Get1Resource(PROBE_TYPE, 130);
	if (h != NULL) {
		ReleaseResource(h);
		err = ResError();
		line("ReleaseResource ResError=%d", err);
		h = Get1Resource(PROBE_TYPE, 130);
		err = ResError();
		sz = h ? GetHandleSize(h) : -1L;
		line("reload after release nil=%d size=%ld ResError=%d", h == NULL, sz, err);
	}

	UseResFile(appRef);
	err = ResError();
	line("UseResFile ResError=%d", err);
	UseResFile(-12345);
	err = ResError();
	line("UseResFile(bad) ResError=%d", err);
	UseResFile(appRef);
}

/* Write the buffer to "MemRsrc.out" beside the application. */
static void writeOutput(short vRef, long dirID)
{
	HParamBlockRec pb;
	ParamBlockRec io;
	static const unsigned char fname[] = "\pMemRsrc.out";
	short ref;

	memset(&pb, 0, sizeof pb);
	pb.fileParam.ioNamePtr = (StringPtr)fname;
	pb.fileParam.ioVRefNum = vRef;
	pb.fileParam.ioDirID = dirID;
	PBHDeleteSync(&pb);

	memset(&pb, 0, sizeof pb);
	pb.fileParam.ioNamePtr = (StringPtr)fname;
	pb.fileParam.ioVRefNum = vRef;
	pb.fileParam.ioDirID = dirID;
	if (PBHCreateSync(&pb) != noErr)
		return;

	memset(&pb, 0, sizeof pb);
	pb.fileParam.ioNamePtr = (StringPtr)fname;
	pb.fileParam.ioVRefNum = vRef;
	pb.fileParam.ioDirID = dirID;
	PBHGetFInfoSync(&pb);
	pb.fileParam.ioFlFndrInfo.fdType = 0x54455854;    /* 'TEXT' */
	pb.fileParam.ioFlFndrInfo.fdCreator = 0x74747874; /* 'ttxt' */
	pb.fileParam.ioDirID = dirID;
	PBHSetFInfoSync(&pb);

	memset(&pb, 0, sizeof pb);
	pb.ioParam.ioNamePtr = (StringPtr)fname;
	pb.ioParam.ioVRefNum = vRef;
	pb.fileParam.ioDirID = dirID;
	pb.ioParam.ioPermssn = fsRdWrPerm;
	if (PBHOpenSync(&pb) != noErr)
		return;
	ref = pb.ioParam.ioRefNum;

	memset(&io, 0, sizeof io);
	io.ioParam.ioRefNum = ref;
	io.ioParam.ioBuffer = out;
	io.ioParam.ioReqCount = (long)outLen;
	io.ioParam.ioPosMode = fsFromStart;
	PBWriteSync(&io);

	memset(&io, 0, sizeof io);
	io.ioParam.ioRefNum = ref;
	PBCloseSync(&io);

	memset(&io, 0, sizeof io);
	io.ioParam.ioVRefNum = vRef;
	PBFlushVolSync(&io);
}

static int fileExists(short vRef, long dirID, const unsigned char *name)
{
	HParamBlockRec pb;

	memset(&pb, 0, sizeof pb);
	pb.fileParam.ioNamePtr = (StringPtr)name;
	pb.fileParam.ioVRefNum = vRef;
	pb.fileParam.ioDirID = dirID;
	return PBHGetFInfoSync(&pb) == noErr;
}

int main(void)
{
	Str255 appName;
	short appRef;
	Handle appParms;
	FCBPBRec fcb;
	short vRef = 0;
	long dirID = 0;

	/* Where are we? The FCB of our own resource fork gives volume and folder. */
	GetAppParms(appName, &appRef, &appParms);
	memset(&fcb, 0, sizeof fcb);
	fcb.ioRefNum = appRef;
	fcb.ioFCBIndx = 0;
	if (PBGetFCBInfoSync(&fcb) == noErr) {
		vRef = fcb.ioFCBVRefNum;
		dirID = fcb.ioFCBParID;
	}

	line("MemRsrc probe v1");
	probeMemory();
	probeResources(appRef);
	line("== end");

	writeOutput(vRef, dirID);

	if (fileExists(vRef, dirID, (const unsigned char *)"\pAutoShutDown"))
		ShutDwnPower();
	return 0;
}
