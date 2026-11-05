import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

extension BlueBox {
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
            // Pascal's decform keeps the style in its first byte ($0100):
            // fixed (Star Patrol's speed field, "0.0e+0" was too long).
            wr16(form, 0x0100); wr16(form + 2, 2)
            tb_ROMlib_Fdec2str(form, dec, out)
            #expect(pstring(at: out) == "-123.45")
            let x = ext(0)
            let code = pea(form) + pea(x) + pea(dec) + op(0x000B) + [0xA9F4] // FX2DEC
            #expect(run(code) == "ExitToShell")
            tb_ROMlib_Fdec2str(form, dec, out)
            #expect(pstring(at: out) == "0.00")
        }

        /// A huge value in fixed style used to overflow a 64-byte buffer in
        /// the host (PictWave's Option dialog); SANE answers '?' digits.
        @Test func hugeFixedDecimalDoesNotOverflow() {
            let big = ext(1e300)
            let form = tb_NewPtr(4, false, true)
            wr16(form, 0x0100); wr16(form + 2, 60)
            let dec = tb_NewPtr(26, false, true)
            let code = pea(form) + pea(big) + pea(dec) + op(0x000B) + [0xA9F4]
            #expect(run(code) == "ExitToShell")
            #expect(pstring(at: dec + 4) == "?")
        }

        // Raw operands: an extended's sign/exponent word and significand,
        // a double's bits.
        func extBits(_ se: UInt16, _ m: UInt64) -> UInt32 {
            let p = tb_NewPtr(12, false, true)
            wr16(p, se); wr32(p + 2, UInt32(m >> 32)); wr32(p + 6, UInt32(m & 0xFFFF_FFFF))
            return p
        }
        func bits(_ p: UInt32) -> (UInt16, UInt64) { (rd16(p), UInt64(rd32(p + 2)) << 32 | UInt64(rd32(p + 6))) }
        func dbl(_ v: Double) -> UInt32 {
            let p = tb_NewPtr(12, false, true)
            wr32(p, UInt32(v.bitPattern >> 32)); wr32(p + 4, UInt32(v.bitPattern & 0xFFFF_FFFF))
            return p
        }
        func dblBits(_ p: UInt32) -> UInt64 { UInt64(rd32(p)) << 32 | UInt64(rd32(p + 4)) }
        func word(_ v: UInt16) -> UInt32 { let p = tb_NewPtr(4, false, true); wr16(p, v); return p }
        var env: UInt16 { rd16(0x0A4A) }
        func setEnv(_ e: UInt16) { wr16(0x0A4A, e) }

        /// Extended arithmetic keeps a 64-bit significand: 1/3 rounds in its
        /// last bit by the rounding direction, and to 53 bits under double
        /// rounding precision.
        @Test func extendedPrecisionAndRounding() {
            func third(_ e: UInt16) -> (UInt16, UInt64, UInt16) {
                setEnv(e)
                let x = ext(1), three = ext(3)
                #expect(run(pea(three) + pea(x) + op(0x0006) + [0xA9F4]) == "ExitToShell")
                let r = env
                setEnv(0)
                let (se, m) = bits(x)
                return (se, m, r)
            }
            let near = third(0)
            #expect(near.0 == 0x3FFD && near.1 == 0xAAAA_AAAA_AAAA_AAAB)
            #expect(near.2 == 0x1000) // inexact
            #expect(third(0x6000).1 == 0xAAAA_AAAA_AAAA_AAAA) // toward zero
            #expect(third(0x2000).1 == 0xAAAA_AAAA_AAAA_AAAB) // upward
            #expect(third(0x0020).1 == 0xAAAA_AAAA_AAAA_A800) // double precision
            #expect(third(0x0040).1 == 0xAAAA_AB00_0000_0000) // single precision
            // sqrt(2) to 64 bits, and the exact remainder 7 REM 2 = -1, n = 4 in D0.
            let two = ext(2)
            let seven = ext(7), d = ext(2)
            #expect(run(pea(two) + op(0x0012) + pea(d) + pea(seven) + op(0x000C) + [0xA9F4]) == "ExitToShell")
            #expect(bits(two).1 == 0xB504_F333_F9DE_6484)
            #expect(double(seven) == -1 && Int16(bitPattern: UInt16(cpu.pointee.d.0 & 0xFFFF)) == 4)
            // An extended denormal holds its value (exponent field 0 is 2^-16382).
            let den = extBits(0x0000, 0x4000_0000_0000_0000), factor = ext(2)
            #expect(run(pea(factor) + pea(den) + op(0x0004) + [0xA9F4]) == "ExitToShell")
            #expect(bits(den) == (0x0001, 0x8000_0000_0000_0000))
        }

        /// Next-after changes its SRC operand, in the format given for both
        /// (a double stays eight bytes), and copy-sign gives SRC DST's sign.
        @Test func nextAfterAndCopySign() {
            let x = dbl(1), toward = dbl(2)
            wr16(x + 8, 0x5A5A)
            #expect(run(pea(x) + pea(toward) + op(0x0813) + [0xA9F4]) == "ExitToShell")
            #expect(dblBits(x) == 0x3FF0_0000_0000_0001 && rd16(x + 8) == 0x5A5A)
            #expect(dblBits(toward) == 2.0.bitPattern)
            let e = ext(1), down = ext(0)
            #expect(run(pea(e) + pea(down) + op(0x0013) + [0xA9F4]) == "ExitToShell")
            #expect(bits(e) == (0x3FFE, 0xFFFF_FFFF_FFFF_FFFF))
            let s = ext(3), negative = ext(-1)
            #expect(run(pea(s) + pea(negative) + op(0x0011) + [0xA9F4]) == "ExitToShell")
            #expect(double(s) == -3 && double(negative) == -1)
        }

        /// Class from the operand's own bits; integer conversions out of
        /// range give the most negative value and signal invalid; comp's
        /// NaN converts to a NaN with code 20.
        @Test func classAndIntegerConversions() {
            let snan = dbl(Double(bitPattern: 0x7FF0_0000_0000_0001)), denormal = dbl(Double.leastNonzeroMagnitude)
            let c1 = word(0), c2 = word(0)
            #expect(run(pea(snan) + pea(c1) + op(0x081C) + pea(denormal) + pea(c2) + op(0x081C) + [0xA9F4]) == "ExitToShell")
            #expect(rd16(c1) == 1 && rd16(c2) == 6)
            setEnv(0)
            let big = ext(40000), i = word(0)
            #expect(run(pea(big) + pea(i) + op(0x2010) + [0xA9F4]) == "ExitToShell")
            #expect(rd16(i) == 0x8000 && env & 0x0100 != 0)
            let nan = extBits(0x7FFF, 0x4000_0000_0000_0000), comp = dbl(0)
            #expect(run(pea(nan) + pea(comp) + op(0x3010) + [0xA9F4]) == "ExitToShell")
            #expect(dblBits(comp) == 0x8000_0000_0000_0000)
            let back = ext(0)
            #expect(run(pea(comp) + pea(back) + op(0x300E) + [0xA9F4]) == "ExitToShell")
            #expect(rd16(back) & 0x7FFF == 0x7FFF && (bits(back).1 >> 48) & 0xFF == 20)
            setEnv(0)
        }

        /// Exceptions by index, procedure entry and exit, and a halt: the
        /// halt procedure is called with the operation's operands and clears
        /// them from the stack, and the result is delivered as well.
        @Test func exceptionsAndHalts() {
            setEnv(0)
            let idx = word(1) // underflow
            let saved = word(0)
            #expect(run(pea(idx) + op(0x0015) + pea(saved) + op(0x0017) + [0xA9F4]) == "ExitToShell")
            #expect(rd16(saved) == 0x0200 && env == 0)
            let t = word(1)
            setEnv(0x0200)
            #expect(run(pea(t) + op(0x001B) + [0xA9F4]) == "ExitToShell")
            #expect(rd8(t) == 1)
            setEnv(0x0800) // divide-by-zero raised inside; exit merges it into the saved one
            let s2 = word(0x6000)
            #expect(run(pea(s2) + op(0x0019) + [0xA9F4]) == "ExitToShell")
            #expect(env == 0x6800)
            // The halt: MOVEA.L 18(SP),A1 (the MiscRec); MOVE.W (A1),marker
            // (the halting exceptions); MOVEA.L (SP)+,A0; ADDA.W #18,SP; JMP (A0).
            let marker = word(0)
            let halt = tb_NewPtr(32, false, true)
            for (i, w) in [UInt16(0x226F), 0x0012, 0x33D1, UInt16(marker >> 16), UInt16(marker & 0xFFFF),
                           0x205F, 0xDEFC, 0x0012, 0x4ED0].enumerated() { wr16(halt + UInt32(i * 2), w) }
            let vec = tb_NewPtr(4, false, true)
            wr32(vec, halt)
            setEnv(0x0008) // halt on divide-by-zero
            let x = ext(1), zero = ext(0)
            let code = [0x2E0F] + pea(vec) + op(0x0005) + pea(zero) + pea(x) + op(0x0006)
                + [0x9E8F, 0xA9F4] // SUB.L A7,D7
            #expect(run(code) == "ExitToShell")
            #expect(rd16(marker) == 0x0008)
            #expect(cpu.pointee.d.7 == 0)
            #expect(rd16(x) == 0x7FFF && env == 0x0808)
            wr32(0x0A4C, 0)
            setEnv(0)
        }

        /// Decimal to binary and back are exact before one rounding: 0.1 is
        /// extended's nearest, and gives back nineteen digits of it.
        @Test func decimalConversionsRoundOnce() {
            let dec = tb_NewPtr(26, false, true)
            wr8(dec, 0); wr16(dec + 2, UInt16(bitPattern: -1)); guestPString("1", at: dec + 4)
            let x = ext(0)
            let form = tb_NewPtr(4, false, true)
            wr16(form, 0); wr16(form + 2, 19)
            let out = tb_NewPtr(26, false, true)
            #expect(run(pea(dec) + pea(x) + op(0x0009) + pea(form) + pea(x) + pea(out) + op(0x000B) + [0xA9F4]) == "ExitToShell")
            #expect(bits(x) == (0x3FFB, 0xCCCC_CCCC_CCCC_CCCD))
            #expect(pstring(at: out + 4) == "1000000000000000000" && Int16(bitPattern: rd16(out + 2)) == -19)
            // 1e-4940 is a denormal in extended; 1e5000 overflows.
            wr16(dec + 2, UInt16(bitPattern: -4940))
            let tiny = ext(1)
            setEnv(0)
            #expect(run(pea(dec) + pea(tiny) + op(0x0009) + [0xA9F4]) == "ExitToShell")
            #expect(rd16(tiny) == 0 && bits(tiny).1 != 0 && env & 0x0200 != 0)
            wr16(dec + 2, 5000)
            #expect(run(pea(dec) + pea(tiny) + op(0x0009) + [0xA9F4]) == "ExitToShell")
            #expect(bits(tiny) == (0x7FFF, 0) && env & 0x0400 != 0)
            setEnv(0)
        }

        /// The elementary functions: powers, the financial ones (annuity at
        /// a zero rate is n) and logs over extended's range.
        @Test func elementaryFunctions() {
            let x = ext(1.5), i = word(3)
            let base = ext(2), y = ext(0.5)
            let rate = ext(0.1), n = ext(2), r = ext(0)
            let rate0 = ext(0), n7 = ext(7), r0 = ext(0)
            let big = extBits(0x7FFE, 0x8000_0000_0000_0000) // 2^16383
            let code = pea(i) + pea(x) + op(0x8010, 0xA9EC)
                + pea(y) + pea(base) + op(0x8012, 0xA9EC)
                + pea(rate) + pea(n) + pea(r) + op(0xC016, 0xA9EC)
                + pea(rate0) + pea(n7) + pea(r0) + op(0xC016, 0xA9EC)
                + pea(big) + op(0x0002, 0xA9EC)
                + [0xA9F4]
            #expect(run(code) == "ExitToShell")
            #expect(double(x) == 3.375)
            #expect(abs(double(base) - 2.0.squareRoot()) < 1e-15)
            #expect(abs(double(r) - 1.7355371900826446) < 1e-14)
            #expect(double(r0) == 7)
            #expect(double(big) == 16383)
        }

        /// With double rounding precision, FP68K's arithmetic on doubles
        /// gives IEEE double results bit for bit, in each rounding
        /// direction; the remainder is exact; decimal conversions of
        /// seventeen digits agree with the host's.
        @Test func doubleResultsMatchIEEE() {
            func fp(_ op: UInt16, _ args: [UInt32]) {
                for a in args { cpu_push32(cpu, a) }
                cpu_push16(cpu, op)
                cpu_push32(cpu, 0)
                raw_FP68K(cpu, 0xA9EB)
                _ = cpu_pop32(cpu)
            }
            cpu.pointee.a.7 = tb_NewPtr(4096, false, true) + 4000
            var rng = SystemRandomNumberGenerator()
            func random() -> Double {
                let e = Int.random(in: -1080...1023, using: &rng)
                let m = 1 + Double.random(in: 0..<1, using: &rng)
                let v = Swift.abs(e) < 1070 ? m * pow(2, Double(e)) : Double(bitPattern: UInt64.random(in: 1..<(1 << 52), using: &rng))
                return Bool.random(using: &rng) ? -v : v
            }
            let a = dbl(0), b = dbl(0), x = ext(0), r = dbl(0), back = dbl(0)
            let form = tb_NewPtr(4, false, true), dec = tb_NewPtr(26, false, true)
            let modes: [(UInt16, Int32)] = [(0, FE_TONEAREST), (0x2000, FE_UPWARD), (0x4000, FE_DOWNWARD), (0x6000, FE_TOWARDZERO)]
            var bad = 0
            for _ in 0..<2000 {
                let u = random(), v = random()
                wr32(a, UInt32(u.bitPattern >> 32)); wr32(a + 4, UInt32(u.bitPattern & 0xFFFF_FFFF))
                wr32(b, UInt32(v.bitPattern >> 32)); wr32(b + 4, UInt32(v.bitPattern & 0xFFFF_FFFF))
                for (env, host) in modes {
                    for (code, f) in [(UInt16(0), { (p: Double, q: Double) in p + q }), (2, { $0 - $1 }), (4, { $0 * $1 }),
                                      (6, { $0 / $1 }), (0x12, { p, _ in p.squareRoot() })] {
                        setEnv(env | 0x0020)
                        fp(0x080E, [a, x]) // x = u
                        if code == 0x12 { fp(0x0012, [x]) } else { fp(0x0800 | code, [b, x]) }
                        fp(0x0810, [x, r])
                        fesetround(host)
                        let want = f(u, v)
                        fesetround(FE_TONEAREST)
                        if dblBits(r) != want.bitPattern && !(want.isNaN && Double(bitPattern: dblBits(r)).isNaN) {
                            if bad < 5 { print("SANE mismatch op \(code) env \(env): \(u) \(v) -> \(Double(bitPattern: dblBits(r))) want \(want)") }
                            bad += 1
                        }
                    }
                }
                setEnv(0)
                fp(0x080E, [a, x])
                fp(0x080C, [b, x])
                fp(0x0810, [x, r])
                if dblBits(r) != remainder(u, v).bitPattern { bad += 1 }
                // Seventeen digits out and back.
                wr16(form, 0); wr16(form + 2, 17)
                fp(0x080B, [form, a, dec])
                let digits = pstring(at: dec + 4), exp = Int(Int16(bitPattern: rd16(dec + 2)))
                let text = String(format: "%.16e", Swift.abs(u))
                let mant = text.prefix(while: { $0 != "e" }).replacingOccurrences(of: ".", with: "")
                let e10 = Int(text.split(separator: "e")[1])! - 16
                if digits != mant || exp != e10 { bad += 1 }
                fp(0x0809, [dec, back])
                if dblBits(back) != u.bitPattern { bad += 1 }
            }
            setEnv(0)
            #expect(bad == 0)
        }

        /// The elementary functions agree with the host's to about a
        /// double's precision, and reach past a double's range: e^11000 and
        /// 2^-16000 are extended numbers.
        @Test func elementaryFunctionsMatchTheHost() {
            func el(_ op: UInt16, _ v: Double) -> Double {
                let x = ext(v)
                cpu_push32(cpu, x)
                cpu_push16(cpu, op)
                cpu_push32(cpu, 0)
                raw_Elems68K(cpu, 0xA9EC)
                _ = cpu_pop32(cpu)
                defer { tb_DisposePtr(x) }
                return double(x)
            }
            cpu.pointee.a.7 = tb_NewPtr(4096, false, true) + 4000
            let fns: [(UInt16, (Double) -> Double, ClosedRange<Double>)] = [
                (0x00, log, 1e-300...1e300), (0x02, log2, 1e-300...1e300), (0x04, log1p, -0.9...1e10),
                (0x08, exp, -700...700), (0x0A, exp2, -1000...1000), (0x0C, expm1, -40...40),
                (0x18, sin, -1000...1000), (0x1A, cos, -1000...1000), (0x1C, tan, -1.5...1.5), (0x1E, atan, -1e6...1e6),
            ]
            var worst = 0.0
            for (op, f, range) in fns {
                for _ in 0..<300 {
                    let v = Double.random(in: range)
                    let want = f(v), got = el(op, v)
                    // Near a zero of sin or cos the extended pi SANE reduces by
                    // (64 bits) shows: an absolute error, not a relative one.
                    let scale = op >= 0x18 && op <= 0x1A ? max(Swift.abs(want), 0.1) : Swift.abs(want)
                    worst = max(worst, Swift.abs(got - want) / max(scale, 1e-300))
                }
            }
            #expect(worst < 1e-14)
            let big = ext(11000)
            cpu_push32(cpu, big); cpu_push16(cpu, 0x0008); cpu_push32(cpu, 0)
            raw_Elems68K(cpu, 0xA9EC)
            _ = cpu_pop32(cpu)
            // e^11000 = 2^15869.6...: exponent field 16383 + 15869
            #expect(rd16(big) == 16383 + 15869)
            let small = ext(-16000)
            cpu_push32(cpu, small); cpu_push16(cpu, 0x000A); cpu_push32(cpu, 0)
            raw_Elems68K(cpu, 0xA9EC)
            _ = cpu_pop32(cpu)
            #expect(rd16(small) == 16383 - 16000 && rd32(small + 2) == 0x8000_0000 && rd32(small + 6) == 0)
        }

        @Test func unknownOperationHalts() {
            #expect(run(op(0x7F44) + [0xA9F4]) == "FP68K: unknown operation 7F44")
        }

        @Test func floatingPointCoprocessor() {
            let x = tb_NewPtr(8, false, true)
            let code: [UInt16] = [
                0xF23C, 0x4000, 0x0000, 0x0003,   // FMOVE.L #3,FP0
                0xF23C, 0x4022, 0x0000, 0x0004,   // FADD.L #4,FP0
                0xF201, 0x6000,                   // FMOVE.L FP0,D1
                0xF23C, 0x4038, 0x0000, 0x0007,   // FCMP.L #7,FP0
                0xF281, 0x0004,                   // FBEQ.W +4 (over the next instruction)
                0x7401,                           // MOVEQ #1,D2 (skipped)
                0xF23C, 0x4080, 0x0000, 0x0002,   // FMOVE.L #2,FP1
                0xF200, 0x00A0,                   // FDIV.X FP0,FP1 → 2/7
                0xF239, 0x7480, UInt16(x >> 16), UInt16(x & 0xFFFF), // FMOVE.D FP1,x
                0xA9F4,
            ]
            #expect(run(code) == "ExitToShell")
            #expect(cpu.pointee.d.1 == 7)
            #expect(cpu.pointee.d.2 == 0)
            let bits = UInt64(rd32(x)) << 32 | UInt64(rd32(x + 4))
            #expect(abs(Double(bitPattern: bits) - 2.0 / 7.0) < 1e-12)
        }

        /// Packed decimal: stored with a k-factor in a register or the
        /// instruction (LoanCalc formats numbers so), and read back.
        @Test func packedDecimal() {
            let p = tb_NewPtr(12, false, true), q = tb_NewPtr(12, false, true)
            let code: [UInt16] = [
                0x207C, UInt16(p >> 16), UInt16(p & 0xFFFF),   // MOVEA.L #p,A0
                0x227C, UInt16(q >> 16), UInt16(q & 0xFFFF),   // MOVEA.L #q,A1
                0xF23C, 0x4000, 0x0000, 0x3039,   // FMOVE.L #12345,FP0
                0xF23C, 0x4020, 0x0000, 0x0064,   // FDIV.L #100,FP0 → 123.45
                0x7405,                           // MOVEQ #5,D2
                0xF210, 0x7C20,                   // FMOVE.P FP0,(A0){D2}
                0xF23C, 0x4080, 0xFFFF, 0xFFFB,   // FMOVE.L #-5,FP1
                0xF23C, 0x40A0, 0x0000, 0x0080,   // FDIV.L #128,FP1 → -0.0390625
                0xF211, 0x6C83,                   // FMOVE.P FP1,(A1){#3}
                0xF210, 0x4D00,                   // FMOVE.P (A0),FP2
                0xF23C, 0x4123, 0x0000, 0x0064,   // FMUL.L #100,FP2
                0xF203, 0x6100,                   // FMOVE.L FP2,D3
                0xA9F4,
            ]
            #expect(run(code) == "ExitToShell")
            #expect(rd32(p) == 0x0002_0001 && rd32(p + 4) == 0x2345_0000 && rd32(p + 8) == 0) // +1.2345E+2
            #expect(rd32(q) == 0xC002_0003 && rd32(q + 4) == 0x9100_0000)                      // -3.91E-2
            #expect(cpu.pointee.d.3 == 12345)
        }

        /// FSAVE pushes the 68040's 4-byte idle frame and FRESTORE takes back
        /// as much: an interrupt routine that saves the FPU around its work
        /// (Safari's sound callback, with FMOVEM of all eight registers)
        /// returns with its stack where it was.
        @Test func fsaveAndFrestoreBalance() {
            let code: [UInt16] = [
                0x2E0F,                           // MOVE.L A7,D7
                0xF327,                           // FSAVE -(A7)
                0x1C17,                           // MOVE.B (A7),D6: the frame's version
                0xF227, 0xE0FF,                   // FMOVEM.X FP0-FP7,-(A7)
                0xF21F, 0xD0FF,                   // FMOVEM.X (A7)+,FP0-FP7
                0xF35F,                           // FRESTORE (A7)+
                0x9E8F,                           // SUB.L A7,D7
                0xA9F4,
            ]
            #expect(run(code) == "ExitToShell")
            #expect(cpu.pointee.d.7 == 0)
            #expect(cpu.pointee.d.6 & 0xFF == 0x41)
        }
    }
}

extension BlueBox {
    /// ReplaceText takes a Str15 key by the book, but programs pass longer
    /// strings (T-Minus Ten); the host's copy used to overflow its buffer.
    @Suite struct ReplaceTextKey {
        init() { bootMachine() }

        @Test func longKeyIsReplaced() {
            func handle(_ s: String) -> UInt32 {
                let h = tb_NewHandle(Int32(s.utf8.count), false, true)
                for (i, b) in s.utf8.enumerated() { wr8(rd32(h) + UInt32(i), b) }
                return h
            }
            let key = "0123456789abcdefghij" // 20 bytes
            let k = tb_NewPtr(256, false, true)
            wr8(k, UInt8(key.utf8.count))
            for (i, b) in key.utf8.enumerated() { wr8(k + 1 + UInt32(i), b) }
            let base = handle("x" + key + "y")
            let subst = handle("--")
            #expect(tb_ReplaceText(base, subst, k) == 1)
            #expect(tb_GetHandleSize(base) == 4)
        }
    }
}
