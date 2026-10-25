import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

/// What the handlers below saw, in order.
nonisolated(unsafe) var ceLog: [String] = []
nonisolated(unsafe) var ceResults: [UInt32: UInt32] = [:] // a handler's result, by its user data

/// Call an export from inside a native routine (c is g_ppc there): the
/// arguments in r3 on, r3 after.
@discardableResult
func ceCallFrom(_ c: UnsafeMutablePointer<ppc_cpu>, _ name: String, _ args: [UInt32]) -> UInt32 {
    let lr = c.pointee.lr
    withUnsafeMutableBytes(of: &c.pointee.gpr) { gpr in
        for (i, a) in args.enumerated() { gpr.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
    }
    cfm_export_fn(name)!(c)
    c.pointee.lr = lr
    return c.pointee.gpr.3
}

func ceArg(_ c: UnsafeMutablePointer<ppc_cpu>, _ n: Int) -> UInt32 {
    withUnsafeBytes(of: c.pointee.gpr) { $0.load(fromByteOffset: (3 + n) * 4, as: UInt32.self) }
}

func ceReturn(_ c: UnsafeMutablePointer<ppc_cpu>, _ v: UInt32) {
    c.pointee.gpr.3 = v
    c.pointee.pc = c.pointee.lr
}

/// An event handler: logs "<user data as a letter>" and gives the result
/// set for it (eventNotHandledErr unless set).
let ceHandler: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c else { return }
    let data = ceArg(c, 2)
    ceLog.append(String(UnicodeScalar(UInt8(data))))
    ceReturn(c, ceResults[data] ?? UInt32(bitPattern: -9874))
}

/// A handler that passes the event on first (CallNextEventHandler), logs
/// "next" and gives what the rest of the chain did.
let ceNextHandler: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c else { return }
    let call = ceArg(c, 0), event = ceArg(c, 1)
    ceLog.append("next")
    ceReturn(c, ceCallFrom(c, "CallNextEventHandler", [call, event]))
}

/// A window drawing handler: logs "draw".
let ceDrawHandler: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c else { return }
    ceLog.append("draw")
    ceReturn(c, 0)
}

/// A command handler: logs the command's ID and its menu item.
let ceCommandHandler: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c else { return }
    let event = ceArg(c, 1)
    let buf = tb_NewPtr(16, false, true)
    let err = ceCallFrom(c, "GetEventParameter", [event, fourCC("----"), fourCC("hcmd"), 0, 14, 0, buf])
    ceLog.append(err == 0 ? "cmd \(String(format: "%08X", rd32(buf + 4))) item \(rd16(buf + 12))" : "cmd error")
    tb_DisposePtr(buf)
    ceReturn(c, 0)
}

/// A timer: counts its firings in ceResults[0]; at the third it quits the
/// application's event loop.
let ceTimer: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c else { return }
    ceResults[0, default: 0] += 1
    if ceResults[0] == 3 {
        ceCallFrom(c, "QuitApplicationEventLoop", [])
    }
    c.pointee.pc = c.pointee.lr
}

extension BlueBox {
    /// CarbonLib's Carbon Event Manager: events and their parameters,
    /// targets and handler chains, the queue, timers and the event loop.
    @Suite(.serialized) struct CarbonEvents {
        init() {
            bootMachine()
            cfm_reset() // native routines' transition vectors made afresh in this machine
            _ = cfm_ppc_cpu()
            g_ppc_mode = true // handlers are PowerPC routines
            ceLog = []
            ceResults = [:]
        }

        /// Call an export: integer arguments in r3 on, floating point in f1 on.
        @discardableResult
        func call(_ name: String, _ args: [UInt32] = [], fp: [Double] = []) -> UInt32 {
            withUnsafeMutableBytes(of: &g_ppc.gpr) { gpr in
                for (i, a) in args.enumerated() { gpr.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
            }
            for (i, f) in fp.enumerated() { ppc_set_fpr(&g_ppc, Int32(1 + i), f) }
            guard let fn = cfm_export_fn(name) else {
                Issue.record("\(name) is not exported")
                return 0xFFFF_FFFF
            }
            fn(&g_ppc)
            return g_ppc.gpr.3
        }

        func err(_ name: String, _ args: [UInt32] = [], fp: [Double] = []) -> Int32 {
            Int32(bitPattern: call(name, args, fp: fp))
        }

        func ptr(_ words: [UInt32]) -> UInt32 {
            let p = tb_NewPtr(Int32(max(words.count, 1) * 4), false, true)
            for (i, w) in words.enumerated() { wr32(p + UInt32(4 * i), w) }
            return p
        }

        func types(_ list: [(String, UInt32)]) -> UInt32 { ptr(list.flatMap { [fourCC($0.0), $0.1] }) }

        func newEvent(_ cls: String, _ kind: UInt32) -> UInt32 {
            let out = ptr([0])
            #expect(err("CreateEvent", [0, fourCC(cls), kind, 0, 0, 0, out], fp: [0]) == 0)
            return rd32(out)
        }

        func handler(_ fn: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void, _ name: String) -> UInt32 {
            cfm_native_tvector(fn, name)
        }

        func install(_ target: UInt32, _ fn: UInt32, _ list: [(String, UInt32)], data: UInt32) -> UInt32 {
            let out = ptr([0])
            #expect(err("InstallEventHandler", [target, fn, UInt32(list.count), types(list), data, out]) == 0)
            return rd32(out)
        }

        func window() -> UInt32 {
            _ = initToolbox()
            let r = ptr([0x0032_0032, 0x0096_00C8]) // 50,50,150,200
            let title = tb_NewPtr(8, false, true)
            guestPString("W", at: title)
            return tb_NewCWindow(0, r, title, 1, 0, 0xFFFF_FFFF, 1, 0)
        }

        /// An event's class, kind, time, retain count and parameters, with
        /// numbers coerced between integer types and missing ones reported.
        @Test func eventsAndParameters() {
            let e = newEvent("wind", 2)
            #expect(call("GetEventClass", [e]) == fourCC("wind"))
            #expect(call("GetEventKind", [e]) == 2)
            #expect(call("GetEventRetainCount", [e]) == 1)
            #expect(call("RetainEvent", [e]) == e && call("GetEventRetainCount", [e]) == 2)
            call("ReleaseEvent", [e])
            #expect(call("GetEventRetainCount", [e]) == 1)
            #expect(err("SetEventTime", [e], fp: [5.25]) == 0)
            call("GetEventTime", [e])
            #expect(ppc_fpr(&g_ppc, 1) == 5.25)

            let value = ptr([0x0000_0100])
            #expect(err("SetEventParameter", [e, fourCC("kmod"), fourCC("magn"), 4, value]) == 0)
            let type = ptr([0]), size = ptr([0]), out = ptr([0])
            #expect(err("GetEventParameter", [e, fourCC("kmod"), fourCC("shor"), type, 2, size, out]) == 0)
            #expect(rd32(type) == fourCC("shor") && rd32(size) == 2 && rd16(out) == 0x100)
            #expect(err("GetEventParameter", [e, fourCC("kmod"), fourCC("****"), type, 2, size, out]) == 0)
            #expect(rd32(type) == fourCC("magn") && rd32(size) == 4 && rd16(out) == 0) // two bytes of four
            #expect(err("GetEventParameter", [e, fourCC("kmod"), fourCC("QDpt"), type, 4, size, out]) == -1700)
            #expect(err("GetEventParameter", [e, fourCC("none"), fourCC("****"), type, 4, size, out]) == -9870)

            let copy = call("CopyEvent", [e])
            #expect(copy != 0 && copy != e && call("GetEventKind", [copy]) == 2)
            #expect(err("GetEventParameter", [copy, fourCC("kmod"), fourCC("magn"), 0, 4, 0, out]) == 0)
            #expect(rd32(out) == 0x100)
            call("ReleaseEvent", [copy])
            #expect(call("GetEventClass", [copy]) == 0) // gone
            call("ReleaseEvent", [e])
        }

        /// A key-down made by hand converts to an EventRecord; Escape is a
        /// user's cancel.
        @Test func keyEventsAsRecords() {
            let e = newEvent("keyb", 1)
            let ch = tb_NewPtr(1, false, true)
            wr8(ch, 0x61)
            call("SetEventParameter", [e, fourCC("kchr"), fourCC("TEXT"), 1, ch])
            call("SetEventParameter", [e, fourCC("kcod"), fourCC("magn"), 4, ptr([0])])
            call("SetEventParameter", [e, fourCC("kmod"), fourCC("magn"), 4, ptr([0x100])])
            let rec = tb_NewPtr(16, false, true)
            #expect(call("ConvertEventRefToEventRecord", [e, rec]) & 0xFF == 1)
            #expect(rd16(rec) == 3 && rd32(rec + 2) == 0x61 && rd16(rec + 14) == 0x100)
            #expect(call("IsEventInMask", [e, 0x0008]) & 0xFF == 1)
            #expect(call("IsEventInMask", [e, 0x0002]) & 0xFF == 0)
            #expect(call("IsUserCancelEventRef", [e]) & 0xFF == 0)
            wr8(ch, 0x1B)
            call("SetEventParameter", [e, fourCC("kchr"), fourCC("TEXT"), 1, ch])
            #expect(call("IsUserCancelEventRef", [e]) & 0xFF == 1)
            let other = newEvent("cust", 1)
            #expect(call("ConvertEventRefToEventRecord", [other, rec]) & 0xFF == 0)
            #expect(rd16(rec) == 0) // a null event
        }

        /// Handlers run newest first, then the parent target's (a window's
        /// application); CallNextEventHandler passes an event on once;
        /// removed handlers and types no longer see it.
        @Test func handlerChains() {
            let w = window()
            let wt = call("GetWindowEventTarget", [w]), app = call("GetApplicationEventTarget")
            #expect(wt != 0 && app != 0 && wt != app)
            #expect(call("GetWindowEventTarget", [w]) == wt) // one per window
            let fn = handler(ceHandler, "ceHandler"), next = handler(ceNextHandler, "ceNextHandler")
            let list: [(String, UInt32)] = [("test", 1)]
            _ = install(wt, fn, list, data: 0x41) // A, not handled
            let b = install(app, fn, list, data: 0x42) // B, handled
            ceResults[0x42] = 0
            let e = newEvent("test", 1)
            #expect(err("SendEventToEventTarget", [e, wt]) == 0)
            #expect(ceLog == ["A", "B"])

            ceLog = []
            let c = install(wt, next, list, data: 0x43)
            #expect(err("SendEventToEventTarget", [e, wt]) == 0)
            #expect(ceLog == ["next", "A", "B"])

            ceLog = []
            #expect(err("RemoveEventHandler", [c]) == 0)
            #expect(err("RemoveEventTypesFromHandler", [b, 1, types(list)]) == 0)
            #expect(err("SendEventToEventTarget", [e, wt]) == -9874)
            #expect(ceLog == ["A"])
            #expect(err("AddEventTypesToHandler", [b, 1, types(list)]) == 0)
            ceLog = []
            #expect(err("SendEventToEventTarget", [e, app]) == 0)
            #expect(ceLog == ["B"]) // straight to the application
            #expect(err("RemoveEventHandler", [c]) == -50) // already gone
        }

        /// The queue: posted once, counted, found, flushed by type; and
        /// ReceiveNextEvent takes posted events and Blue Box's own,
        /// converted, or times out.
        @Test func queueAndReceive() {
            let q = call("GetMainEventQueue")
            #expect(q != 0 && call("GetCurrentEventQueue") == q)
            let e = newEvent("cust", 7)
            #expect(err("PostEventToQueue", [q, e, 1]) == 0)
            #expect(err("PostEventToQueue", [q, e, 1]) == -9860)
            #expect(call("GetNumEventsInQueue", [q]) == 1 && call("IsEventInQueue", [q, e]) & 0xFF == 1)
            let out = ptr([0])
            let custom = types([("cust", 7)])
            #expect(err("ReceiveNextEvent", [1, custom, 0, 0, 1, out], fp: [0]) == 0)
            #expect(rd32(out) == e && call("GetNumEventsInQueue", [q]) == 0)
            call("ReleaseEvent", [e])
            #expect(err("ReceiveNextEvent", [1, custom, 0, 0, 1, out], fp: [0]) == -9875)

            let f = newEvent("cust", 8)
            call("PostEventToQueue", [q, f, 1])
            #expect(err("FlushEventsMatchingListFromQueue", [q, 1, types([("cust", 8)])]) == 0)
            #expect(call("GetNumEventsInQueue", [q]) == 0)

            // A classic key-down, as Carbon's raw key-down.
            #expect(tb_PostEvent(3, 0x0161) == 0) // 'a', key code 1
            #expect(err("ReceiveNextEvent", [1, types([("keyb", 1)]), 0, 0, 1, out], fp: [0]) == 0)
            let k = rd32(out)
            #expect(call("GetEventClass", [k]) == fourCC("keyb") && call("GetEventKind", [k]) == 1)
            let v = ptr([0])
            #expect(err("GetEventParameter", [k, fourCC("kchr"), fourCC("TEXT"), 0, 1, 0, v]) == 0 && rd8(v) == 0x61)
            #expect(err("GetEventParameter", [k, fourCC("kcod"), fourCC("magn"), 0, 4, 0, v]) == 0 && rd32(v) == 1)

            // QuitEventLoop: the next wait ends at once.
            #expect(err("QuitEventLoop", [call("GetMainEventLoop")]) == 0)
            #expect(err("ReceiveNextEvent", [1, custom, 0, 0, 1, out], fp: [-1]) == -9876)
        }

        /// Timers fire while the loop waits, repeating; RunApplicationEventLoop
        /// runs until QuitApplicationEventLoop (from a timer here).
        @Test func timersAndTheApplicationLoop() {
            let proc = handler(ceTimer, "ceTimer")
            let upp = call("NewEventLoopTimerUPP", [proc])
            #expect(upp != 0)
            let out = ptr([0])
            // InstallEventLoopTimer(loop, delay f1, interval f2, proc, data, &timer)
            #expect(err("InstallEventLoopTimer", [call("GetMainEventLoop"), 0, 0, 0, 0, upp, 0, out], fp: [0.001, 0.001]) == 0)
            let timer = rd32(out)
            #expect(timer != 0)
            call("RunApplicationEventLoop")
            #expect(ceResults[0] == 3)
            #expect(err("RemoveEventLoopTimer", [timer]) == 0)
            #expect(err("RemoveEventLoopTimer", [timer]) == -50)
            // One that never fires.
            #expect(err("InstallEventLoopTimer", [call("GetMainEventLoop"), 0, 0, 0, 0, upp, 0, out], fp: [-1, 0]) == 0)
            #expect(err("ReceiveNextEvent", [1, types([("none", 1)]), 0, 0, 1, ptr([0])], fp: [0.01]) == -9875)
            #expect(ceResults[0] == 3)
        }

        /// The dispatcher: an update draws through kEventWindowDrawContent,
        /// a Command key's menu item becomes kEventCommandProcess with the
        /// item's command ID, and the standard handler's close disposes of
        /// the window.
        @Test func dispatcherAndStandardHandlers() {
            let w = window()
            let wt = call("GetWindowEventTarget", [w]), app = call("GetApplicationEventTarget")
            #expect(err("InstallStandardEventHandler", [wt]) == 0)
            _ = install(wt, handler(ceDrawHandler, "ceDrawHandler"), [("wind", 2)], data: 0)
            _ = install(app, handler(ceCommandHandler, "ceCommandHandler"), [("cmds", 1)], data: 0)
            let dispatcher = call("GetEventDispatcherTarget")
            let out = ptr([0])
            tb_SetRectRgn(WindowRecord_updateRgn(w), 50, 50, 200, 150) // the window to draw
            var drew = false
            for _ in 0..<8 where !drew {
                guard err("ReceiveNextEvent", [0, 0, 0, 0, 1, out], fp: [0]) == 0 else { break }
                call("SendEventToEventTarget", [rd32(out), dispatcher])
                call("ReleaseEvent", [rd32(out)])
                drew = ceLog.contains("draw")
            }
            #expect(drew)
            #expect(tb_EmptyRgn(WindowRecord_updateRgn(w)) != 0) // its update bracketed

            // Test/B, command 'beep'.
            let title = tb_NewPtr(8, false, true), items = tb_NewPtr(16, false, true)
            guestPString("Test", at: title)
            guestPString("Beep/B", at: items)
            let menu = tb_NewMenu(128, title)
            tb_AppendMenu(menu, items)
            tb_InsertMenu(menu, 0)
            #expect(tb_SetMenuItemCommandID(menu, 1, fourCC("beep")) == 0)
            let key = newEvent("keyb", 1)
            let ch = tb_NewPtr(1, false, true)
            wr8(ch, 0x62)
            call("SetEventParameter", [key, fourCC("kchr"), fourCC("TEXT"), 1, ch])
            call("SetEventParameter", [key, fourCC("kcod"), fourCC("magn"), 4, ptr([11])])
            call("SetEventParameter", [key, fourCC("kmod"), fourCC("magn"), 4, ptr([0x100])])
            ceLog = []
            #expect(err("SendEventToEventTarget", [key, dispatcher]) == 0)
            #expect(ceLog == ["cmd 62656570 item 1"])

            // ProcessHICommand goes to the user focus, then the application.
            ceLog = []
            let cmd = ptr([0, fourCC("abcd"), 0, 0])
            #expect(err("ProcessHICommand", [cmd]) == 0)
            #expect(ceLog == ["cmd 61626364 item 0"])
            #expect(call("GetUserFocusWindow") == w)

            // Closing: the standard handler disposes of the window.
            let close = newEvent("wind", 72)
            call("SetEventParameter", [close, fourCC("----"), fourCC("wind"), 4, ptr([w])])
            #expect(err("SendEventToEventTarget", [close, wt]) == 0)
            #expect(rd32(UInt32(LM_WindowList)) == 0)
        }

        /// Default and cancel buttons are kept per window; a window must exist.
        @Test func defaultAndCancelButtons() {
            let w = window()
            let out = ptr([0])
            #expect(err("SetWindowCancelButton", [w, 0x1234]) == 0)
            #expect(err("GetWindowCancelButton", [w, out]) == 0 && rd32(out) == 0x1234)
            #expect(err("GetWindowDefaultButton", [w, out]) == 0 && rd32(out) == 0)
            #expect(err("GetWindowDefaultButton", [0x10, out]) == -5600)
            #expect(err("BeginAppModalStateForWindow", [w]) == 0)
            #expect(err("BeginAppModalStateForWindow", [w]) == -5623)
            #expect(err("EndAppModalStateForWindow", [w]) == 0)
            #expect(err("EndAppModalStateForWindow", [w]) == -5622)
        }
    }
}
