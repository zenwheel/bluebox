import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

extension Classic {
    /// SANE through its traps, as compiled code calls it.
    @Suite struct SANE {
        init() { bootMachine() }

        var cpu: UnsafeMutablePointer<cpu68k> { g_cpu! }

        func run(_ words: [UInt16]) -> String {
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            let stack = tb_NewPtr(65536, false, true)
            cpu.pointee.a.7 = stack + 65536 - 16
            cpu.pointee.pc = code
            cpu.pointee.halted = false
            _ = cpu_run(cpu, 10_000)
            return String(cString: machine_halt_reason())
        }

        func pea(_ a: UInt32) -> [UInt16] { [0x4879, UInt16(a >> 16), UInt16(a & 0xFFFF)] }
        func op(_ w: UInt16, _ trap: UInt16 = 0xA9EB) -> [UInt16] { [0x3F3C, w, trap] }

        func ext(_ v: Double) -> UInt32 {
            let p = tb_NewPtr(10, false, true)
            let bits = v.bitPattern
            let e = Int((bits >> 52) & 0x7FF), m = (bits & 0xF_FFFF_FFFF_FFFF) | (1 << 52)
            let se = UInt16(v < 0 ? 0x8000 : 0) | UInt16(v == 0 ? 0 : e - 1023 + 16383)
            let mant = v == 0 ? 0 : m << 11
            wr16(p, se); wr32(p + 2, UInt32(mant >> 32)); wr32(p + 6, UInt32(mant & 0xFFFF_FFFF))
            return p
        }

        func double(_ p: UInt32) -> Double {
            let se = rd16(p)
            let mant = UInt64(rd32(p + 2)) << 32 | UInt64(rd32(p + 6))
            if se & 0x7FFF == 0 && mant == 0 { return 0 }
            let v = Double(mant) * pow(2, Double(Int(se & 0x7FFF) - 16383 - 63))
            return se & 0x8000 != 0 ? -v : v
        }

        @Test func arithmeticConversionAndComparison() {
            let x = ext(1.5)
            let y = tb_NewPtr(8, false, true)
            wr32(y, UInt32(2.25.bitPattern >> 32)); wr32(y + 4, UInt32(2.25.bitPattern & 0xFFFF_FFFF))
            let l = tb_NewPtr(4, false, true)
            let code = pea(y) + pea(x) + op(0x0800)       // FADDD: x += y
                + pea(x) + pea(l) + op(0x2810)              // FX2L: l = x
                + pea(y) + pea(x) + op(0x0808)              // FCMPD: x vs y
                + [0x42C6, 0xA9F4]                          // MOVE CCR,D6; ExitToShell
            #expect(run(code) == "ExitToShell")
            #expect(double(x) == 3.75)
            #expect(rd32(l) == 4)
            #expect(cpu.pointee.d.6 & 0x1F == 0) // greater: no flags
        }

        @Test func elementaryFunctionsAndDecimal() {
            let x = ext(0)
            let pi = ext(Double.pi)
            let form = tb_NewPtr(4, false, true)
            wr16(form, 0); wr16(form + 2, 5)
            let dec = tb_NewPtr(26, false, true)
            let code = pea(x) + op(0x0008, 0xA9EC)          // exp(x)
                + pea(form) + pea(pi) + pea(dec) + op(0x000B) // FX2DEC
                + [0xA9F4]
            #expect(run(code) == "ExitToShell")
            #expect(double(x) == 1)
            #expect(rd8(dec) == 0 && Int16(bitPattern: rd16(dec + 2)) == -4)
            #expect(pstring(at: dec + 4) == "31416")
        }

        @Test func fix2XLeavesTheResultAddress() {
            let result = tb_NewPtr(10, false, true)
            // PEA result; MOVE.L #$00018000,-(SP) (1.5); _Fix2X; MOVEA.L (SP)+,A2;
            // then X2Fix back: CLR.L -(SP); MOVE.L A2,-(SP); _X2Fix; MOVE.L (SP)+,D5
            let code = pea(result) + [0x2F3C, 0x0001, 0x8000, 0xA843, 0x245F,
                                      0x42A7, 0x2F0A, 0xA844, 0x2A1F, 0xA9F4]
            #expect(run(code) == "ExitToShell")
            #expect(double(result) == 1.5)
            #expect(cpu.pointee.a.2 == result)
            #expect(cpu.pointee.d.5 == 0x0001_8000)
        }

        @Test func decimalStrings() {
            let form = tb_NewPtr(4, false, true)
            wr16(form, 1); wr16(form + 2, 2) // fixed, two places
            let dec = tb_NewPtr(26, false, true)
            let s = tb_NewPtr(256, false, true), idx = tb_NewPtr(2, false, true), ok = tb_NewPtr(2, false, true)
            guestPString("  -12.345e1xyz", at: s)
            wr16(idx, 1)
            tb_ROMlib_Fpstr2dec(s, idx, dec, ok)
            #expect(rd8(ok) == 1 && rd16(idx) == 12) // stops at 'x'
            #expect(rd8(dec) == 1 && Int16(bitPattern: rd16(dec + 2)) == -2 && pstring(at: dec + 4) == "12345")
            let out = tb_NewPtr(256, false, true)
            tb_ROMlib_Fdec2str(form, dec, out)
            #expect(pstring(at: out) == "-123.45")
            wr16(form, 0); wr16(form + 2, 3) // floating, three digits
            tb_ROMlib_Fdec2str(form, dec, out)
            #expect(pstring(at: out) == "-1.23e+2")
        }
    }
}
