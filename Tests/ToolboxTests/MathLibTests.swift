import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// MathLib's long double (double-double) routines, classification, the
    /// floating-point environment and the decimal conversions, called as a
    /// PowerPC caller would: integers and pointers in r3 on (past r10 in the
    /// parameter area at 24(r1)), floating point in f1 on, a long double in
    /// two FPRs (head, tail) and four GPR slots, its result in f1 and f2.
    @Suite struct MathLib {
        typealias LD = (hi: Double, lo: Double)

        /// Call an export; r3 after.
        @discardableResult
        func call(_ name: String, _ args: [UInt32] = [], fp: [Double] = []) -> UInt32 {
            let stack = tb_NewPtr(256, true, true)
            withUnsafeMutableBytes(of: &g_ppc.gpr) { $0.storeBytes(of: stack, toByteOffset: 4, as: UInt32.self) }
            for (i, a) in args.enumerated() {
                if i < 8 {
                    withUnsafeMutableBytes(of: &g_ppc.gpr) { $0.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
                } else {
                    wr32(stack + 24 + UInt32(4 * i), a)
                }
            }
            for (i, f) in fp.enumerated() { ppc_set_fpr(&g_ppc, Int32(1 + i), f) }
            guard let fn = cfm_export_fn(name) else {
                Issue.record("\(name) is not exported")
                return 0xFFFF_FFFF
            }
            fn(&g_ppc)
            tb_DisposePtr(stack)
            return g_ppc.gpr.3
        }

        /// A long double function of long doubles (and integer arguments
        /// after them, from GPR slot 4 per long double on).
        func ld(_ name: String, _ xs: [LD], _ after: [UInt32] = []) -> LD {
            let slots = [UInt32](repeating: 0, count: 4 * xs.count) + after
            call(name, slots, fp: xs.flatMap { [$0.hi, $0.lo] })
            return (ppc_fpr(&g_ppc, 1), ppc_fpr(&g_ppc, 2))
        }

        func f1() -> Double { ppc_fpr(&g_ppc, 1) }

        func rdDouble(_ p: UInt32) -> Double { Double(bitPattern: UInt64(rd32(p)) << 32 | UInt64(rd32(p + 4))) }

        func cstr(_ s: String) -> UInt32 {
            let b = Array(s.utf8) + [0]
            let p = tb_NewPtr(Int32(b.count), false, true)
            for (i, x) in b.enumerated() { wr8(p + UInt32(i), x) }
            return p
        }

        /// A decimal record: sgn, exp, sig.
        func decimal(_ negative: Bool, _ exp: Int16, _ sig: String) -> UInt32 {
            let d = tb_NewPtr(42, true, true)
            wr8(d, negative ? 1 : 0)
            wr16(d + 2, UInt16(bitPattern: exp))
            wr8(d + 4, UInt8(sig.utf8.count))
            for (i, ch) in sig.utf8.enumerated() { wr8(d + 5 + UInt32(i), ch) }
            return d
        }

        func readDecimal(_ d: UInt32) -> (negative: Bool, exp: Int16, sig: String) {
            let n = Int(rd8(d + 4))
            let sig = String(decoding: (0..<n).map { rd8(d + 5 + UInt32($0)) }, as: UTF8.self)
            return (rd8(d) != 0, Int16(bitPattern: rd16(d + 2)), sig)
        }

        func decform(_ fixed: Bool, _ digits: Int16) -> UInt32 {
            let f = tb_NewPtr(4, true, true)
            wr8(f, fixed ? 1 : 0)
            wr16(f + 2, UInt16(bitPattern: digits))
            return f
        }

        static let pi: LD = (Double.pi, 1.2246467991473532e-16)

        /// The precision a double-double keeps: the tail carries the bits
        /// past the head's 53.
        @Test func longDoubleArithmeticKeepsTheTail() {
            bootMachine()
            let r = ld("sqrtl", [(2, 0)])
            #expect(r.hi == 2.0.squareRoot())
            #expect(abs(r.lo - -9.667293313452913e-17) < 1e-31)
            let h = ld("hypotl", [(3, 0), (4, 0)])
            #expect(h.hi == 5 && h.lo == 0)
            let big = ld("hypotl", [(3e300, 0), (4e300, 0)])
            #expect(abs(big.hi - 5e300) <= 5e300 * 1e-15)
            // Computed on the value as a double, the tail 0.
            let e = ld("expl", [(1, 0)])
            #expect(e.hi == exp(1.0) && e.lo == 0)
            let a = ld("atan2l", [(1, 0), (1, 0)])
            #expect(a.hi == Double.pi / 4)
            let p = ld("powl", [(2, 0), (10, 0)])
            #expect(p.hi == 1024)
            let s = ld("fabsl", [(-1, -0x1p-60)])
            #expect(s.hi == 1 && s.lo == 0x1p-60)
            let cs = ld("copysignl", [(1, 0x1p-60), (-2, 0)])
            #expect(cs.hi == -1 && cs.lo == -0x1p-60)
        }

        @Test func longDoubleRounding() {
            bootMachine()
            g_ppc.fpscr = 0
            let big: LD = (0x1p53, 0.5) // 2^53 + 1/2
            #expect(ld("floorl", [big]) == (0x1p53, 0))
            #expect(ld("ceill", [big]) == (0x1p53, 1))
            #expect(ld("rintl", [big]) == (0x1p53, 0)) // halfway: to even
            #expect(ld("roundl", [big]) == (0x1p53, 1))
            let under: LD = (1, -0x1p-60) // just under 1
            #expect(ld("floorl", [under]) == (0, 0))
            #expect(ld("ceill", [under]) == (1, 0))
            #expect(ld("truncl", [under]) == (0, 0))
            #expect(ld("roundl", [under]) == (1, 0))
            #expect(ld("rintl", [(0.5, 0x1p-60)]) == (1, 0))
            #expect(ld("rintl", [(0.5, -0x1p-60)]) == (0, 0))
            let negZero = ld("truncl", [(-0.5, 0)])
            #expect(negZero.hi == 0 && negZero.hi.sign == .minus)
            // rintl and rinttoll in the rounding direction.
            #expect(call("fesetround", [2]) == 1) // FE_UPWARD
            #expect(ld("nearbyintl", [(1, 0x1p-60)]) == (2, 0))
            #expect(call("rinttoll", [], fp: [1, 0x1p-60]) == 2)
            call("fesetround", [0])
            #expect(call("rinttoll", [], fp: [2.5, 0]) == 2)
            #expect(call("roundtoll", [], fp: [2.5, 0]) == 3)
            #expect(Int32(bitPattern: call("roundtoll", [], fp: [-2.5, 0])) == -3)
            #expect(call("rinttoll", [], fp: [1e10, 0]) == 0x7FFF_FFFF)
            #expect(call("rinttoll", [], fp: [Double.nan, 0]) == 0x8000_0000)
        }

        @Test func longDoubleScalingAndParts() {
            bootMachine()
            let e = tb_NewPtr(4, true, true)
            // A head of 1/2 after frexp with a tail below it: the fraction is
            // still in [1/2, 1).
            let m = ld("frexpl", [(1, -0x1p-60)], [e])
            #expect(m == (1, -0x1p-60) && rd32(e) == 0)
            let m2 = ld("frexpl", [(48, 0x1p-56)], [e])
            #expect(m2 == (0.75, 0x1p-62) && rd32(e) == 6)
            #expect(ld("ldexpl", [(3, 0x1p-60)], [4]) == (48, 0x1p-56))
            #expect(ld("scalbl", [(3, 0x1p-60)], [UInt32(bitPattern: -2)]) == (0.75, 0x1p-62))
            let ip = tb_NewPtr(16, true, true)
            let f = ld("modfl", [(0x1p53, 0.5)], [ip])
            #expect(f == (0.5, 0) && rdDouble(ip) == 0x1p53 && rdDouble(ip + 8) == 0)
            #expect(ld("logbl", [(8, 0)]).hi == 3)
            #expect(ld("logbl", [(8, -0x1p-60)]).hi == 2)
            #expect(ld("nextafterl", [(1, 0), (2, 0)]) == (1, 0x1p-106))
            #expect(ld("nextafterl", [(1, 0), (0, 0)]) == (1, -0x1p-106))
            #expect(ld("nextafterl", [(0, 0), (1, 0)]).hi == Double.leastNonzeroMagnitude)
        }

        @Test func longDoubleComparisonAndRemainder() {
            bootMachine()
            // relop: GREATERTHAN 0, LESSTHAN 1, EQUALTO 2, UNORDERED 3.
            #expect(call("relationl", [], fp: [1, 0x1p-60, 1, 0]) == 0)
            #expect(call("relationl", [], fp: [1, 0, 1, 0x1p-60]) == 1)
            #expect(call("relationl", [], fp: [1, 0, 1, 0]) == 2)
            #expect(call("relationl", [], fp: [.nan, 0, 1, 0]) == 3)
            #expect(call("relation", [], fp: [1, 2]) == 1)
            #expect(call("relation", [], fp: [2, 2]) == 2)
            #expect(ld("fmaxl", [(1, 0x1p-60), (1, 0)]) == (1, 0x1p-60))
            #expect(ld("fminl", [(.nan, 0), (3, 0)]) == (3, 0))
            #expect(ld("fdiml", [(1, 0), (3, 0)]) == (0, 0))
            #expect(ld("fdiml", [(3, 0x1p-55), (1, 0)]) == (2, 0x1p-55))
            #expect(ld("remainderl", [(10, 0), (3, 0)]) == (1, 0))
            // remquol's quo pointer is the ninth argument word.
            let q = tb_NewPtr(4, true, true)
            let r = ld("remquol", [(-10, 0), (3, 0)], [q])
            #expect(r == (-1, 0) && Int32(bitPattern: rd32(q)) == -3)
            let r2 = ld("remquol", [(1, 0x1p-60), (0.5, 0)], [q])
            #expect(r2 == (0x1p-60, 0) && rd32(q) == 2)
            _ = ld("remquol", [(1000, 0), (1, 0)], [q])
            #expect(rd32(q) == 1000 % 128) // seven low bits
        }

        @Test func classificationAndNaNs() {
            bootMachine()
            let snan = Double(bitPattern: 0x7FF0_0000_0000_0001)
            #expect(call("__fpclassify", [], fp: [snan, 0]) == 0)
            #expect(call("__fpclassify", [], fp: [.nan, 0]) == 1)
            #expect(call("__fpclassify", [], fp: [.infinity, 0]) == 2)
            #expect(call("__fpclassify", [], fp: [0, 0]) == 3)
            #expect(call("__fpclassify", [], fp: [1, 0x1p-60]) == 4)
            #expect(call("__fpclassify", [], fp: [5e-324, 0]) == 5)
            #expect(call("__isfinite", [], fp: [1, 0]) == 1)
            #expect(call("__isfinite", [], fp: [.infinity, 0]) == 0)
            #expect(call("__isnormal", [], fp: [5e-324, 0]) == 0)
            #expect(call("__signbit", [], fp: [-0.0, 0]) == 1)
            // A NaN's code is the 8th to 15th bits of its fraction.
            call("nan", [cstr("33")])
            #expect(f1().bitPattern == 0x7FF8_0000_0000_0000 | 33 << 37)
            call("nan", [cstr("")])
            #expect(f1().bitPattern == 0x7FF8_0000_0000_0000 | 21 << 37)
            call("nanf", [cstr("36")])
            #expect(Float(f1()).bitPattern == 0x7FC0_0000 | 36 << 8)
            let l = ld("nanl", [], [cstr("255")])
            #expect(l.hi.bitPattern == 0x7FF8_0000_0000_0000 | 255 << 37 && l.lo == 0)
            // Floats: a float argument takes one GPR slot.
            let ip = tb_NewPtr(4, true, true)
            call("modff", [0, ip], fp: [2.75])
            #expect(f1() == 0.75 && Float(bitPattern: rd32(ip)) == 2)
            call("nextafterf", [], fp: [1, 2])
            #expect(f1() == Double(Float(1).nextUp))
        }

        /// fenv_t is the FPSCR; the flags are its sticky bits.
        @Test func environment() {
            bootMachine()
            let FE_INVALID: UInt32 = 0x2000_0000, FE_OVERFLOW: UInt32 = 0x1000_0000, FE_ALL: UInt32 = 0x3E00_0000
            g_ppc.fpscr = 0
            #expect(call("fesetround", [3]) == 1)
            #expect(call("fegetround") == 3)
            #expect(call("fesetround", [7]) == 0)
            #expect(call("fegetround") == 3)
            call("feraiseexcept", [FE_INVALID | FE_OVERFLOW])
            #expect(call("fetestexcept", [FE_ALL]) == FE_INVALID | FE_OVERFLOW)
            let flag = tb_NewPtr(4, true, true)
            call("fegetexcept", [flag, FE_INVALID])
            #expect(rd32(flag) == FE_INVALID)
            call("feclearexcept", [FE_ALL])
            #expect(call("fetestexcept", [FE_ALL]) == 0)
            call("fesetexcept", [flag, FE_ALL])
            #expect(call("fetestexcept", [FE_ALL]) == FE_INVALID)
            // An invalid operation the processor flagged (VXCVI) counts.
            call("feclearexcept", [FE_ALL])
            g_ppc.fpscr |= 0x100
            #expect(call("fetestexcept", [FE_INVALID]) == FE_INVALID)
            // Hold, raise, update: the held flags come back with the new.
            let env = tb_NewPtr(4, true, true)
            #expect(call("feholdexcept", [env]) == 0)
            #expect(call("fetestexcept", [FE_ALL]) == 0)
            call("feraiseexcept", [FE_OVERFLOW])
            call("feupdateenv", [env])
            #expect(call("fetestexcept", [FE_ALL]) == FE_INVALID | FE_OVERFLOW)
            #expect(call("fegetround") == 3)
            // FE_DFL_ENV: round to nearest, no flags.
            let dfl = ppc_data_export("_FE_DFL_ENV")
            #expect(dfl != 0 && ppc_data_export_known("_FE_DFL_ENV"))
            call("fesetenv", [dfl])
            #expect(g_ppc.fpscr == 0)
            call("fegetenv", [env])
            #expect(rd32(env) == 0)
        }

        @Test func decimalConversions() {
            bootMachine()
            g_ppc.fpscr = 0
            // 32 digits of pi to a long double: head and tail both right.
            let p = ld("dec2numl", [], [decimal(false, -31, "31415926535897932384626433832795")])
            #expect(p.hi == Self.pi.hi && abs(p.lo - Self.pi.lo) < 1e-31)
            let m = ld("dec2numl", [], [decimal(true, 0, "I")])
            #expect(m.hi == -.infinity && m.lo == 0)
            #expect(ld("dec2numl", [], [decimal(false, 2, "0913")]) == (0, 0))
            let n1 = ld("dec2numl", [], [decimal(false, 0, "N21")])
            let n2 = ld("dec2numl", [], [decimal(false, 0, "N4021000000000000")])
            #expect(n1.hi.bitPattern == 0x7FF8_0000_0000_0000 | 0x21 << 37)
            #expect(n2.hi.bitPattern == n1.hi.bitPattern)
            // A float halfway between two from its head alone; the digits
            // past it decide.
            call("dec2f", [decimal(false, 0, "16777217")])
            #expect(f1() == 16_777_216)
            call("dec2f", [decimal(false, -22, "167772170000000000000000000001")])
            #expect(f1() == 16_777_218)
            #expect(Int16(truncatingIfNeeded: call("dec2s", [decimal(false, 0, "40000")])) == 32767)
            #expect(Int16(truncatingIfNeeded: call("dec2s", [decimal(true, 0, "123")])) == -123)
            #expect(call("dec2l", [decimal(false, -1, "25")]) == 2)
            #expect(call("dec2l", [decimal(false, -1, "35")]) == 4)
            // num2decl: f in r3, x in f1/f2 (slots 1 to 4), d in r8.
            let d = tb_NewPtr(42, true, true)
            call("num2decl", [decform(false, 30), 0, 0, 0, 0, d], fp: [Self.pi.hi, Self.pi.lo])
            #expect(readDecimal(d) == (false, -29, "314159265358979323846264338328"))
            call("num2decl", [decform(true, 2), 0, 0, 0, 0, d], fp: [-1234.5678, 0])
            #expect(readDecimal(d) == (true, -2, "123457"))
            call("num2decl", [decform(true, -2), 0, 0, 0, 0, d], fp: [1234.5678, 0])
            #expect(readDecimal(d) == (false, 2, "12"))
            call("num2decl", [decform(true, 2), 0, 0, 0, 0, d], fp: [1e40, 0])
            #expect(readDecimal(d).sig == "?")
            call("num2decl", [decform(false, 5), 0, 0, 0, 0, d], fp: [0, 0])
            #expect(readDecimal(d).sig == "0")
            call("num2decl", [decform(false, 5), 0, 0, 0, 0, d], fp: [Double(bitPattern: 0x7FF8_0000_0000_0000 | 0x21 << 37), 0])
            #expect(readDecimal(d).sig == "N4021000000000000")
            call("num2decl", [decform(false, 3), 0, 0, 0, 0, d], fp: [9.9996, 0])
            #expect(readDecimal(d) == (false, -1, "100"))
            // Round trip of a long double's 0.1.
            let tenth = ld("dec2numl", [], [decimal(false, -1, "1")])
            call("num2decl", [decform(false, 31), 0, 0, 0, 0, d], fp: [tenth.hi, tenth.lo])
            #expect(readDecimal(d) == (false, -31, "1000000000000000000000000000000"))
        }
    }
}
