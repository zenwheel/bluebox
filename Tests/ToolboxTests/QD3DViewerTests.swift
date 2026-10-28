import CCore
import CPUPPC
import Testing
import Toolbox

extension BlueBox {
    /// The QuickDraw 3D Viewer (QD3DViewerLib): a model drawn in a window
    /// with its controller strip, moved by dragging.
    @Suite struct QD3DViewer {
        typealias Q = Q3Call
        static let defaultFlags: UInt32 = 0x8000_0000
        static let orbit: UInt32 = 1 << 7, truck: UInt32 = 1 << 6

        func rect(_ t: Int, _ l: Int, _ b: Int, _ r: Int) -> UInt32 {
            let p = tb_NewPtr(8, false, true)
            for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(i * 2), UInt16(bitPattern: Int16(v))) }
            return p
        }

        func pixel(_ h: Int, _ v: Int) -> (UInt16, UInt16, UInt16) {
            let c = tb_NewPtr(6, false, true)
            tb_GetCPixel(Int16(h), Int16(v), c)
            defer { tb_DisposePtr(c) }
            return (rd16(c), rd16(c + 2), rd16(c + 4))
        }

        func isBlue(_ p: (UInt16, UInt16, UInt16)) -> Bool { p.0 < 0x1000 && p.1 < 0x1000 && p.2 > 0xF000 }

        /// A display group holding a unit cube, red.
        func model() -> UInt32 {
            let group = Q.call("Q3DisplayGroup_New")
            let set = Q.call("Q3AttributeSet_New")
            #expect(Q.call("Q3AttributeSet_Add", [set, 5, QuickDraw3D.data([Float(1), Float(0), Float(0)])]) == 1)
            let box = Q.call("Q3Box_New", [QuickDraw3D.data([Float(-0.5), Float(-0.5), Float(-0.5), Float(0), Float(1), Float(0),
                                                             Float(0), Float(0), Float(1), Float(1), Float(0), Float(0), 0, set])])
            #expect(box != 0)
            #expect(Q.call("Q3Group_AddObject", [group, box]) != 0)
            Q.call("Q3Object_Dispose", [box])
            Q.call("Q3Object_Dispose", [set])
            return group
        }

        /// How many pixels of row y (from x0 to x1) aren't the background.
        func modelWidth(_ y: Int, _ x0: Int, _ x1: Int) -> Int {
            (x0..<x1).filter { !isBlue(pixel($0, y)) }.count
        }

        @Test func drawsAModelWithItsStrip() {
            launchTestApp()
            _ = initToolbox()
            _ = cfm_ppc_cpu()
            let w = tb_NewCWindow(0, rect(50, 50, 250, 250), 0, 1, 0, 0xFFFF_FFFF, 0, 0)
            tb_SetPort(w)
            let viewer = Q.call("Q3ViewerNew", [w, rect(0, 0, 200, 200), Self.defaultFlags])
            #expect(viewer != 0)
            let major = tb_NewPtr(8, false, true)
            #expect(Q.call("Q3ViewerGetVersion", [major, major + 4]) == 0)
            #expect(rd32(major) == 1 && rd32(major + 4) == 6)
            #expect(Q.call("Q3ViewerGetState", [viewer]) == 0) // empty
            let group = model()
            #expect(Q.call("Q3ViewerUseGroup", [viewer, group]) == 0)
            #expect(Q.call("Q3ViewerGetState", [viewer]) & 1 == 1)
            #expect(Q.call("Q3ViewerSetBackgroundColor", [viewer, QuickDraw3D.data([Float(1), Float(0), Float(0), Float(1)])]) == 0)
            #expect(Q.call("Q3ViewerDraw", [viewer]) == 0)
            tb_SetPort(w)
            let mid = pixel(100, 90)
            #expect(mid.0 > 0xC000 && mid.1 < mid.0 / 2 && mid.2 == mid.1) // the cube: red, lit, a Phong highlight
            #expect(isBlue(pixel(10, 10)))                               // the background
            #expect(pixel(0, 0) == (0, 0, 0))                            // the frame
            #expect(pixel(4, 190) == (0xDDDD, 0xDDDD, 0xDDDD))           // the strip
            // The orbit button is a button in the strip.
            let br = tb_NewPtr(8, false, true)
            #expect(Q.call("Q3ViewerGetButtonRect", [viewer, Self.orbit, br]) == 0)
            #expect(Int16(bitPattern: rd16(br)) >= 180)
            #expect(Q.call("Q3ViewerGetCurrentButton", [viewer]) == Self.orbit)

            // Dragging in orbit mode turns the cube: its outline changes.
            let before = modelWidth(90, 2, 198)
            #expect(Q.call("Q3ViewerMouseDown", [viewer, 100, 90]) == 1)
            #expect(Q.call("Q3ViewerContinueTracking", [viewer, 160, 90]) == 1)
            #expect(Q.call("Q3ViewerMouseUp", [viewer, 160, 90]) == 1)
            tb_SetPort(w)
            let after = modelWidth(90, 2, 198)
            #expect(before > 10 && after > 10 && before != after)
            #expect(Q.call("Q3ViewerGetState", [viewer]) & 2 == 2) // undo
            #expect(Q.call("Q3ViewerUndo", [viewer]) == 0)
            tb_SetPort(w)
            #expect(modelWidth(90, 2, 198) == before)

            // Flags, bounds and buttons round trip.
            #expect(Q.call("Q3ViewerSetCurrentButton", [viewer, Self.truck]) == 0)
            #expect(Q.call("Q3ViewerGetCurrentButton", [viewer]) == Self.truck)
            #expect(Q.call("Q3ViewerSetFlags", [viewer, 1 << 1]) == 0)
            #expect(Q.call("Q3ViewerGetFlags", [viewer]) == 1 << 1)
            #expect(Q.call("Q3ViewerGetButtonRect", [viewer, Self.orbit, br]) != 0) // no strip now
            #expect(Q.call("Q3ViewerSetBounds", [viewer, rect(10, 20, 110, 220)]) == 0)
            #expect(Q.call("Q3ViewerGetBounds", [viewer, br]) == 0)
            #expect([rd16(br), rd16(br + 2), rd16(br + 4), rd16(br + 6)] == [10, 20, 110, 220])
            let dim = tb_NewPtr(8, false, true)
            #expect(Q.call("Q3ViewerGetDimension", [viewer, dim, dim + 4]) == 0)
            #expect(rd32(dim) == 200 && rd32(dim + 4) == 100)
            let pict = Q.call("Q3ViewerGetPict", [viewer])
            #expect(pict != 0 && tb_GetHandleSize(pict) > 10)
            #expect(Q.call("Q3ViewerGetView", [viewer]) != 0)
            #expect(Q.call("Q3ViewerGetPort", [viewer]) == w)
            #expect(Q.call("Q3ViewerDispose", [viewer]) == 0)
            Q.call("Q3Object_Dispose", [group])
            tb_DisposeWindow(w)
        }
    }
}
