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

extension BlueBox {
    /// The Object Support Library on a model of three windows ("One",
    /// "Two", "Three"), each of words: tokens are 'WTok' (window index),
    /// 'WPrp' (a window's name) and 'Word' (window and word index).
    @Suite struct ObjectSupport {
        static let names = ["One", "Two", "Three"]

        func tokenDesc(_ type: String, _ values: [UInt32], _ out: UInt32) {
            let p = guestBytes(values.flatMap { be32($0) })
            _ = tb_AECreateDesc(code(type), p, Int32(values.count * 4), out)
        }

        func longs(_ d: UInt32) -> [UInt32] {
            let b = descBytes(d)
            return stride(from: 0, to: b.count, by: 4).map { i in b[i..<i + 4].reduce(0) { $0 << 8 | UInt32($1) } }
        }

        /// The model's accessors and callbacks; `log` records disposals.
        func install(_ log: @escaping (String) -> Void) {
            guestStack()
            // FUNCTION (desiredClass, container, containerClass, form, selectionData, VAR value, refcon): OSErr
            let windows = pascalProc([4, 4, 4, 4, 4, 4, 4]) { a in
                let form = a[3], seld = a[4]
                if form == fourCC("indx") {
                    let i = rd32(rd32(rd32(seld + 4)))
                    guard i >= 1 && i <= 3 else { return -1728 }
                    self.tokenDesc("WTok", [i], a[5])
                } else if form == fourCC("name") {
                    guard let i = Self.names.firstIndex(of: descText(seld)) else { return -1728 }
                    self.tokenDesc("WTok", [UInt32(i + 1)], a[5])
                } else { return -1708 }
                return 0
            }
            let property = pascalProc([4, 4, 4, 4, 4, 4, 4]) { a in
                self.tokenDesc("WPrp", self.longs(a[1]), a[5])
                return 0
            }
            let words = pascalProc([4, 4, 4, 4, 4, 4, 4]) { a in
                let w = self.longs(a[1])[0], form = a[3], seld = a[4]
                if form == fourCC("indx") {
                    self.tokenDesc("Word", [w, rd32(rd32(rd32(seld + 4)))], a[5])
                } else if form == fourCC("rang") {
                    let star = newDesc(), stop = newDesc()
                    _ = tb_AEGetParamDesc(seld, code("star"), code("****"), star)
                    _ = tb_AEGetParamDesc(seld, code("stop"), code("****"), stop)
                    log("range \(self.longs(star)) \(self.longs(stop))")
                    self.tokenDesc("Word", [w, self.longs(star)[1], self.longs(stop)[1]], a[5])
                } else { return -1708 }
                return 0
            }
            _ = tb_AEInstallObjectAccessor(code("cwin"), code("null"), windows, 0, 0)
            _ = tb_AEInstallObjectAccessor(code("prop"), code("WTok"), property, 0, 0)
            _ = tb_AEInstallObjectAccessor(code("cwrd"), code("WTok"), words, 0, 1) // the system's table works too
            // FUNCTION (comparisonOperator, obj1, obj2, VAR result: Boolean): OSErr
            let compare = pascalProc([4, 4, 4, 4]) { a in
                guard a[0] == fourCC("=   "), rd32(a[1]) == fourCC("WPrp") else { return -1708 }
                wr8(a[3], Self.names[Int(self.longs(a[1])[0]) - 1] == descText(a[2]) ? 1 : 0)
                return 0
            }
            // FUNCTION (desiredType, containerClass, container, VAR result: LONGINT): OSErr
            let count = pascalProc([4, 4, 4, 4]) { a in wr32(a[3], 3); return 0 }
            let dispose = pascalProc([4]) { a in
                log("dispose \(String(decoding: be32(rd32(a[0])), as: UTF8.self))")
                return -1708 // the default disposal does the rest
            }
            #expect(tb_AESetObjectCallbacks(compare, count, dispose, 0, 0, 0, 0) == 0)
        }

        func windowNamed(_ name: String) -> UInt32 {
            let key = newDesc(), spec = newDesc()
            _ = tb_AECreateDesc(code("TEXT"), guestBytes(Array(name.utf8)), Int32(name.utf8.count), key)
            #expect(tb_CreateObjSpecifier(code("cwin"), 0, code("name"), key, 1, spec) == 0)
            return spec
        }

        func word(_ n: UInt32, of container: UInt32) -> UInt32 {
            let key = newDesc(), spec = newDesc()
            #expect(tb_CreateOffsetDescriptor(Int32(n), key) == 0)
            #expect(tb_CreateObjSpecifier(code("cwrd"), container, code("indx"), key, 0, spec) == 0)
            return spec
        }

        @Test func nestedSpecifiers() {
            launchTestApp()
            var log: [String] = []
            install { log.append($0) }
            let spec = word(2, of: windowNamed("Two"))
            #expect(descType(spec) == fourCC("obj "))
            let token = newDesc()
            #expect(tb_AEResolve(spec, 0, token) == 0)
            #expect(descType(token) == fourCC("Word") && longs(token) == [2, 2])
            #expect(log == ["dispose WTok"]) // the container's token, done with
            log = []
            #expect(tb_AEDisposeToken(token) == 0)
            #expect(log == ["dispose Word"] && descType(token) == fourCC("null"))

            // A null specifier is the application.
            let null = newDesc()
            _ = tb_AECreateDesc(code("null"), 0, 0, null)
            #expect(tb_AEResolve(null, 0, token) == 0 && descType(token) == fourCC("null"))
            // Nothing for the class: no accessor; nothing by that name: no such object.
            let key = newDesc(), bad = newDesc()
            _ = tb_CreateOffsetDescriptor(1, key)
            _ = tb_CreateObjSpecifier(code("cpar"), 0, code("indx"), key, 0, bad)
            #expect(tb_AEResolve(bad, 0, token) == -1723)
            #expect(tb_AEResolve(windowNamed("Four"), 0, token) == -1728)
        }

        /// windows whose name = "Two" or name = "Three": the test runs here,
        /// element by element.
        @Test func whoseClauses() {
            launchTestApp()
            install { _ in }
            func nameIs(_ s: String) -> UInt32 {
                let pnam = newDesc(), exmn = newDesc(), prop = newDesc(), text = newDesc(), cmp = newDesc()
                _ = tb_AECreateDesc(code("type"), guestLong(fourCC("pnam")), 4, pnam)
                _ = tb_AECreateDesc(code("exmn"), 0, 0, exmn)
                _ = tb_CreateObjSpecifier(code("prop"), exmn, code("prop"), pnam, 1, prop)
                _ = tb_AECreateDesc(code("TEXT"), guestBytes(Array(s.utf8)), Int32(s.utf8.count), text)
                #expect(tb_CreateCompDescriptor(code("=   "), prop, text, 1, cmp) == 0)
                return cmp
            }
            let terms = newDesc(), test = newDesc(), spec = newDesc()
            _ = tb_AECreateList(0, 0, 0, terms)
            _ = tb_AEPutDesc(terms, 0, nameIs("Two"))
            _ = tb_AEPutDesc(terms, 0, nameIs("Three"))
            #expect(tb_CreateLogicalDescriptor(terms, code("OR  "), 1, test) == 0)
            #expect(descType(test) == fourCC("logi"))
            #expect(tb_CreateObjSpecifier(code("cwin"), 0, code("test"), test, 1, spec) == 0)
            let token = newDesc()
            #expect(tb_AEResolve(spec, 0, token) == 0)
            #expect(descType(token) == fourCC("list"))
            let count = tb_NewPtr(4, false, true)
            _ = tb_AECountItems(token, count)
            #expect(rd32(count) == 2)
            let item = newDesc()
            _ = tb_AEGetNthDesc(token, 1, code("****"), 0, item)
            #expect(longs(item) == [2])

            // Word 1 of each of them: the accessor applies to every token.
            let words = word(1, of: spec), wtok = newDesc()
            #expect(tb_AEResolve(words, 0, wtok) == 0)
            _ = tb_AEGetNthDesc(wtok, 2, code("****"), 0, item)
            #expect(descType(wtok) == fourCC("list") && longs(item) == [3, 1])
        }

        /// words 2 thru 3 of window 1: the bounds resolve in the window and
        /// the accessor gets their tokens.
        @Test func ranges() {
            launchTestApp()
            var log: [String] = []
            install { log.append($0) }
            let one = newDesc(), win = newDesc(), ccnt = newDesc(), range = newDesc(), spec = newDesc()
            _ = tb_CreateOffsetDescriptor(1, one)
            _ = tb_CreateObjSpecifier(code("cwin"), 0, code("indx"), one, 1, win)
            _ = tb_AECreateDesc(code("ccnt"), 0, 0, ccnt)
            let start = word(2, of: ccnt), stop = word(3, of: ccnt)
            #expect(tb_CreateRangeDescriptor(start, stop, 1, range) == 0)
            #expect(tb_CreateObjSpecifier(code("cwrd"), win, code("rang"), range, 1, spec) == 0)
            let token = newDesc()
            #expect(tb_AEResolve(spec, 0, token) == 0)
            #expect(longs(token) == [1, 2, 3])
            #expect(log.first == "range [1, 2] [1, 3]")
        }

        /// PowerPC programs import the library's calls from ObjectSupportLib.
        @Test func powerPCExports() {
            launchTestApp()
            for name in ["AEObjectInit", "AEResolve", "AEInstallObjectAccessor", "AESetObjectCallbacks",
                         "AEDisposeToken", "AECallObjectAccessor", "CreateObjSpecifier", "CreateOffsetDescriptor",
                         "CreateCompDescriptor", "CreateLogicalDescriptor", "CreateRangeDescriptor"] {
                #expect(cfm_export_fn(name) != nil, "\(name)")
            }
            let key = newDesc(), spec = newDesc()
            _ = tb_CreateOffsetDescriptor(4, key)
            g_ppc.gpr.3 = fourCC("cwin")
            g_ppc.gpr.4 = 0
            g_ppc.gpr.5 = fourCC("indx")
            g_ppc.gpr.6 = key
            g_ppc.gpr.7 = 0
            g_ppc.gpr.8 = spec
            cfm_export_fn("CreateObjSpecifier")!(&g_ppc)
            #expect(g_ppc.gpr.3 == 0 && descType(spec) == fourCC("obj "))
            #expect(tb_AEGetParamPtr(spec, code("seld"), code("long"), 0, key, 4, 0) == 0)
        }
    }
}

extension BlueBox {
    /// Events between processes travel flattened, files as host paths.
    @Suite struct AppleEventsBetweenProcesses {
        func takeEvent() -> UInt32? {
            let e = tb_NewPtr(Int32(EventRecord_SIZEOF), false, true)
            guard tb_GetNextEvent(0x0400, e) != 0, rd16(e) == 23 else { return nil }
            return e
        }

        /// An event flattened in one process arrives whole in another: its
        /// files as FSSpecs and aliases of that process, records nested.
        @Test func flattenedEventsArriveWhole() throws {
            let dir = launchTestApp(sizeFlags: 0x0040)
            guestStack()
            let file = dir.appendingPathComponent("Report")
            try Data("x".utf8).write(to: file)
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            fm_spec_for_path(file.path, spec)
            let evt = newDesc(), list = newDesc(), rec = newDesc(), alias = newDesc()
            _ = tb_AECreateAppleEvent(code("test"), code("flat"), 0, -1, 0, evt)
            _ = tb_AECreateList(0, 0, 0, list)
            _ = tb_AEPutPtr(list, 0, code("fss "), spec, Int32(FSSpec_SIZEOF))
            _ = tb_AECoercePtr(code("fss "), spec, Int32(FSSpec_SIZEOF), code("alis"), alias)
            _ = tb_AEPutDesc(list, 0, alias)
            _ = tb_AEPutParamDesc(evt, code("----"), list)
            _ = tb_AECreateList(0, 0, 1, rec)
            _ = tb_AEPutParamPtr(rec, code("kwd1"), code("long"), guestLong(99), 4)
            _ = tb_AEPutParamDesc(evt, code("reco"), rec)
            var len: UInt32 = 0
            let flat = aem_flatten(evt, &len)
            #expect(flat != nil && len > 0)
            // The FSSpec's numbers mean nothing elsewhere: it travels as its path.
            let bytes = Array(UnsafeBufferPointer(start: flat, count: Int(len)))
            #expect(String(decoding: bytes, as: UTF8.self).contains(file.path))

            var names: [String] = []
            var kept: UInt32 = 0
            let handler = pascalProc([4, 4, 4]) { a in
                let items = newDesc(), item = newDesc(), out = tb_NewPtr(4, false, true)
                _ = tb_AEGetParamDesc(a[0], code("----"), code("list"), items)
                for i: Int32 in 1...2 {
                    _ = tb_AEGetNthDesc(items, i, code("fss "), 0, item) // the alias coerced
                    names.append(pstring(at: rd32(rd32(item + 4)) + UInt32(FSSpec_name_off)))
                }
                let inner = newDesc()
                _ = tb_AEGetParamDesc(a[0], code("reco"), code("reco"), inner)
                _ = tb_AEGetParamPtr(inner, code("kwd1"), code("long"), 0, out, 4, 0)
                kept = rd32(out)
                return 0
            }
            _ = tb_AEInstallEventHandler(code("test"), code("flat"), handler, 0, 0)
            aem_queue_flat(flat, len, nil, nil)
            free(flat)
            guard let e = takeEvent() else { Issue.record("no event"); return }
            #expect(rd32(e + 2) == fourCC("test") && rd32(e + 10) == fourCC("flat"))
            #expect(tb_AEProcessAppleEvent(e) == 0)
            #expect(names == ["Report", "Report"] && kept == 99)
        }

        /// Another process (here a thread, as `bluebox send` would) reaches
        /// a run through its socket and gets the reply.
        @Test func sendingToARun() {
            launchTestApp(sizeFlags: 0x0040)
            guestStack()
            #expect(machine_listen_for_apple_events())
            var procs = [machine_ae_process](repeating: machine_ae_process(), count: 64)
            let n = Int(machine_ae_processes(&procs, 64))
            guard let me = procs.prefix(n).first(where: { $0.pid == getpid() }) else {
                Issue.record("not listed")
                return
            }
            let dosc = pascalProc([4, 4, 4]) { a in
                let d = newDesc()
                _ = tb_AEGetParamDesc(a[0], code("----"), code("TEXT"), d)
                let answer = Array("ran: ".utf8) + descBytes(d)
                _ = tb_AEPutParamPtr(a[1], code("----"), code("TEXT"), guestBytes(answer), Int32(answer.count))
                return 0
            }
            _ = tb_AEInstallEventHandler(code("misc"), code("dosc"), dosc, 0, 0)
            let done = DispatchSemaphore(value: 0)
            nonisolated(unsafe) var result = ""
            nonisolated(unsafe) var err: Int16 = 1
            Thread.detachNewThread {
                var buf = [CChar](repeating: 0, count: 256)
                var errn: Int32 = 0
                err = machine_ae_send_to(me.pid, me.signature, fourCC("misc"), fourCC("dosc"), nil, 0, "go",
                                         true, &buf, buf.count, &errn)
                result = String(cString: buf)
                done.signal()
            }
            var handled = false
            for _ in 0..<200 where !handled {
                if let e = takeEvent() {
                    #expect(tb_AEProcessAppleEvent(e) == 0)
                    handled = true
                } else { usleep(10000) }
            }
            #expect(done.wait(timeout: .now() + 5) == .success)
            #expect(err == 0 && result == "ran: go")

            // The Process Manager lists the other runs after us: here a
            // stand-in (a live process with an entry in the folder).
            let folder = FileManager.default.temporaryDirectory.path
            let ours = (try? FileManager.default.contentsOfDirectory(atPath: folder))?
                .filter { $0.hasPrefix("bluebox-ae-") }
                .map { folder + "/" + $0 }
                .first { ((try? FileManager.default.contentsOfDirectory(atPath: $0)) ?? []).contains { $0.hasPrefix("\(getpid()).") } }
            let sleeper = Process()
            sleeper.executableURL = URL(fileURLWithPath: "/bin/sleep")
            sleeper.arguments = ["10"]
            if let ours, (try? sleeper.run()) != nil {
                let entry = ours + "/\(sleeper.processIdentifier).54455354" // 'TEST'
                FileManager.default.createFile(atPath: entry, contents: nil)
                defer { try? FileManager.default.removeItem(atPath: entry); sleeper.terminate() }
                let psn = tb_NewPtr(8, false, true), info = tb_NewPtr(Int32(ProcessInfoRec_SIZEOF), false, true)
                var signatures: [UInt32] = []
                while tb_GetNextProcess(psn) == 0 {
                    wr32(info + UInt32(ProcessInfoRec_processInfoLength_off), UInt32(ProcessInfoRec_SIZEOF))
                    #expect(tb_GetProcessInformation(psn, info) == 0)
                    signatures.append(rd32(info + UInt32(ProcessInfoRec_processSignature_off)))
                    if rd32(psn) == 1 && rd32(psn + 4) == UInt32(sleeper.processIdentifier) {
                        // It doesn't answer: nothing listens there.
                        let target = newDesc(), evt = newDesc()
                        _ = tb_AECreateDesc(code("psn "), psn, 8, target)
                        _ = tb_AECreateAppleEvent(code("misc"), code("dosc"), target, -1, 0, evt)
                        #expect(tb_AESend(evt, 0, 1, 0, -1, 0, 0) == -600)
                    }
                }
                #expect(signatures.contains(fourCC("TEST")))
            } else {
                Issue.record("no socket folder or no process to stand in")
            }

            // A signature nobody has: no such process.
            let target = newDesc(), evt = newDesc(), reply = newDesc()
            _ = tb_AECreateDesc(code("sign"), guestLong(fourCC("Nobd")), 4, target)
            _ = tb_AECreateAppleEvent(code("misc"), code("dosc"), target, -1, 0, evt)
            #expect(tb_AESend(evt, reply, 3, 0, -1, 0, 0) == -600)
        }
    }
}

private nonisolated(unsafe) var launched: [String] = []
private nonisolated(unsafe) var hostReplies: [([UInt8], Int16)] = []

extension BlueBox {
    /// High-level events other than Apple Events: PostHighLevelEvent,
    /// AcceptHighLevelEvent and GetSpecificHighLevelEvent.
    @Suite struct PostedHighLevelEvents {
        static let receiverIDisPSN: Int32 = 0x8000, receiverIDisSignature: Int32 = 0x7000
        static let receiverIDisSessionID: Int32 = 0x6000, receiverIDisTargetID: Int32 = 0x5000
        static let nReturnReceipt: Int32 = 0x200, nAttnMsg: Int32 = 1

        func takeEvent() -> UInt32? {
            let e = tb_NewPtr(Int32(EventRecord_SIZEOF), false, true)
            guard tb_GetNextEvent(0x0400, e) != 0, rd16(e) == 23 else { return nil }
            return e
        }

        /// An event record for a class and ID, as PostHighLevelEvent takes.
        func hle(_ cls: String, _ id: String) -> UInt32 {
            let e = tb_NewPtr(Int32(EventRecord_SIZEOF), false, true)
            wr16(e, 23)
            wr32(e + 2, fourCC(cls))
            wr32(e + 10, fourCC(id))
            return e
        }

        func ourSignature() -> UInt32 {
            let psn = tb_NewPtr(8, false, true), port = tb_NewPtr(Int32(PPCPortRec_SIZEOF), false, true)
            _ = tb_GetCurrentProcess(psn)
            _ = tb_GetPortNameFromProcessSerialNumber(port, psn)
            return rd32(port + UInt32(PPCPortRec_u_port_creator_off))
        }

        /// A program posts itself an event by serial number; the message is
        /// accepted a piece at a time, and the receipt comes back.
        @Test func postToSelf() {
            launchTestApp()
            let psn = tb_NewPtr(8, false, true)
            wr32(psn + 4, 2) // kCurrentProcess
            let text = Array("hello world".utf8)
            #expect(tb_PostHighLevelEvent(hle("Blue", "Test"), psn, 77, guestBytes(text), Int32(text.count),
                                          Self.receiverIDisPSN | Self.nReturnReceipt) == 0)
            guard let e = takeEvent() else { Issue.record("no event"); return }
            #expect(rd32(e + 2) == fourCC("Blue") && rd32(e + 10) == fourCC("Test"))
            // Not an Apple Event: AEProcessAppleEvent leaves it alone.
            #expect(tb_AEProcessAppleEvent(e) == -1708)

            let sender = tb_NewPtr(Int32(TargetID_SIZEOF), false, true), refcon = tb_NewPtr(4, false, true)
            let buf = tb_NewPtr(32, false, true), len = tb_NewPtr(4, false, true)
            wr32(len, 4)
            #expect(tb_AcceptHighLevelEvent(sender, refcon, buf, len) == -607) // bufferIsSmall
            #expect((0..<4).map { rd8(buf + $0) } == Array("hell".utf8) && rd32(len) == 7)
            wr32(len, 32)
            #expect(tb_AcceptHighLevelEvent(sender, refcon, buf, len) == 0)
            #expect((0..<7).map { rd8(buf + $0) } == Array("o world".utf8) && rd32(len) == 7)
            #expect(rd32(refcon) == 77)
            // From us: our session (the process ID) and our port.
            #expect(rd32(sender) == UInt32(getpid()))
            let port = sender + UInt32(TargetID_name_off)
            #expect(pstring(at: port + UInt32(PPCPortRec_name_off)) == "Test App")
            #expect(rd32(port + UInt32(PPCPortRec_u_port_creator_off)) == ourSignature())
            // Accepted: nothing more outstanding.
            #expect(tb_AcceptHighLevelEvent(sender, refcon, buf, len) == -608) // noOutstandingHLE

            // The receipt: fully accepted, with the original refCon.
            guard let r = takeEvent() else { Issue.record("no receipt"); return }
            #expect(rd32(r + 2) == fourCC("jaym") && rd32(r + 10) == fourCC("rtrn"))
            #expect(rd16(r + 14) == 1) // msgWasFullyAccepted
            wr32(len, 32)
            #expect(tb_AcceptHighLevelEvent(sender, refcon, buf, len) == 0)
            #expect(rd32(refcon) == 77 && rd32(len) == 0)

            // Unaccepted, the receipt says so.
            #expect(tb_PostHighLevelEvent(hle("Blue", "Skip"), psn, 5, 0, 0,
                                          Self.receiverIDisPSN | Self.nReturnReceipt) == 0)
            _ = takeEvent()
            let skipped = takeEvent() // the next event drops the one not accepted
            #expect(skipped.map { rd32($0 + 10) } == fourCC("rtrn") && skipped.map { rd16($0 + 14) } == 0)
        }

        /// The receiver by signature, by session and by target ID; who is
        /// not there.
        @Test func addressing() {
            launchTestApp()
            let sig = ourSignature()
            #expect(tb_PostHighLevelEvent(hle("Blue", "sign"), sig, 1, 0, 0, Self.receiverIDisSignature) == 0)
            #expect(tb_PostHighLevelEvent(hle("Blue", "sess"), UInt32(getpid()), 2, 0, 0,
                                          Self.receiverIDisSessionID) == 0)
            let target = tb_NewPtr(Int32(TargetID_SIZEOF), false, true)
            let port = target + UInt32(TargetID_name_off)
            wr16(port + UInt32(PPCPortRec_portKindsSelector_off), 1) // ppcByCreatorAndType
            wr32(port + UInt32(PPCPortRec_u_port_creator_off), sig)
            #expect(tb_PostHighLevelEvent(hle("Blue", "targ"), target, 3, 0, 0, Self.receiverIDisTargetID) == 0)
            // An urgent one goes first.
            #expect(tb_PostHighLevelEvent(hle("Blue", "attn"), sig, 4, 0, 0,
                                          Self.receiverIDisSignature | Self.nAttnMsg) == 0)
            var ids: [UInt32] = []
            while let e = takeEvent() { ids.append(rd32(e + 10)) }
            #expect(ids == ["attn", "sign", "sess", "targ"].map(fourCC))

            #expect(tb_PostHighLevelEvent(hle("Blue", "none"), fourCC("Nobd"), 0, 0, 0,
                                          Self.receiverIDisSignature) == -600) // procNotFound
            let other = tb_NewPtr(8, false, true)
            wr32(other, 1)
            wr32(other + 4, 1) // launchd: no run of ours
            #expect(tb_PostHighLevelEvent(hle("Blue", "none"), other, 0, 0, 0, Self.receiverIDisPSN) == -600)
            #expect(tb_PostHighLevelEvent(hle("Blue", "none"), 1, 0, 0, 0, Self.receiverIDisSessionID) == -609)
            #expect(tb_PostHighLevelEvent(hle("Blue", "none"), sig, 0, 0, 0, 0) == -50)
        }

        /// The filter looks at each queued event and accepts the one it wants.
        @Test func specificEvents() {
            launchTestApp()
            guestStack()
            let err = tb_NewPtr(2, false, true)
            nonisolated(unsafe) var seen: [UInt32] = []
            let none = pascalProc([4, 4, 4]) { _ in 0 }
            #expect(tb_GetSpecificHighLevelEvent(none, 0, err) == 0)
            #expect(Int16(bitPattern: rd16(err)) == -608)

            let sig = ourSignature()
            for (i, id) in ["one ", "two ", "thre"].enumerated() {
                let msg = Array(id.utf8)
                #expect(tb_PostHighLevelEvent(hle("Blue", id), sig, Int32(i + 1), guestBytes(msg), 4,
                                              Self.receiverIDisSignature) == 0)
            }
            let buf = tb_NewPtr(8, false, true), len = tb_NewPtr(4, false, true)
            let filter = pascalProc([4, 4, 4]) { a in
                let refcon = rd32(a[1] + UInt32(HighLevelEventMsg_userRefCon_off))
                seen.append(refcon)
                #expect(rd32(a[1] + UInt32(HighLevelEventMsg_msgLength_off)) == 4)
                #expect(rd32(a[2]) == UInt32(getpid()))
                guard refcon == a[0] else { return 0 }
                wr32(len, 8)
                _ = tb_AcceptHighLevelEvent(0, 0, buf, len)
                return 0x0100 // true
            }
            #expect(tb_GetSpecificHighLevelEvent(filter, 2, err) == 1)
            #expect(rd16(err) == 0 && seen == [1, 2])
            #expect(rd32(len) == 4 && rd32(buf) == fourCC("two "))
            var ids: [UInt32] = []
            while let e = takeEvent() { ids.append(rd32(e + 10)) }
            #expect(ids == ["one ", "thre"].map(fourCC))
        }

        /// From another run: the flattened form travels through a run's
        /// socket and arrives with its sender.
        @Test func fromAnotherRun() {
            launchTestApp(sizeFlags: 0x0040)
            #expect(machine_listen_for_apple_events())
            let sig = ourSignature()
            var name = [UInt8](repeating: 0, count: 32)
            name[0] = 5
            name.replaceSubrange(1...5, with: Array("Other".utf8))
            let flat = be32(fourCC("BHLE")) + be32(fourCC("Blue")) + be32(fourCC("Remo")) + be32(9) + be32(0)
                + be32(4242) + be32(fourCC("OthR")) + [0, 0, 0, 0] + name + Array("data".utf8)
            let done = DispatchSemaphore(value: 0)
            nonisolated(unsafe) var err: Int16 = 1
            Thread.detachNewThread {
                var reply: UnsafeMutablePointer<UInt8>?
                var rlen: UInt32 = 0
                err = flat.withUnsafeBufferPointer {
                    machine_ae_send(getpid(), sig, $0.baseAddress, UInt32($0.count), 1, 1000, &reply, &rlen)
                }
                done.signal()
            }
            #expect(done.wait(timeout: .now() + 5) == .success)
            #expect(err == 0)
            var got: UInt32?
            for _ in 0..<200 where got == nil {
                if let e = takeEvent() { got = e } else { usleep(10000) }
            }
            guard let e = got else { Issue.record("no event"); return }
            #expect(rd32(e + 2) == fourCC("Blue") && rd32(e + 10) == fourCC("Remo"))
            let sender = tb_NewPtr(Int32(TargetID_SIZEOF), false, true), refcon = tb_NewPtr(4, false, true)
            let buf = tb_NewPtr(8, false, true), len = tb_NewPtr(4, false, true)
            wr32(len, 8)
            #expect(tb_AcceptHighLevelEvent(sender, refcon, buf, len) == 0)
            #expect(rd32(len) == 4 && rd32(buf) == fourCC("data") && rd32(refcon) == 9)
            #expect(rd32(sender) == 4242)
            let port = sender + UInt32(TargetID_name_off)
            #expect(pstring(at: port + UInt32(PPCPortRec_name_off)) == "Other")
            #expect(rd32(port + UInt32(PPCPortRec_u_port_creator_off)) == fourCC("OthR"))
        }

        /// To another run: a stand-in listens at a run's socket (a live
        /// process's ID, the signature 'OthR') and reads what arrives.
        @Test func toAnotherRun() throws {
            launchTestApp(sizeFlags: 0x0040)
            #expect(machine_listen_for_apple_events())
            let folder = FileManager.default.temporaryDirectory.path
            guard let ours = (try? FileManager.default.contentsOfDirectory(atPath: folder))?
                .filter({ $0.hasPrefix("bluebox-ae-") })
                .map({ folder + "/" + $0 })
                .first(where: { ((try? FileManager.default.contentsOfDirectory(atPath: $0)) ?? []).contains { $0.hasPrefix("\(getpid()).") } })
            else { Issue.record("no socket folder"); return }
            let sleeper = Process()
            sleeper.executableURL = URL(fileURLWithPath: "/bin/sleep")
            sleeper.arguments = ["10"]
            try sleeper.run()
            defer { sleeper.terminate() }
            let path = ours + "/\(sleeper.processIdentifier).4F746852" // 'OthR'
            defer { unlink(path) }
            let sock = socket(AF_UNIX, SOCK_STREAM, 0)
            defer { close(sock) }
            var sa = sockaddr_un()
            sa.sun_family = sa_family_t(AF_UNIX)
            withUnsafeMutableBytes(of: &sa.sun_path) { dst in
                _ = path.utf8CString.withUnsafeBytes { src in memcpy(dst.baseAddress, src.baseAddress, src.count) }
            }
            let bound = withUnsafePointer(to: &sa) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(sock, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) }
            }
            #expect(bound == 0 && listen(sock, 1) == 0)
            nonisolated(unsafe) var received: [UInt8] = []
            let done = DispatchSemaphore(value: 0)
            Thread.detachNewThread {
                let fd = accept(sock, nil, nil)
                var buf = [UInt8](repeating: 0, count: 256)
                var n = 0
                while n < buf.count {
                    let k = buf.withUnsafeMutableBytes { read(fd, $0.baseAddress! + n, 256 - n) }
                    if k <= 0 { break }
                    n += k
                }
                received = Array(buf.prefix(n))
                close(fd)
                done.signal()
            }
            let msg = Array("ping".utf8)
            #expect(tb_PostHighLevelEvent(hle("Blue", "Ping"), fourCC("OthR"), 31, guestBytes(msg), 4,
                                          Self.receiverIDisSignature) == 0)
            #expect(done.wait(timeout: .now() + 5) == .success)
            // Framed as Apple Events are (length, mode kAENoReply), then the
            // raw event: class, ID, refCon, the sender, the message.
            #expect(received.count == 8 + 64 + 4)
            guard received.count == 76 else { return }
            let body = Array(received[8...])
            #expect(Array(received[4..<8]) == be32(1))
            #expect(Array(body[0..<16]) == be32(fourCC("BHLE")) + be32(fourCC("Blue")) + be32(fourCC("Ping")) + be32(31))
            #expect(Array(body[20..<28]) == be32(UInt32(getpid())) + be32(ourSignature()))
            #expect(Array(body[64...]) == msg)
        }
    }
}
