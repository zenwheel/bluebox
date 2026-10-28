import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// ControlStripLib's module services.
    @Suite struct OS9RestControlStrip {
        init() { bootMachine() }

        @Test func fontAndHotKey() {
            let p = Rest.ptr(4), k = Rest.ptr(2)
            #expect(Rest.signed("SBGetControlStripFontID", [p]) == 0 && rd16(p) == 3)
            #expect(Rest.signed("SBSetControlStripFontID", [22]) == 0)
            #expect(Rest.signed("SBGetControlStripFontID", [p]) == 0 && rd16(p) == 22)
            #expect(Rest.signed("SBGetControlStripFontSize", [p]) == 0 && rd16(p) == 9)
            #expect(Rest.signed("SBSetControlStripFontSize", [12]) == 0)
            #expect(Rest.signed("SBGetControlStripFontSize", [p]) == 0 && rd16(p) == 12)
            #expect(Rest.signed("SBSetControlStripFontSize", [0]) == -50)
            #expect(Rest.signed("SBGetControlStripFontID", [0]) == -50)

            #expect(Rest.signed("SBGetShowHideHotKey", [p, k]) == 0 && rd16(p) != 0 && rd8(k) == 1)
            #expect(Rest.signed("SBSetShowHideHotKey", [0x0900, 0x24]) == 0)
            #expect(Rest.signed("SBGetShowHideHotKey", [p, k]) == 0 && rd16(p) == 0x0900 && rd8(k) == 0x24)
            #expect(Rest.signed("SBIsShowHideHotKeyEnabled", [p]) == 0 && rd8(p) == 1)
            #expect(Rest.signed("SBEnableShowHideHotKey", [0]) == 0)
            #expect(Rest.signed("SBIsShowHideHotKeyEnabled", [p]) == 0 && rd8(p) == 0)
        }

        @Test func barGraphs() {
            #expect(Rest.signed("SBGetBarGraphWidth", [0]) == 0)
            #expect(Rest.signed("SBGetBarGraphWidth", [1]) == 4)
            #expect(Rest.signed("SBGetBarGraphWidth", [7]) == 7 * 4 + 6)
            // drawing needs a port; a bare one with the point packed v, h
            Rest.call("SBDrawBarGraph", [3, 7, 0, (10 << 16) | 20])
        }

        @Test func slidersWithoutAMouse() {
            let rect = Rest.ptr(8), hit = Rest.ptr(2)
            wr8(hit, 9)
            // no button down: the initial value, not hit
            #expect(Int16(truncatingIfNeeded: Rest.call("SBTrackSlider", [rect, 8, 3])) == 3)
            #expect(Int16(truncatingIfNeeded: Rest.call("SBHitTrackSlider", [rect, 8, 5, hit])) == 5 && rd8(hit) == 0)
            // a value past the slider's ends is held to them
            #expect(Int16(truncatingIfNeeded: Rest.call("SBTrackSlider", [rect, 8, 40])) == 7)
            #expect(Int16(truncatingIfNeeded: Rest.call("SBTrackSlider", [rect, 8, UInt32(bitPattern: -4)])) == 0)
            #expect(Rest.signed("SBShowHelpString", [rect, Rest.ptr(8)]) == 0)
            #expect(Rest.signed("SBShowHelpString", [0, 0]) == -50)
            #expect(Int16(truncatingIfNeeded: Rest.call("SBTrackPopupMenu", [0, 0])) == 0)
        }
    }
}
