import CCore
import Foundation
import Testing
import Toolbox

/// A four-character code as the Toolbox calls take it.
private func code(_ s: String) -> Int32 { Int32(bitPattern: fourCC(s)) }

/// A guest AEDesc (8 bytes), null.
private func newDesc() -> UInt32 { tb_NewPtr(8, false, true) }

private func descType(_ d: UInt32) -> UInt32 { rd32(d) }

/// A descriptor's data as bytes.
private func descBytes(_ d: UInt32) -> [UInt8] {
    let h = rd32(d + 4)
    guard h != 0 else { return [] }
    return (0..<UInt32(tb_GetHandleSize(h))).map { rd8(rd32(h) + $0) }
}

private func descText(_ d: UInt32) -> String { String(decoding: descBytes(d), as: UTF8.self) }

/// Guest bytes holding `bytes`.
private func guestBytes(_ bytes: [UInt8]) -> UInt32 {
    let p = tb_NewPtr(Int32(max(bytes.count, 1)), false, true)
    for (i, b) in bytes.enumerated() { wr8(p + UInt32(i), b) }
    return p
}

private func guestLong(_ v: UInt32) -> UInt32 {
    let p = tb_NewPtr(4, false, true)
    wr32(p, v)
    return p
}

/// Pascal routines written in Swift, for handlers and accessors: each is a
/// native stub that pops its arguments (sizes in bytes, 1 for a Boolean),
/// runs the body and returns its OSErr.
nonisolated(unsafe) var pascalBodies: [(sizes: [Int], body: ([UInt32]) -> Int16)] = []
let pascalTrampoline: native_fn = { c, trapword in
    guard let c else { return }
    let entry = pascalBodies[Int(trapword)]
    let ret = cpu_pop32(c)
    var args = [UInt32]()
    for size in entry.sizes.reversed() {
        args.insert(size == 4 ? cpu_pop32(c) : UInt32(size == 1 ? cpu_pop16(c) >> 8 : cpu_pop16(c)), at: 0)
    }
    let r = entry.body(args)
    wr16(c.pointee.a.7, UInt16(bitPattern: r))
    cpu_push32(c, ret)
}

func pascalProc(_ sizes: [Int], _ body: @escaping ([UInt32]) -> Int16) -> UInt32 {
    pascalBodies.append((sizes, body))
    return native_register(pascalTrampoline, "AETestProc", UInt16(pascalBodies.count - 1))
}

/// A guest stack for calls into handlers.
func guestStack() { g_cpu!.pointee.a.7 = tb_NewPtr(65536, false, true) + 65536 - 16 }

extension BlueBox {
    @Suite struct AppleEventDescriptors {
        /// Records keep their shape under any descriptor type, as the object
        /// model's 'obj ' and 'rang' records need.
        @Test func recordsOfAnyType() {
            launchTestApp()
            let rec = newDesc()
            #expect(tb_AECreateList(0, 0, 1, rec) == 0)
            #expect(descType(rec) == fourCC("reco"))
            #expect(tb_AEPutParamPtr(rec, code("want"), code("type"), guestLong(fourCC("cwin")), 4) == 0)
            #expect(tb_AEPutParamPtr(rec, code("form"), code("enum"), guestLong(fourCC("indx")), 4) == 0)
            #expect(tb_AEPutParamPtr(rec, code("seld"), code("long"), guestLong(1), 4) == 0)
            let obj = newDesc()
            #expect(tb_AECoerceDesc(rec, code("obj "), obj) == 0)
            #expect(descType(obj) == fourCC("obj "))
            let count = tb_NewPtr(4, false, true)
            #expect(tb_AECountItems(obj, count) == 0)
            #expect(rd32(count) == 3)
            let out = tb_NewPtr(4, false, true), type = tb_NewPtr(4, false, true), size = tb_NewPtr(4, false, true)
            #expect(tb_AEGetParamPtr(obj, code("form"), code("enum"), type, out, 4, size) == 0)
            #expect(rd32(out) == fourCC("indx"))
            // Replacing a parameter keeps its place.
            #expect(tb_AEPutParamPtr(obj, code("form"), code("enum"), guestLong(fourCC("name")), 4) == 0)
            let key = tb_NewPtr(4, false, true)
            #expect(tb_AEGetNthPtr(obj, 2, code("****"), key, type, out, 4, size) == 0)
            #expect(rd32(key) == fourCC("form") && rd32(out) == fourCC("name"))

            // A record made with AECreateDesc and no data, as some programs do.
            let empty = newDesc()
            #expect(tb_AECreateDesc(code("reco"), 0, 0, empty) == 0)
            #expect(tb_AEPutParamPtr(empty, code("kwd1"), code("long"), guestLong(7), 4) == 0)
            #expect(tb_AECountItems(empty, count) == 0 && rd32(count) == 1)

            // Plain data is neither.
            let text = newDesc()
            #expect(tb_AECreateDesc(code("TEXT"), guestBytes(Array("hi".utf8)), 2, text) == 0)
            #expect(tb_AECountItems(text, count) == -1703) // errAEWrongDataType
            #expect(tb_AEPutParamPtr(text, code("kwd1"), code("long"), guestLong(7), 4) == -1703)
        }

        @Test func standardCoercions() throws {
            let dir = launchTestApp()
            func coerce(_ type: String, _ bytes: [UInt8], to: String) -> (Int16, UInt32, [UInt8]) {
                let d = newDesc()
                let err = tb_AECoercePtr(code(type), guestBytes(bytes), Int32(bytes.count), code(to), d)
                return (err, descType(d), descBytes(d))
            }
            #expect(coerce("long", be32(42), to: "TEXT").2 == Array("42".utf8))
            #expect(coerce("TEXT", Array(" 12".utf8), to: "shor").2 == be16(12))
            #expect(coerce("TEXT", Array("12x".utf8), to: "long").0 == -1700) // errAECoercionFail
            #expect(coerce("TEXT", Array("2.5".utf8), to: "doub").2 == [0x40, 0x04, 0, 0, 0, 0, 0, 0])
            #expect(coerce("doub", [0x40, 0x04, 0, 0, 0, 0, 0, 0], to: "TEXT").2 == Array("2.5".utf8))
            #expect(coerce("enum", Array("true".utf8), to: "bool").2 == [1])
            #expect(coerce("bool", [0], to: "enum").2 == Array("fals".utf8))
            #expect(coerce("type", Array("TEXT".utf8), to: "TEXT").2 == Array("TEXT".utf8))
            #expect(coerce("shor", be16(300), to: "bool").0 == -1700)
            // Anything to a list: a list of it.
            let l = coerce("long", be32(5), to: "list")
            #expect(l.0 == 0 && l.1 == fourCC("list"))

            // FSSpecs and aliases, both ways.
            let file = dir.appendingPathComponent("Letter")
            try Data("x".utf8).write(to: file)
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            fm_spec_for_path(file.path, spec)
            let alias = newDesc()
            #expect(tb_AECoercePtr(code("fss "), spec, Int32(FSSpec_SIZEOF), code("alis"), alias) == 0)
            #expect(descType(alias) == fourCC("alis"))
            let back = newDesc()
            #expect(tb_AECoerceDesc(alias, code("fss "), back) == 0)
            #expect(pstring(at: rd32(rd32(back + 4)) + UInt32(FSSpec_name_off)) == "Letter")

            // Parameters come out coerced.
            let rec = newDesc()
            _ = tb_AECreateList(0, 0, 1, rec)
            _ = tb_AEPutParamPtr(rec, code("----"), code("TEXT"), guestBytes(Array("17".utf8)), 2)
            let out = tb_NewPtr(4, false, true), type = tb_NewPtr(4, false, true), size = tb_NewPtr(4, false, true)
            #expect(tb_AEGetParamPtr(rec, code("----"), code("long"), type, out, 4, size) == 0)
            #expect(rd32(out) == 17 && rd32(type) == fourCC("long") && rd32(size) == 4)
        }

        /// The application's coercion handlers come before the system's, and
        /// both before the built-in coercions.
        @Test func coercionHandlers() {
            launchTestApp()
            guestStack()
            var seen: [String] = []
            // FUNCTION (typeCode, dataPtr, dataSize, toType, refcon, VAR result): OSErr
            let fromPtr = pascalProc([4, 4, 4, 4, 4, 4]) { a in
                seen.append("ptr \(a[2]) bytes refcon \(a[4])")
                _ = tb_AECreateDesc(Int32(bitPattern: a[3]), guestBytes(Array("PT".utf8)), 2, a[5])
                return 0
            }
            // FUNCTION (VAR desc, toType, refcon, VAR result): OSErr
            let fromDesc = pascalProc([4, 4, 4, 4]) { a in
                seen.append("desc \(String(format: "%08X", rd32(a[0])))")
                return -1700
            }
            #expect(tb_AEInstallCoercionHandler(code("TEXT"), code("QDpt"), fromPtr, 9, 0, 0) == 0)
            #expect(tb_AEInstallCoercionHandler(code("****"), code("QDrt"), fromDesc, 0, 1, 1) == 0)
            let d = newDesc()
            #expect(tb_AECoercePtr(code("TEXT"), guestBytes(Array("10,20".utf8)), 5, code("QDpt"), d) == 0)
            #expect(descType(d) == fourCC("QDpt") && descText(d) == "PT")
            #expect(tb_AECoercePtr(code("long"), guestLong(1), 4, code("QDrt"), d) == -1700)
            #expect(seen == ["ptr 5 bytes refcon 9", "desc 6C6F6E67"]) // 'long'
            let h = tb_NewPtr(4, false, true), refcon = tb_NewPtr(4, false, true), isDesc = tb_NewPtr(2, false, true)
            #expect(tb_AEGetCoercionHandler(code("****"), code("QDrt"), h, refcon, isDesc, 1) == 0)
            #expect(rd32(h) == fromDesc && rd8(isDesc) == 1)
            #expect(tb_AEGetCoercionHandler(code("****"), code("QDrt"), h, refcon, isDesc, 0) == -1717)
            #expect(tb_AERemoveCoercionHandler(code("TEXT"), code("QDpt"), fromPtr, 0) == 0)
            #expect(tb_AECoercePtr(code("TEXT"), guestBytes(Array("1".utf8)), 1, code("QDpt"), d) == -1700)
        }

        @Test func arrays() {
            launchTestApp()
            let list = newDesc()
            _ = tb_AECreateList(0, 0, 0, list)
            let longs = guestBytes(be32(3) + be32(1) + be32(4))
            #expect(tb_AEPutArray(list, 0, longs, code("long"), 4, 3) == 0) // kAEDataArray
            let count = tb_NewPtr(4, false, true)
            _ = tb_AECountItems(list, count)
            #expect(rd32(count) == 3)
            let out = tb_NewPtr(16, false, true), type = tb_NewPtr(4, false, true)
            let size = tb_NewPtr(4, false, true), n = tb_NewPtr(4, false, true)
            wr32(type, fourCC("shor"))
            wr32(size, 2)
            #expect(tb_AEGetArray(list, 0, out, 16, type, size, n) == 0)
            #expect(rd32(n) == 3 && rd16(out) == 3 && rd16(out + 2) == 1 && rd16(out + 4) == 4)

            // Descriptors into a record, keyed.
            let rec = newDesc()
            _ = tb_AECreateList(0, 0, 1, rec)
            let pairs = tb_NewPtr(24, false, true)
            wr32(pairs, fourCC("key1"))
            _ = tb_AECreateDesc(code("long"), guestLong(11), 4, pairs + 4)
            wr32(pairs + 12, fourCC("key2"))
            _ = tb_AECreateDesc(code("long"), guestLong(22), 4, pairs + 16)
            #expect(tb_AEPutArray(rec, 4, pairs, 0, 0, 2) == 0) // kAEKeyDescArray
            #expect(tb_AEPutArray(rec, 3, pairs + 4, 0, 0, 1) == -1703) // descriptors without keys
            let got = tb_NewPtr(24, false, true)
            #expect(tb_AEGetArray(rec, 4, got, 24, 0, 0, n) == 0)
            #expect(rd32(n) == 2 && rd32(got + 12) == fourCC("key2"))
            #expect(rd32(rd32(rd32(got + 20))) == 22)
        }

        /// Launch parameters ('appa') carry an Open Documents event to the
        /// application LaunchApplication starts.
        @Test func launchParametersCarryDocuments() throws {
            let dir = launchTestApp()
            let app = dir.appendingPathComponent("Other App"), doc = dir.appendingPathComponent("A Doc")
            try Data().write(to: app)
            try Data("x".utf8).write(to: doc)
            let evt = newDesc()
            _ = tb_AECreateAppleEvent(code("aevt"), code("odoc"), 0, -1, 0, evt)
            let list = newDesc()
            _ = tb_AECreateList(0, 0, 0, list)
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            fm_spec_for_path(doc.path, spec)
            _ = tb_AEPutPtr(list, 0, code("fss "), spec, Int32(FSSpec_SIZEOF))
            _ = tb_AEPutParamDesc(evt, code("----"), list)
            let appa = newDesc()
            #expect(tb_AECoerceDesc(evt, code("appa"), appa) == 0)

            launched = []
            machine_set_launch_handler({ path, docs, n, _ in
                launched.append(([String(cString: path!)] + (0..<Int(n)).map { String(cString: docs![$0]!) })
                    .map { ($0 as NSString).lastPathComponent }.joined(separator: " + "))
                return true
            }, nil)
            defer { machine_set_launch_handler(nil, nil) }
            let pb = tb_NewPtr(Int32(LaunchParamBlockRec_SIZEOF), false, true)
            fm_spec_for_path(app.path, spec)
            wr16(pb + UInt32(LaunchParamBlockRec_launchBlockID_off), 0x4C43)
            wr16(pb + UInt32(LaunchParamBlockRec_launchControlFlags_off), 0x4000) // launchContinue
            wr32(pb + UInt32(LaunchParamBlockRec_launchAppSpec_off), spec)
            wr32(pb + UInt32(LaunchParamBlockRec_launchAppParameters_off), rd32(rd32(appa + 4)))
            #expect(tb_LaunchApplication(pb) == 0)
            #expect(launched == ["Other App + A Doc"])
        }
    }

    @Suite struct AppleEventDispatch {
        func takeEvent() -> UInt32? {
            let e = tb_NewPtr(Int32(EventRecord_SIZEOF), false, true)
            guard tb_GetNextEvent(0x0400, e) != 0, rd16(e) == 23 else { return nil }
            return e
        }

        /// Handlers from the application's table, then the system's when the
        /// application's declines; the predispatch handler first of all.
        @Test func tablesAndPredispatch() {
            launchTestApp()
            guestStack()
            var calls: [String] = []
            let app = pascalProc([4, 4, 4]) { a in calls.append("app \(a[2])"); return -1708 }
            let sys = pascalProc([4, 4, 4]) { a in calls.append("system \(a[2])"); return 0 }
            _ = tb_AEInstallEventHandler(code("test"), code("ping"), app, 1, 0)
            _ = tb_AEInstallEventHandler(code("test"), code("****"), sys, 2, 1)
            let evt = newDesc(), reply = newDesc()
            _ = tb_AECreateAppleEvent(code("test"), code("ping"), 0, -1, 0, evt)
            #expect(tb_AESend(evt, reply, 1, 0, -1, 0, 0) == 0) // kAENoReply
            #expect(calls == ["app 1", "system 2"])
            #expect(descType(reply) == fourCC("null"))

            let pre = pascalProc([4, 4, 4]) { _ in calls.append("pre"); return 0 }
            _ = tb_AEInstallSpecialHandler(code("phac"), pre, 0)
            calls = []
            #expect(tb_AESend(evt, reply, 1, 0, -1, 0, 0) == 0)
            #expect(calls == ["pre"])
        }

        /// A handler's error goes into the reply as keyErrorNumber; a queued
        /// reply arrives later as an Answer event.
        @Test func replies() {
            launchTestApp(sizeFlags: 0x0040)
            guestStack()
            let failing = pascalProc([4, 4, 4]) { a in
                #expect(rd32(a[1]) == fourCC("aevt")) // a reply was asked for
                return -1728
            }
            _ = tb_AEInstallEventHandler(code("test"), code("fail"), failing, 0, 0)
            let evt = newDesc(), reply = newDesc()
            _ = tb_AECreateAppleEvent(code("test"), code("fail"), 0, 77, 0, evt)
            #expect(tb_AESend(evt, reply, 3, 0, -1, 0, 0) == -1728) // kAEWaitReply
            let out = tb_NewPtr(4, false, true), type = tb_NewPtr(4, false, true), size = tb_NewPtr(4, false, true)
            #expect(tb_AEGetParamPtr(reply, code("errn"), code("long"), type, out, 4, size) == 0)
            #expect(Int32(bitPattern: rd32(out)) == -1728)
            #expect(tb_AEGetAttributePtr(reply, code("evid"), code("type"), type, out, 4, size) == 0)
            #expect(rd32(out) == fourCC("ansr"))
            #expect(tb_AEGetAttributePtr(reply, code("rtid"), code("shor"), type, out, 2, size) == 0)
            #expect(rd16(out) == 77)

            var answered = 0
            let answer = pascalProc([4, 4, 4]) { a in
                let n = tb_NewPtr(4, false, true)
                if tb_AEGetParamPtr(a[0], code("errn"), code("long"), 0, n, 4, 0) == 0 { answered = Int(Int32(bitPattern: rd32(n))) }
                return 0
            }
            _ = tb_AEInstallEventHandler(code("aevt"), code("ansr"), answer, 0, 0)
            #expect(tb_AESend(evt, reply, 2, 0, -1, 0, 0) == -1728) // kAEQueueReply
            let e = takeEvent()
            #expect(e.map { rd32($0 + 2) } == fourCC("aevt") && e.map { rd32($0 + 10) } == fourCC("ansr"))
            if let e { _ = tb_AEProcessAppleEvent(e) }
            #expect(answered == -1728)
        }

        /// The host's Do Script: the text arrives as the direct object, with
        /// Mac line ends, and the reply comes back to the host.
        @Test func doScriptFromTheHost() {
            launchTestApp(sizeFlags: 0x0040)
            guestStack()
            var script = ""
            let dosc = pascalProc([4, 4, 4]) { a in
                let d = newDesc()
                #expect(tb_AEGetParamDesc(a[0], code("----"), code("TEXT"), d) == 0)
                script = descText(d)
                let result = Array("done".utf8)
                #expect(tb_AEPutParamPtr(a[1], code("----"), code("TEXT"), guestBytes(result), 4) == 0)
                return 0
            }
            _ = tb_AEInstallEventHandler(code("misc"), code("dosc"), dosc, 0, 0)
            hostReplies = []
            machine_send_apple_event(fourCC("misc"), fourCC("dosc"), nil, 0, "beep\nbeep", { _, data, len, err in
                hostReplies.append((data.map { Array(UnsafeBufferPointer(start: $0, count: Int(len))) } ?? [], err))
            }, nil)
            guard let e = takeEvent() else { Issue.record("no event"); return }
            #expect(rd32(e + 2) == fourCC("misc") && rd32(e + 10) == fourCC("dosc"))
            #expect(tb_AEProcessAppleEvent(e) == 0)
            #expect(script == "beep\rbeep")
            #expect(hostReplies.count == 1 && hostReplies.first?.1 == 0)
            // The flattened reply holds the result text.
            let flat = hostReplies.first?.0 ?? []
            #expect(flat.count > 12 && String(decoding: flat.suffix(4), as: UTF8.self) == "done")
        }

        /// A suspended event is answered only when resumed.
        @Test func suspendAndResume() {
            launchTestApp(sizeFlags: 0x0040)
            guestStack()
            let kept = newDesc(), keptReply = newDesc()
            let handler = pascalProc([4, 4, 4]) { a in
                gmemcpy(kept, a[0], 8)
                gmemcpy(keptReply, a[1], 8)
                _ = tb_AESuspendTheCurrentEvent(a[0])
                return 0
            }
            _ = tb_AEInstallEventHandler(code("misc"), code("dosc"), handler, 0, 0)
            hostReplies = []
            machine_send_apple_event(fourCC("misc"), fourCC("dosc"), nil, 0, "later", { _, data, len, err in
                hostReplies.append((data.map { Array(UnsafeBufferPointer(start: $0, count: Int(len))) } ?? [], err))
            }, nil)
            guard let e = takeEvent() else { Issue.record("no event"); return }
            #expect(tb_AEProcessAppleEvent(e) == 0)
            #expect(hostReplies.isEmpty)
            let d = newDesc()
            #expect(tb_AEGetParamDesc(kept, code("----"), code("TEXT"), d) == 0) // still there
            #expect(descText(d) == "later")
            _ = tb_AEPutParamPtr(keptReply, code("----"), code("long"), guestLong(5), 4)
            #expect(tb_AEResumeTheCurrentEvent(kept, keptReply, 0, 0) == 0) // kAENoDispatch
            #expect(hostReplies.count == 1 && hostReplies.first?.1 == 0)
            #expect(descType(kept) == fourCC("null")) // disposed of
        }

        /// Whether a handler may talk to the user depends on where the event
        /// came from and the interaction level.
        @Test func interactingWithTheUser() {
            launchTestApp(sizeFlags: 0x0040)
            guestStack()
            var results: [Int16] = []
            let handler = pascalProc([4, 4, 4]) { _ in
                results.append(tb_AEInteractWithUser(-1, 0, 0))
                return 0
            }
            _ = tb_AEInstallEventHandler(code("misc"), code("dosc"), handler, 0, 0)
            let level = tb_NewPtr(2, false, true)
            _ = tb_AEGetInteractionAllowed(level)
            #expect(rd8(level) == 1) // kAEInteractWithLocal
            for allowed: UInt8 in [1, 0] {
                _ = tb_AESetInteractionAllowed(allowed)
                machine_send_apple_event(fourCC("misc"), fourCC("dosc"), nil, 0, "x", nil, nil)
                if let e = takeEvent() { _ = tb_AEProcessAppleEvent(e) }
            }
            // From ourselves, always.
            let evt = newDesc()
            _ = tb_AECreateAppleEvent(code("misc"), code("dosc"), 0, -1, 0, evt)
            _ = tb_AESend(evt, 0, 1, 0, -1, 0, 0)
            #expect(results == [0, -1713, 0]) // errAENoUserInteraction from the host at kAEInteractWithSelf
        }
    }
}

private nonisolated(unsafe) var launched: [String] = []
private nonisolated(unsafe) var hostReplies: [([UInt8], Int16)] = []
