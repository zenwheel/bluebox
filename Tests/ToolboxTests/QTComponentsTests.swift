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
    let area = tb_NewPtr(256, false, true)
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
    }
}
