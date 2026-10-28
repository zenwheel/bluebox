import CCore
import CPU68K
import CPUPPC
import Testing
import Toolbox

/// A machine ready for QuickTime calls from PowerPC code: the test
/// application launched, a 68k stack for the traps the exports make.
func qtMachine() {
    launchTestApp()
    _ = initToolbox()
    let stack = tb_NewPtr(65536, false, true)
    g_cpu!.pointee.a.7 = stack + 65536 - 16
    g_cpu!.pointee.halted = false
    _ = tb_EnterMovies()
}

/// Call a QuickTime export as a PowerPC program does: arguments in r3 on
/// (and the parameter area past the eighth), r3 back.
@discardableResult
func qtCall(_ name: String, _ args: [UInt32]) -> UInt32 {
    guard let fn = cfm_export_fn(name) else {
        Issue.record("no export \(name)")
        return 0
    }
    var c = ppc_cpu()
    ppc_init(&c)
    let area = tb_NewPtr(4096, false, true) + 3072 // frames go below r1
    withUnsafeMutableBytes(of: &c.gpr) { raw in
        raw.storeBytes(of: area, toByteOffset: 4, as: UInt32.self) // r1
        for (i, v) in args.prefix(8).enumerated() { raw.storeBytes(of: v, toByteOffset: (3 + i) * 4, as: UInt32.self) }
    }
    for (i, v) in args.enumerated() where i >= 8 { wr32(area + 24 + UInt32(i * 4), v) }
    fn(&c)
    return withUnsafeBytes(of: c.gpr) { $0.load(fromByteOffset: 12, as: UInt32.self) }
}

func qtCallErr(_ name: String, _ args: [UInt32]) -> Int32 { Int32(bitPattern: qtCall(name, args)) }

extension BlueBox {
    @Suite struct QTComponents {
        /// The component calls' table: selectors and argument sizes as the
        /// interfaces have them, and an export for each.
        @Test func callTable() {
            #expect(qt_call_count > 1000)
            let q = qt_call_find("MovieExport", 0x80)
            #expect(q != nil && String(cString: q!.pointee.name) == "MovieExportToHandle")
            #expect(String(cString: q!.pointee.sizes) == "44444")
            let d = qt_call_find("DataH", 0x0028)
            #expect(d != nil)
            for name in ["MovieExportToHandle", "DataHGetData", "TextMediaAddTextSample", "SpriteMediaSetProperty",
                         "ImageCodecPreDecompress", "GraphicsExportDoExport", "ClockGetTime", "MusicGetPart",
                         "QTSMediaGetInfo", "IDHGetDeviceList", "TweenerDoTween", "RTPRssmInitialize"] {
                #expect(cfm_export_fn(name) != nil, "\(name)")
            }
        }

        /// A call reaches the instance through the Component Manager (the
        /// tune player, ours); one to no instance answers badComponentInstance.
        @Test func callsReachInstances() {
            qtMachine()
            let tune = components_open_default(fourCC("tune"), 0)
            #expect(tune != 0)
            #expect(qtCall("TuneSetVolume", [tune, 0x8000]) == 0)
            #expect(qtCall("TuneGetVolume", [tune]) == 0x8000)
            #expect(qtCall("TuneGetVolume", [0x1234_5678]) == 0x8000_8001)
            components_close(tune)
        }
    
        /// Atom containers: children by type, ID and index, leaf data, and
        /// the tree kept consistent through inserts, removals and swaps.
        @Test func atomContainers() {
            qtMachine()
            let out = tb_NewPtr(16, false, true)
            #expect(qtCall("QTNewAtomContainer", [out]) == 0)
            let c = rd32(out)
            let data = tb_NewPtr(8, false, true)
            wr32(data, 0x1122_3344)
            // 'sprt' 1 and 2 under the root, a leaf 'name' under sprite 2
            #expect(qtCall("QTInsertChild", [c, 0, fourCC("sprt"), 1, 0, 0, 0, out]) == 0)
            let s1 = rd32(out)
            #expect(qtCall("QTInsertChild", [c, 0, fourCC("sprt"), 0, 0, 0, 0, out]) == 0)
            var s2 = rd32(out)
            #expect(qtCall("QTInsertChild", [c, s2, fourCC("name"), 1, 0, 4, data, out]) == 0)
            #expect(qtCall("QTCountChildrenOfType", [c, 0, fourCC("sprt")]) == 2)
            #expect(qtCall("QTCountChildrenOfType", [c, 0, 0]) == 2)
            // the new sprite's ID was made unique
            let idp = tb_NewPtr(4, false, true)
            s2 = qtCall("QTFindChildByIndex", [c, 0, fourCC("sprt"), 2, idp])
            #expect(s2 != 0 && rd32(idp) == 2)
            #expect(qtCall("QTFindChildByID", [c, 0, fourCC("sprt"), 1, idp]) == s1)
            #expect(qtCall("QTInsertChild", [c, 0, fourCC("sprt"), 1, 0, 0, 0, out]) == UInt32(bitPattern: -2105))
            let name = qtCall("QTFindChildByID", [c, s2, fourCC("name"), 1, 0])
            #expect(name != 0)
            #expect(qtCall("QTGetAtomParent", [c, name]) == s2)
            let h = tb_NewHandle(0, false, false)
            #expect(qtCall("QTCopyAtomDataToHandle", [c, name, h]) == 0)
            #expect(tb_GetHandleSize(h) == 4 && rd32(rd32(h)) == 0x1122_3344)
            #expect(qtCall("QTCopyAtomDataToHandle", [c, s2, h]) == UInt32(bitPattern: -2102)) // not a leaf
            wr32(data, 0xAABB_CCDD)
            wr32(data + 4, 0x5566_7788)
            #expect(qtCall("QTSetAtomData", [c, name, 8, data]) == 0)
            let size = tb_NewPtr(4, false, true)
            #expect(qtCall("QTCopyAtomDataToPtr", [c, name, 0, 8, data, size]) == 0 && rd32(size) == 8)
            #expect(qtCall("QTGetNextChildType", [c, 0, 0]) == fourCC("sprt"))
            #expect(qtCall("QTGetNextChildType", [c, 0, fourCC("sprt")]) == 0)
            #expect(qtCall("QTNextChildAnyType", [c, 0, 0, out]) == 0 && rd32(out) == s1)
            // swap: sprite 1 gets sprite 2's child, each keeps its ID
            #expect(qtCall("QTSwapAtoms", [c, s1, s2]) == 0)
            let first = qtCall("QTFindChildByID", [c, 0, fourCC("sprt"), 1, 0])
            #expect(qtCall("QTCountChildrenOfType", [c, first, fourCC("name")]) == 1)
            // a copy of the container holds the same
            #expect(qtCall("QTCopyAtom", [c, 0, out]) == 0)
            let copy = rd32(out)
            #expect(qtCall("QTCountChildrenOfType", [copy, 0, fourCC("sprt")]) == 2)
            #expect(qtCall("QTRemoveAtom", [c, first]) == 0)
            #expect(qtCall("QTCountChildrenOfType", [c, 0, fourCC("sprt")]) == 1)
            #expect(qtCall("QTRemoveChildren", [c, 0]) == 0)
            #expect(qtCall("QTCountChildrenOfType", [c, 0, 0]) == 0)
            #expect(tb_GetHandleSize(c) == 32)
            #expect(qtCall("QTInsertChildren", [c, 0, copy]) == 0)
            #expect(qtCall("QTCountChildrenOfType", [c, 0, fourCC("sprt")]) == 2)
            #expect(qtCall("QTLockContainer", [c]) == 0)
            let n2 = qtCall("QTFindChildByID", [c, qtCall("QTFindChildByID", [c, 0, fourCC("sprt"), 1, 0]),
                                               fourCC("name"), 1, 0])
            let ptr = tb_NewPtr(4, false, true)
            #expect(qtCall("QTGetAtomDataPtr", [c, n2, size, ptr]) == 0)
            #expect(rd32(size) == 8 && rd32(rd32(ptr)) == 0xAABB_CCDD)
            #expect(qtCall("QTUnlockContainer", [c]) == 0)
            #expect(qtCall("QTDisposeAtomContainer", [c]) == 0)
        }

        /// User data: items by type and index, text by language, and its
        /// flattened form back again.
        @Test func userData() {
            qtMachine()
            let out = tb_NewPtr(4, false, true)
            #expect(qtCall("NewUserData", [out]) == 0)
            let ud = rd32(out)
            let h = tb_NewHandle(3, false, false)
            gput(rd32(h), [1, 2, 3], 3)
            #expect(qtCall("AddUserData", [ud, h, fourCC("abcd")]) == 0)
            #expect(qtCall("AddUserData", [ud, h, fourCC("abcd")]) == 0)
            #expect(qtCall("CountUserDataType", [ud, fourCC("abcd")]) == 2)
            let text = tb_NewHandle(5, false, false)
            gput(rd32(text), Array("Hello".utf8), 5)
            #expect(qtCall("AddUserDataText", [ud, text, 0xA96E_616D, 1, 0]) == 0) // '©nam', English
            gput(rd32(text), Array("Salut".utf8), 5)
            #expect(qtCall("AddUserDataText", [ud, text, 0xA96E_616D, 1, 1]) == 0) // French
            #expect(qtCall("GetNextUserDataType", [ud, 0]) == fourCC("abcd"))
            #expect(qtCall("GetNextUserDataType", [ud, fourCC("abcd")]) == 0xA96E_616D)
            let got = tb_NewHandle(0, false, false)
            #expect(qtCall("GetUserDataText", [ud, got, 0xA96E_616D, 1, 0]) == 0)
            #expect(tb_GetHandleSize(got) == 5 && rd8(rd32(got)) == UInt8(ascii: "H"))
            #expect(qtCall("GetUserDataText", [ud, got, 0xA96E_616D, 1, 2]) == UInt32(bitPattern: -2026))
            #expect(qtCall("RemoveUserData", [ud, fourCC("abcd"), 1]) == 0)
            let flat = tb_NewHandle(0, false, false)
            #expect(qtCall("PutUserDataIntoHandle", [ud, flat]) == 0)
            #expect(tb_GetHandleSize(flat) == 8 + 3 + 8 + 18)
            #expect(qtCall("NewUserDataFromHandle", [flat, out]) == 0)
            let again = rd32(out)
            #expect(qtCall("CountUserDataType", [again, 0xA96E_616D]) == 1)
            #expect(qtCall("GetUserDataText", [again, got, 0xA96E_616D, 1, 1]) == 0)
            #expect(rd8(rd32(got)) == UInt8(ascii: "S"))
            #expect(qtCall("DisposeUserData", [ud]) == 0)
            #expect(qtCall("DisposeUserData", [again]) == 0)
        }
    }
}
