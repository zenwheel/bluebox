import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// InputSprocketLib from PowerPC code: needs mapped to the keyboard and a
    /// host game controller, the devices and their elements.
    @Suite struct InputSprocket {
        static let needs: UInt32 = 0x6000_0000, out: UInt32 = 0x6000_1000, buf: UInt32 = 0x6000_2000

        /// Call an export with its arguments in r3 onwards; its result.
        @discardableResult
        func isp(_ name: String, _ args: [UInt32]) -> Int32 {
            for (i, a) in args.enumerated() {
                withUnsafeMutableBytes(of: &g_ppc.gpr) { $0.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
            }
            cfm_export_fn(name)!(&g_ppc)
            return Int32(bitPattern: g_ppc.gpr.3)
        }

        /// ISpNeed records, 92 bytes each.
        func writeNeeds(_ list: [(name: String, kind: String, label: String, player: UInt8)]) {
            for (i, n) in list.enumerated() {
                let at = Self.needs + UInt32(i * 92)
                for k in 0..<92 { wr8(at + UInt32(k), 0) }
                guestPString(n.name, at: at)
                wr8(at + 66, n.player)
                wr32(at + 68, fourCC(n.kind))
                wr32(at + 72, fourCC(n.label))
            }
        }

        func state(_ element: UInt32) -> UInt32 {
            #expect(isp("ISpElement_GetSimpleState", [element, Self.buf]) == 0)
            return rd32(Self.buf)
        }

        func setPad(_ index: Int32, buttons: UInt32 = 0, lx: Float = 0, ly: Float = 0, connected: Bool = true) {
            var pad = machine_gamepad()
            pad.connected = connected
            withUnsafeMutableBytes(of: &pad.name) { $0.copyBytes(from: Array("Test Pad".utf8)) }
            pad.buttons = buttons
            pad.axes.0 = lx
            pad.axes.1 = ly
            machine_set_gamepad(index, &pad)
        }

        @Test func needsFollowKeysAndController() {
            bootMachine()
            inputsprocket_reset()
            machine_release_keys()
            defer { setPad(0, connected: false); machine_release_keys() }
            writeNeeds([("Turn", "axis", "xaxi", 0), ("Fire Lasers", "butn", "fire", 0), ("Jump", "butn", "none", 1),
                        ("Move", "movm", "move", 0)])
            #expect(isp("ISpElement_NewVirtualFromNeeds", [4, Self.needs, Self.out, 0]) == 0)
            let turn = rd32(Self.out), fire = rd32(Self.out + 4), jump = rd32(Self.out + 8), move = rd32(Self.out + 12)
            #expect(state(turn) == 0x7FFF_FFFF && state(fire) == 0 && state(jump) == 0)

            // The keyboard: the right arrow turns, space fires.
            machine_post_key(-1, 0x7C, 0, 1, 0)
            machine_post_key(-1, 0x31, 0, 1, 0)
            #expect(state(turn) == 0xFFFF_FFFF && state(fire) == 1)
            machine_release_keys()

            // A controller: A fires, B jumps for player 1, the stick turns.
            setPad(0, buttons: UInt32(MACHINE_PAD_A | MACHINE_PAD_B), lx: -1)
            #expect(state(turn) == 0 && state(fire) == 1 && state(jump) == 1)
            #expect(isp("ISpElement_GetComplexState", [move, 12, Self.buf]) == 0)
            #expect(rd32(Self.buf) == 0 && rd32(Self.buf + 4) == 0x7FFF_FFFF && rd32(Self.buf + 8) == 1) // left

            // Suspended, everything is idle; resumed, the change is no event.
            isp("ISpSuspend", [])
            #expect(state(fire) == 0 && state(turn) == 0x7FFF_FFFF)
            isp("ISpResume", [])
            #expect(isp("ISpElement_GetNextEvent", [fire, 20, Self.buf, Self.buf + 32]) == 0 && rd8(Self.buf + 32) == 0)
            setPad(0)
            #expect(isp("ISpElement_GetNextEvent", [fire, 20, Self.buf, Self.buf + 32]) == 0 && rd8(Self.buf + 32) == 1)
            #expect(rd32(Self.buf + 8) == fire && rd32(Self.buf + 16) == 0)
        }

        /// Delta needs read the mouse's own motion: low memory left as it was
        /// (a game loop that never calls the Event Manager) and the mouse
        /// pushed on past the screen's edge; with them, fire is its button.
        @Test func deltasFollowTheMouseItself() {
            bootMachine()
            inputsprocket_reset()
            machine_post_mouse(100, 100, 0)
            writeNeeds([("Horizontal", "dlta", "xaxi", 0), ("Vertical", "dlta", "yaxi", 0), ("Fire", "butn", "fire", 0)])
            #expect(isp("ISpElement_NewVirtualFromNeeds", [3, Self.needs, Self.out, 0]) == 0)
            let x = rd32(Self.out), y = rd32(Self.out + 4), fire = rd32(Self.out + 8)
            isp("ISpDevices_ActivateClass", [fourCC("mous")])
            #expect(state(x) == 0 && state(y) == 0)

            machine_post_mouse(110, 95, -1)
            #expect(isp("ISpElement_GetNextEvent", [x, 20, Self.buf, Self.buf + 32]) == 0 && rd8(Self.buf + 32) == 1)
            #expect(Int32(bitPattern: rd32(Self.buf + 16)) == 10 << 16 && state(x) == 0)
            #expect(Int32(bitPattern: state(y)) == 5 << 16) // up is positive

            var w: Int32 = 0, h: Int32 = 0
            machine_screen_size(&w, &h)
            machine_post_mouse(w - 1, 95, -1)
            _ = isp("ISpElement_GetNextEvent", [x, 20, Self.buf, Self.buf + 32])
            machine_post_mouse_delta(40, 0)
            #expect(Int32(bitPattern: state(x)) == 40 << 16)

            machine_post_mouse(w - 1, 95, 1)
            #expect(state(fire) == 1)
            machine_post_mouse(w - 1, 95, 0)
        }

        /// An x axis follows the mouse as a joystick: pushed by its motion
        /// since the last read (32 pixels to the end, more kept for later
        /// reads), back in the middle when it stops; keys held override it.
        /// Only once the game has turned the mouse on.
        @Test func axesFollowTheMouseAsAJoystick() {
            bootMachine()
            inputsprocket_reset()
            machine_release_keys()
            machine_post_mouse(100, 100, 0)
            writeNeeds([("Move", "axis", "xaxi", 0)])
            #expect(isp("ISpElement_NewVirtualFromNeeds", [1, Self.needs, Self.out, 0]) == 0)
            let x = rd32(Self.out)
            machine_post_mouse(116, 100, -1)
            #expect(state(x) == 0x7FFF_FFFF) // not until the game turns the mouse on
            isp("ISpDevices_ActivateClass", [fourCC("mous")])
            machine_post_mouse(100, 100, -1)
            #expect(state(x) == 0x7FFF_FFFF)
            machine_post_mouse(116, 100, -1)
            #expect(state(x) > 0xBFFF_0000 && state(x) == 0x7FFF_FFFF) // half way to the right, then back
            machine_post_mouse(36, 100, -1) // 80 left: two reads at the end, then half way
            #expect(state(x) == 0 && state(x) == 0 && state(x) < 0x4000_0000 && state(x) == 0x7FFF_FFFF)
            machine_post_key(-1, 0x7C, 0, 1, 0)
            #expect(state(x) == 0xFFFF_FFFF)
            machine_release_keys()
            // Reading it has the host capture the mouse.
            var mx: Int32 = 0, my: Int32 = 0, relative = false
            _ = machine_mouse_state(&mx, &my, &relative)
            #expect(relative)
        }

        @Test func devicesAndTheirElements() {
            bootMachine()
            inputsprocket_reset()
            defer { setPad(1, connected: false) }
            #expect(isp("ISpDevices_Extract", [8, Self.out, Self.buf]) == 0)
            #expect(rd32(Self.out) == 2) // the keyboard and the mouse
            setPad(1)
            #expect(isp("ISpDevices_ExtractByClass", [fourCC("gmpd"), 8, Self.out, Self.buf]) == 0 && rd32(Self.out) == 1)
            let pad = rd32(Self.buf)
            #expect(isp("ISpDevice_GetDefinition", [pad, 160, Self.buf]) == 0)
            #expect(pstring(at: Self.buf) == "Test Pad" && rd32(Self.buf + 132) == fourCC("gmpd"))

            // Its elements: A is a button labelled fire, and pressing it is an event on the global list.
            #expect(isp("ISpDevice_GetElementList", [pad, Self.out]) == 0)
            let list = rd32(Self.out)
            #expect(isp("ISpElementList_ExtractByLabel", [list, fourCC("fire"), 4, Self.out, Self.buf]) == 0)
            #expect(rd32(Self.out) == 1)
            let a = rd32(Self.buf)
            #expect(isp("ISpElement_GetInfo", [a, Self.buf]) == 0 && rd32(Self.buf + 4) == fourCC("butn"))
            #expect(pstring(at: Self.buf + 8) == "A")
            #expect(isp("ISpGetGlobalElementList", [Self.out]) == 0)
            let global = rd32(Self.out)
            #expect(isp("ISpElementList_GetNextEvent", [global, 20, Self.buf, Self.buf + 32]) == 0 && rd8(Self.buf + 32) == 0)
            setPad(1, buttons: UInt32(MACHINE_PAD_A))
            #expect(isp("ISpElementList_GetNextEvent", [global, 20, Self.buf, Self.buf + 32]) == 0 && rd8(Self.buf + 32) == 1)
            #expect(rd32(Self.buf + 8) == a && rd32(Self.buf + 16) == 1)

            // Deactivated, it is idle, and says so (Brutal Impact asks).
            #expect(isp("ISpDevice_IsActive", [pad, Self.out]) == 0 && rd8(Self.out) == 1)
            wr32(Self.buf, pad)
            isp("ISpDevices_Deactivate", [1, Self.buf])
            #expect(state(a) == 0)
            #expect(isp("ISpDevice_IsActive", [pad, Self.out]) == 0 && rd8(Self.out) == 0)
        }
    }
}

extension BlueBox {
    /// DrawSprocketLib: with no dialog to choose a display from, the user's
    /// choice is the one context there is (Skittles 2 asks for it).
    @Suite struct DrawSprocket {
        @Test func userSelectContextGivesTheContext() {
            bootMachine()
            let attrs: UInt32 = 0x6000_0000, out: UInt32 = 0x6000_0100, best: UInt32 = 0x6000_0104
            for i in 0..<72 { wr8(attrs + UInt32(i), 0) }
            wr32(attrs + 4, 640)
            wr32(attrs + 8, 480)
            func call(_ name: String, _ args: [UInt32]) -> UInt32 {
                for (i, a) in args.enumerated() {
                    withUnsafeMutableBytes(of: &g_ppc.gpr) { $0.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
                }
                cfm_export_fn(name)!(&g_ppc)
                return g_ppc.gpr.3
            }
            #expect(call("DSpStartup", []) == 0)
            #expect(call("DSpUserSelectContext", [attrs, 0, 0, out]) == 0)
            #expect(call("DSpFindBestContext", [attrs, best]) == 0)
            #expect(rd32(out) != 0 && rd32(out) == rd32(best))
        }

        /// DSpFindContextFromPoint: the context for a point on the screen
        /// (µWar asks for the one under the mouse), none off it.
        @Test func findContextFromPoint() {
            bootMachine()
            let out: UInt32 = 0x6000_0100, best: UInt32 = 0x6000_0104
            func call(_ name: String, _ args: [UInt32]) -> UInt32 {
                for (i, a) in args.enumerated() {
                    withUnsafeMutableBytes(of: &g_ppc.gpr) { $0.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
                }
                cfm_export_fn(name)!(&g_ppc)
                return g_ppc.gpr.3
            }
            #expect(call("DSpStartup", []) == 0)
            #expect(call("DSpGetFirstContext", [0, best]) == 0)
            #expect(call("DSpFindContextFromPoint", [(100 << 16) | 200, out]) == 0)
            #expect(rd32(out) != 0 && rd32(out) == rd32(best))
            #expect(Int32(bitPattern: call("DSpFindContextFromPoint", [0xFFF0_FFF0, out])) == -30446)
            #expect(rd32(out) == 0)
        }

        /// Alternate buffers are GWorlds of the context's depth; a blit
        /// between them with a source key leaves the keyed pixels alone.
        @Test func altBuffersAndKeyedBlits() {
            launchTestApp()
            _ = initToolbox()
            let attrs = tb_NewPtr(72, false, true), ctxp = tb_NewPtr(4, false, true)
            wr32(attrs + 4, 64)
            wr32(attrs + 8, 32)
            wr32(attrs + 32, 8) // backBufferDepthMask: 8 bits
            func call(_ name: String, _ args: [UInt32]) -> UInt32 {
                for (i, a) in args.enumerated() {
                    withUnsafeMutableBytes(of: &g_ppc.gpr) { $0.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
                }
                cfm_export_fn(name)!(&g_ppc)
                return g_ppc.gpr.3
            }
            #expect(call("DSpStartup", []) == 0)
            #expect(call("DSpFindBestContext", [attrs, ctxp]) == 0)
            let ctx = rd32(ctxp)
            #expect(call("DSpContext_Reserve", [ctx, attrs]) == 0)
            let altp = tb_NewPtr(4, false, true), portp = tb_NewPtr(8, false, true)
            #expect(call("DSpAltBuffer_New", [ctx, 0, 0, altp]) == 0)
            #expect(call("DSpAltBuffer_GetCGrafPtr", [rd32(altp), 0, portp, portp + 4]) == 0)
            let alt = rd32(portp)
            #expect(call("DSpContext_GetBackBuffer", [ctx, 0, portp]) == 0)
            let back = rd32(portp)
            func pixels(_ port: UInt32) -> UInt32 { // the first row's address
                let pm = rd32(rd32(port + UInt32(CGrafPort_portPixMap_off)))
                #expect(rd16(pm + UInt32(PixMap_pixelSize_off)) == 8)
                return rd32(pm)
            }
            let ab = pixels(alt), bb = pixels(back)
            for x in 0..<4 { wr8(ab + UInt32(x), x == 1 ? 0 : 7) } // pixel 1 is the key
            for x in 0..<4 { wr8(bb + UInt32(x), 3) }
            let info = tb_NewPtr(68, false, true)
            wr32(info + 12, alt)
            wr16(info + 22, 4); wr16(info + 20, 1) // srcRect 0,0,1,4
            wr32(info + 24, 0)                     // srcKey 0
            wr32(info + 32, back)
            wr16(info + 42, 4); wr16(info + 40, 1) // dstRect 0,0,1,4
            wr32(info + 48, 1)                     // kDSpBlitMode_SrcKey
            #expect(call("DSpBlit_Fastest", [info, 0]) == 0)
            #expect((0..<4).map { rd8(bb + UInt32($0)) } == [7, 3, 7, 7] && rd8(info) == 1)
            #expect(call("DSpContext_Release", [ctx]) == 0)
        }

        nonisolated(unsafe) static var timingFlags: [UInt32] = []

        /// The Display Manager's mode entries carry a VDTimingInfoRec whose
        /// csTimingFlags (at 16) say the mode is valid and safe: StarCraft
        /// takes no mode without them.
        @Test func modeListTimingsAreValid() {
            bootMachine()
            Self.timingFlags = []
            // pascal void (void *userData, DMListIndexType, DMDisplayModeListEntryPtr)
            let iterator = native_register({ c, _ in
                guard let c else { return }
                let ret = cpu_pop32(c)
                let entry = cpu_pop32(c)
                _ = cpu_pop32(c)
                _ = cpu_pop32(c)
                DrawSprocket.timingFlags.append(rd32(rd32(entry + 12) + 16))
                cpu_push32(c, ret)
            }, "test mode iterator", 0)
            let count: UInt32 = 0x6000_0200, list: UInt32 = 0x6000_0204
            func call(_ name: String, _ args: [UInt32]) -> UInt32 {
                for (i, a) in args.enumerated() {
                    withUnsafeMutableBytes(of: &g_ppc.gpr) { $0.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
                }
                cfm_export_fn(name)!(&g_ppc)
                return g_ppc.gpr.3
            }
            #expect(call("DMNewDisplayModeList", [0, 0, 0, count, list]) == 0 && rd32(count) > 0)
            #expect(call("DMGetIndexedDisplayModeFromList", [rd32(list), 0, 0, iterator, 0]) == 0)
            #expect(Self.timingFlags == [3])
        }
    }
}
