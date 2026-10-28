import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// InputSprocket's driver side: devices and elements a driver makes
    /// and the data it pushes into them.
    @Suite struct OS9RestInputSprocket {
        init() {
            bootMachine()
            _ = cfm_ppc_cpu()
            inputsprocket_reset()
        }

        func fourCC(_ s: String) -> UInt32 { s.utf8.reduce(0) { $0 << 8 | UInt32($1) } }

        /// A device definition of a class, the meta handler that returns
        /// init (when given selector 1), and the device.
        func makeDevice(cls: String, initFn: UInt32 = 0) -> UInt32 {
            let def = Rest.ptr(160)
            guestPString("Test Wheel", at: def)
            wr32(def + 132, fourCC(cls))
            let meta = Rest.routine([0x2C04_0001, 0x4082_0010, 0x3C60_0000 | (initFn >> 16),
                                     0x6063_0000 | (initFn & 0xFFFF), 0x4E80_0020, 0x3860_0000, 0x4E80_0020])
            let out = Rest.ptr(4)
            #expect(Rest.signed("ISpDevice_New", [def, meta, 0x77, out]) == 0)
            return rd32(out)
        }

        func makeElement(_ device: UInt32, kind: String, label: String, size: UInt32 = 4) -> UInt32 {
            let def = Rest.ptr(100)
            wr32(def, device)
            guestPString("Control", at: def + 8)
            wr32(def + 72, fourCC(kind))
            wr32(def + 76, fourCC(label))
            wr32(def + 88, size)
            let out = Rest.ptr(4)
            #expect(Rest.signed("ISpElement_New", [def, out]) == 0)
            return rd32(out)
        }

        func simple(_ e: UInt32) -> (Int32, UInt32) {
            let v = Rest.ptr(4)
            let err = Rest.signed("ISpElement_GetSimpleState", [e, v])
            return (err, rd32(v))
        }

        @Test func aDriversDeviceIsListed() {
            let count = Rest.ptr(4), devs = Rest.ptr(64)
            #expect(Rest.signed("ISpDevices_Extract", [0, count, 0]) == 0)
            let before = rd32(count)
            let dev = makeDevice(cls: "whel")
            #expect(Rest.signed("ISpDevices_Extract", [0, count, 0]) == 0 && rd32(count) == before + 1)
            #expect(Rest.signed("ISpDevices_ExtractByClass", [fourCC("whel"), 4, count, devs]) == 0)
            #expect(rd32(count) == 1 && rd32(devs) == dev)
            let def = Rest.ptr(160)
            #expect(Rest.signed("ISpDevice_GetDefinition", [dev, 160, def]) == 0)
            #expect(pstring(at: def) == "Test Wheel" && rd32(def + 132) == fourCC("whel"))
            let active = Rest.ptr(2)
            #expect(Rest.signed("ISpDevice_IsActive", [dev, active]) == 0 && rd8(active) == 1)
            #expect(Rest.signed("ISpDevices_DeactivateClass", [fourCC("whel")]) == 0)
            #expect(Rest.signed("ISpDevice_IsActive", [dev, active]) == 0 && rd8(active) == 0)
            #expect(Rest.signed("ISpDevice_Dispose", [dev]) == 0)
            #expect(Rest.signed("ISpDevices_Extract", [0, count, 0]) == 0 && rd32(count) == before)
            #expect(Rest.signed("ISpDevice_Dispose", [dev]) == -50)
            #expect(Rest.signed("ISpDevice_New", [0, 0, 0, devs]) == -50)
        }

        @Test func elementsTakeWhatIsPushed() {
            let dev = makeDevice(cls: "whel")
            let axis = makeElement(dev, kind: "axis", label: "xaxi")
            #expect(simple(axis) == (0, 0x7FFF_FFFF)) // the middle
            #expect(Rest.signed("ISpElement_PushSimpleData", [axis, 0xC000_0000, 0]) == 0)
            #expect(simple(axis) == (0, 0xC000_0000))
            let ev = Rest.ptr(24), was = Rest.ptr(2)
            #expect(Rest.signed("ISpElement_GetNextEvent", [axis, 24, ev, was]) == 0 && rd8(was) == 1)
            #expect(rd32(ev + 8) == axis && rd32(ev + 16) == 0xC000_0000)
            #expect(Rest.signed("ISpElement_GetNextEvent", [axis, 24, ev, was]) == 0 && rd8(was) == 0)
            // the same value pushed again is an event again
            #expect(Rest.signed("ISpElement_PushSimpleData", [axis, 0xC000_0000, 0]) == 0)
            #expect(Rest.signed("ISpElement_GetNextEvent", [axis, 24, ev, was]) == 0 && rd8(was) == 1)

            let delta = makeElement(dev, kind: "dlta", label: "xaxi")
            Rest.call("ISpElement_PushSimpleData", [delta, 5, 0])
            Rest.call("ISpElement_PushSimpleData", [delta, 7, 0])
            #expect(simple(delta) == (0, 12)) // they add up
            #expect(simple(delta) == (0, 0))  // until read

            let move = makeElement(dev, kind: "movm", label: "mov0", size: 12)
            let data = Rest.ptr(12)
            wr32(data, 1); wr32(data + 4, 2); wr32(data + 8, 3)
            #expect(Rest.signed("ISpElement_PushComplexData", [move, 8, data, 0]) == -50) // the wrong size
            #expect(Rest.signed("ISpElement_PushComplexData", [move, 12, data, 0]) == 0)
            let state = Rest.ptr(12)
            #expect(Rest.signed("ISpElement_GetComplexState", [move, 12, state]) == 0)
            #expect(rd32(state) == 1 && rd32(state + 4) == 2 && rd32(state + 8) == 3)
            #expect(Rest.signed("ISpElement_GetComplexState", [move, 8, state]) == -30342)
            #expect(simple(move) == (0, 3))

            // the device's element list holds them, and disposing takes them away
            let list = Rest.ptr(4), found = Rest.ptr(4)
            #expect(Rest.signed("ISpDevice_GetElementList", [dev, list]) == 0)
            #expect(Rest.signed("ISpElementList_Extract", [rd32(list), 0, found, 0]) == 0 && rd32(found) == 3)
            #expect(Rest.signed("ISpElement_Dispose", [delta]) == 0)
            #expect(simple(delta).0 == -30344)
            #expect(Rest.signed("ISpElement_PushSimpleData", [delta, 1, 0]) == -50)
            #expect(Rest.signed("ISpElement_Dispose", [delta]) == -50)
            #expect(Rest.signed("ISpDevice_Dispose", [dev]) == 0)
            #expect(simple(axis).0 == -30344 && simple(move).0 == -30344)
            // not a device's: no
            #expect(Rest.signed("ISpElement_PushSimpleData", [0x1234, 1, 0]) == -50)
        }

        @Test func aDriverTakesTheGamesNeeds() {
            // an init that says it used the first need
            let initFn = Rest.routine([0x3800_0001, 0x9807_0000, 0x3860_0000, 0x4E80_0020])
            _ = makeDevice(cls: "whel", initFn: initFn)
            let needs = Rest.ptr(92 * 2), elements = Rest.ptr(8)
            for (i, (name, label)) in [("Fire", "fire"), ("Jump", "jump")].enumerated() {
                let n = needs + UInt32(i * 92)
                guestPString(name, at: n)
                wr32(n + 68, fourCC("butn"))
                wr32(n + 72, fourCC(label))
            }
            #expect(Rest.signed("ISpElement_NewVirtualFromNeeds", [2, needs, elements, 0]) == 0)
            let fire = rd32(elements), jump = rd32(elements + 4)
            #expect(Rest.signed("ISpElement_PushSimpleData", [fire, 1, 0]) == 0)
            #expect(simple(fire) == (0, 1))
            #expect(Rest.signed("ISpElement_PushSimpleData", [jump, 1, 0]) == -50) // the driver didn't take it
        }

        @Test func drawSprocketsCurrentContext() {
            launchTestApp()
            _ = initToolbox()
            let attrs = Rest.ptr(72), ctxp = Rest.ptr(4), cur = Rest.ptr(4)
            wr32(attrs + 4, 64)
            wr32(attrs + 8, 32)
            wr32(attrs + 32, 8)
            #expect(Rest.signed("DSpStartup") == 0)
            #expect(Rest.signed("DSpFindBestContext", [attrs, ctxp]) == 0)
            wr32(cur, 9)
            #expect(Rest.signed("DSpGetCurrentContext", [1, cur]) == -30446 && rd32(cur) == 0) // none reserved yet
            #expect(Rest.signed("DSpContext_Reserve", [rd32(ctxp), attrs]) == 0)
            #expect(Rest.signed("DSpGetCurrentContext", [1, cur]) == 0 && rd32(cur) == rd32(ctxp))
            #expect(Rest.signed("DSpGetCurrentContext", [77, cur]) == -30446) // no such display
            #expect(Rest.signed("DSpContext_Release", [rd32(ctxp)]) == 0)
            #expect(Rest.signed("DSpGetCurrentContext", [1, cur]) == -30446)
        }
    }
}
