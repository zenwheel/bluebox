import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// UnicodeConverter's older names.
    @Suite struct OS9RestText {
        init() { Rest.boot() }

        func bytes(_ b: [UInt8]) -> UInt32 {
            let p = Rest.ptr(max(b.count, 1))
            for (i, x) in b.enumerated() { wr8(p + UInt32(i), x) }
            return p
        }

        func read(_ p: UInt32, _ n: UInt32) -> [UInt8] { (0..<n).map { rd8(p + $0) } }

        @Test func textToUnicodeReplacesOffsetsInPlace() {
            launchTestApp()
            let t2u = Rest.ptr(4)
            #expect(Rest.signed("CreateTextToUnicodeInfoByEncoding", [0, t2u]) == 0) // Mac OS Roman
            let source = bytes([0x41, 0x8E, 0x42]) // A é B
            let offsets = Rest.ptr(12)
            wr32(offsets, 1); wr32(offsets + 4, 2)
            let consumed = Rest.ptr(4), length = Rest.ptr(4), out = Rest.ptr(64)
            #expect(Rest.signed("ConvertTextToUnicode", [rd32(t2u), 3, source, 0, 2, offsets, 64, consumed, length, out]) == 0)
            #expect(rd32(consumed) == 3 && rd32(length) == 6)
            #expect(read(out, 6) == [0x00, 0x41, 0x00, 0xE9, 0x00, 0x42])
            #expect(rd32(offsets) == 2 && rd32(offsets + 4) == 4) // where the characters at 1 and 2 went
            // no offsets at all
            #expect(Rest.signed("ConvertTextToUnicode", [rd32(t2u), 3, source, 0, 0, 0, 64, consumed, length, out]) == 0)
            #expect(rd32(length) == 6)
            // too little room: the converter's own status
            #expect(Rest.signed("ConvertTextToUnicode", [rd32(t2u), 3, source, 0, 0, 0, 2, consumed, length, out]) != 0)
        }

        @Test func unicodeToTextReplacesOffsetsInPlace() {
            launchTestApp()
            let u2t = Rest.ptr(4)
            #expect(Rest.signed("CreateUnicodeToTextInfoByEncoding", [0, u2t]) == 0)
            let source = bytes([0x00, 0x41, 0x00, 0xE9])
            let offsets = Rest.ptr(8)
            wr32(offsets, 2)
            let consumed = Rest.ptr(4), length = Rest.ptr(4), out = Rest.ptr(16)
            #expect(Rest.signed("ConvertUnicodeToText", [rd32(u2t), 4, source, 0, 1, offsets, 16, consumed, length, out]) == 0)
            #expect(read(out, rd32(length)) == [0x41, 0x8E] && rd32(offsets) == 1)
        }

        @Test func plainRunsHaveNoOffsets() {
            launchTestApp()
            let run = Rest.ptr(4)
            let encs = bytes(Rest.b32(0) + Rest.b32(1)) // Roman, Japanese
            #expect(Rest.signed("CreateUnicodeToTextRunInfoByEncoding", [2, encs, run]) == 0)
            let source = bytes([0x00, 0x41, 0x30, 0x42]) // A, hiragana A
            let consumed = Rest.ptr(4), length = Rest.ptr(4), out = Rest.ptr(16), nruns = Rest.ptr(4), runs = Rest.ptr(64)
            #expect(Rest.signed("ConvertPlainUnicodeToTextRun", [rd32(run), 4, source, 0, 16, consumed, length, out, 8, nruns, runs]) == 0)
            #expect(read(out, rd32(length)) == [0x41, 0x82, 0xA0] && rd32(nruns) == 2)
            #expect(rd32(runs + 4) == 0 && rd32(runs + 12) == 1)
            // script code runs: the Roman script is 0, the Japanese 1
            let scripts = Rest.ptr(4)
            wr16(scripts, 0); wr16(scripts + 2, 1)
            #expect(Rest.signed("CreateUnicodeToTextRunInfoByScriptCode", [2, scripts, run]) == 0)
            #expect(Rest.signed("ConvertPlainUnicodeToScriptCodeRun", [rd32(run), 4, source, 0, 16, consumed, length, out, 8, nruns, runs]) == 0)
            #expect(read(out, rd32(length)) == [0x41, 0x82, 0xA0] && rd32(nruns) == 2)
            #expect(rd16(runs + 4) == 0 && rd16(runs + 10) == 1) // ScriptCodeRun: offset, script
        }

        @Test func tecOptionsAreUnimplemented() {
            #expect(Rest.signed("TECGetOption", [0, 0, 0]) == -4)
            #expect(Rest.signed("TECSetOption", [0, 0, 0]) == -4)
        }
    }
}
