import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

/// Helpers for the CarbonLib services tests (Carbon*Tests.swift).

/// A CarbonLib export called as a PowerPC program calls it: arguments in
/// r3-r10 and the parameter area, the result in r3.  The registers are put
/// back afterwards, so calls nest (from a callback inside another call).
@discardableResult
func svcCall(_ name: String, _ args: UInt32..., f1: Double? = nil) -> UInt32 {
    guard let fn = cfm_export_fn(name) else {
        Issue.record("\(name) is not exported")
        return 0
    }
    let saved = g_ppc
    let stack = tb_NewPtr(0x8000, false, true)
    let sp = (stack + 0x7000) & ~15
    withUnsafeMutableBytes(of: &g_ppc.gpr) { raw in
        let r = raw.bindMemory(to: UInt32.self)
        for (i, a) in args.enumerated() where i < 8 { r[3 + i] = a }
        r[1] = sp
    }
    for (i, a) in args.enumerated() where i >= 8 { wr32(sp + 24 + 4 * UInt32(i), a) }
    if let f1 { g_ppc.fpr.1 = f1.bitPattern }
    g_ppc.lr = 0x1000
    // The global's address, not an access held through the call: calls nest.
    let cpu = withUnsafeMutablePointer(to: &g_ppc) { $0 }
    fn(cpu)
    let r = g_ppc.gpr.3
    g_ppc = saved
    tb_DisposePtr(stack)
    return r
}

func svcStatus(_ v: UInt32) -> Int32 { Int32(bitPattern: v) }

func svcPtr(_ n: Int = 64) -> UInt32 { tb_NewPtr(Int32(n), false, true) }

func svcRect(_ t: Int, _ l: Int, _ b: Int, _ r: Int) -> UInt32 {
    let p = svcPtr(8)
    for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(i * 2), UInt16(bitPattern: Int16(v))) }
    return p
}

func svcPStr(_ s: String) -> UInt32 {
    let p = svcPtr(256)
    guestPString(s, at: p)
    return p
}

/// UTF-16 text in guest memory.
func svcUTF16(_ s: String) -> (UInt32, UInt32) {
    let u = Array(s.utf16)
    let p = svcPtr(max(2, u.count * 2))
    for (i, c) in u.enumerated() { wr16(p + UInt32(2 * i), c) }
    return (p, UInt32(u.count))
}

func svcBytes(_ p: UInt32, _ n: Int) -> [UInt8] { (0..<n).map { rd8(p + UInt32($0)) } }

func svcCFString(_ s: String) -> UInt32 {
    let p = svcPtr(s.utf8.count + 1)
    for (i, b) in s.utf8.enumerated() { wr8(p + UInt32(i), b) }
    return svcCall("CFStringCreateWithCString", 0, p, 0x0800_0100)
}

func svcString(_ cf: UInt32) -> String {
    let n = Int(svcCall("CFStringGetLength", cf))
    let buf = svcPtr(max(2, n * 2))
    svcCall("CFStringGetCharacters", cf, 0, UInt32(n), buf)
    return String(decoding: (0..<n).map { rd16(buf + UInt32(2 * $0)) }, as: UTF16.self)
}

func svcDouble(_ p: UInt32) -> Double { Double(bitPattern: UInt64(rd32(p)) << 32 | UInt64(rd32(p + 4))) }

/// A routine descriptor for 68k native code, as a Carbon UPP.
func svcUPP68k(_ fn: native_fn, _ name: String, _ procInfo: UInt32) -> UInt32 {
    let code = native_register(fn, name, 0)
    return svcCall("NewRoutineDescriptor", code, procInfo, 0)
}

