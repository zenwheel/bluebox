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

            // Deactivated, it is idle.
            wr32(Self.buf, pad)
            isp("ISpDevices_Deactivate", [1, Self.buf])
            #expect(state(a) == 0)
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
    }
}
