import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// AltiVec, the G4's vector unit (ppc_vec.c): every register instruction
    /// against a reference model (Tests/PPC/gen_altivec.py), and hand-checked
    /// cases for the loads and stores, the status register, saturation,
    /// NaNs and denormals, the G3/G4 choice, the disassembler and the JIT.
    @Suite struct AltiVec {
        static let data: UInt32 = 0x6000_0000
        static let codeAt: UInt32 = 0x5000_0000
        static let returnAt: UInt32 = 0x5000_F000

        // ---- Helpers

        static func bytes(_ hex: String) -> [UInt8] {
            var out = [UInt8]()
            var i = hex.startIndex
            while i < hex.endIndex {
                let j = hex.index(i, offsetBy: 2)
                out.append(UInt8(hex[i..<j], radix: 16)!)
                i = j
            }
            return out
        }

        /// A vector from its four words, element 0 first.
        static func words(_ w: [UInt32]) -> [UInt8] { w.flatMap { [UInt8($0 >> 24), UInt8($0 >> 16 & 0xFF), UInt8($0 >> 8 & 0xFF), UInt8($0 & 0xFF)] } }
        static func halves(_ h: [UInt16]) -> [UInt8] { h.flatMap { [UInt8($0 >> 8), UInt8($0 & 0xFF)] } }

        static func setV(_ c: inout ppc_cpu, _ n: Int, _ v: [UInt8]) {
            withUnsafeMutableBytes(of: &c.vr) { p in for (i, b) in v.enumerated() { p[n * 16 + i] = b } }
        }

        static func getV(_ c: ppc_cpu, _ n: Int) -> [UInt8] {
            withUnsafeBytes(of: c.vr) { Array($0[(n * 16)..<(n * 16 + 16)]) }
        }

        static func getW(_ c: ppc_cpu, _ n: Int) -> [UInt32] {
            let b = getV(c, n)
            return (0..<4).map { (i: Int) -> UInt32 in
                let hi = UInt32(b[4 * i]) << 24 | UInt32(b[4 * i + 1]) << 16
                return hi | UInt32(b[4 * i + 2]) << 8 | UInt32(b[4 * i + 3])
            }
        }

        static func setR(_ c: inout ppc_cpu, _ n: Int, _ v: UInt32) {
            withUnsafeMutableBytes(of: &c.gpr) { $0.storeBytes(of: v, toByteOffset: n * 4, as: UInt32.self) }
        }

        static func getR(_ c: ppc_cpu, _ n: Int) -> UInt32 {
            withUnsafeBytes(of: c.gpr) { $0.load(fromByteOffset: n * 4, as: UInt32.self) }
        }

        static func cpu() -> ppc_cpu {
            var c = ppc_cpu()
            ppc_init(&c)
            return c
        }

        // Encodings: VX, VA, VC (opcode 4) and X (opcode 31).
        static func vx(_ xo: UInt32, _ d: UInt32, _ a: UInt32, _ b: UInt32) -> UInt32 { 4 << 26 | d << 21 | a << 16 | b << 11 | xo }
        static func va(_ xo: UInt32, _ d: UInt32, _ a: UInt32, _ b: UInt32, _ c: UInt32) -> UInt32 {
            4 << 26 | d << 21 | a << 16 | b << 11 | c << 6 | xo
        }
        static func vc(_ xo: UInt32, _ d: UInt32, _ a: UInt32, _ b: UInt32, rc: Bool = false) -> UInt32 {
            4 << 26 | d << 21 | a << 16 | b << 11 | (rc ? 0x400 : 0) | xo
        }
        static func x31(_ xo: UInt32, _ d: UInt32, _ a: UInt32, _ b: UInt32) -> UInt32 { 31 << 26 | d << 21 | a << 16 | b << 11 | xo << 1 }

        static func run(_ c: inout ppc_cpu, _ op: UInt32) {
            ppc_execute(&c, op)
        }

        // ---- Every register instruction against the reference model

        @Test func everyInstructionMatchesTheReferenceModel() throws {
            ppc_altivec = true
            let url = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
                .appendingPathComponent("PPC/altivec_vectors.txt")
            let text = try String(contentsOf: url, encoding: .utf8)
            var cases = 0, bad: [String] = [], names = Set<String>()
            for line in text.split(separator: "\n") where !line.hasPrefix("#") {
                let f = line.split(separator: " ").map(String.init)
                // name op vA vB vC vscr -> vD vscr cr6
                guard f.count == 10 else { continue }
                var c = Self.cpu()
                c.cr = 0x1234_5678
                Self.setV(&c, 2, Self.bytes(f[2]))
                Self.setV(&c, 3, Self.bytes(f[3]))
                Self.setV(&c, 4, Self.bytes(f[4]))
                c.vscr = UInt32(f[5], radix: 16)!
                Self.run(&c, UInt32(f[1], radix: 16)!)
                cases += 1
                names.insert(f[0])
                let got = Self.getV(c, 1).map { String(format: "%02X", $0) }.joined()
                let cr6 = String(format: "%X", (c.cr >> 4) & 0xF)
                let crOK = f[9] == "-" ? c.cr == 0x1234_5678 : cr6 == f[9] && c.cr & ~0xF0 == 0x1234_5608
                if c.halted || got != f[7] || String(format: "%X", c.vscr) != f[8] || !crOK {
                    bad.append("\(f[0]) \(f[2]) \(f[3]) \(f[4]) vscr \(f[5]): got \(got) vscr \(String(format: "%X", c.vscr)) cr6 \(cr6), want \(f[7]) \(f[8]) \(f[9])")
                }
            }
            #expect(cases > 2000 && names.count == 156, "\(cases) cases of \(names.count) instructions")
            #expect(bad.isEmpty, "\(bad.count) of \(cases) differ:\n\(bad.prefix(25).joined(separator: "\n"))")
        }

        // ---- Loads and stores

        @Test func loadsAndStoresUseTheAlignedQuadword() {
            bootMachine()
            ppc_altivec = true
            for i in 0..<64 { wr8(Self.data + UInt32(i), UInt8(i)) }
            var c = Self.cpu()
            Self.setR(&c, 3, Self.data)
            Self.setR(&c, 4, 0x13) // unaligned: lvx takes the quadword at 0x10
            Self.run(&c, Self.x31(103, 1, 3, 4)) // lvx v1,r3,r4
            #expect(Self.getV(c, 1) == Array(16..<32).map(UInt8.init))
            Self.run(&c, Self.x31(359, 2, 3, 4)) // lvxl
            #expect(Self.getV(c, 2) == Self.getV(c, 1))
            // stvx to 0x2F stores at 0x20..0x2F.
            Self.setV(&c, 5, Array(repeating: 0xEE, count: 16))
            Self.setR(&c, 4, 0x2F)
            Self.run(&c, Self.x31(231, 5, 3, 4))
            let stored: [UInt8] = (0..<64).map { rd8(Self.data + UInt32($0)) }
            var expected: [UInt8] = (0..<32).map { UInt8($0) }
            expected += [UInt8](repeating: 0xEE, count: 16)
            expected += (48..<64).map { UInt8($0) }
            #expect(stored == expected)
            // rA = 0 means 0, not r0.
            Self.setR(&c, 0, 0xFFFF_0000)
            Self.setR(&c, 4, Self.data)
            Self.run(&c, Self.x31(103, 6, 0, 4))
            #expect(Self.getV(c, 6) == Array(0..<16).map(UInt8.init))
        }

        @Test func elementLoadsAndStores() {
            bootMachine()
            ppc_altivec = true
            for i in 0..<32 { wr8(Self.data + UInt32(i), UInt8(0xA0 + i)) }
            var c = Self.cpu()
            Self.setR(&c, 3, Self.data)
            Self.setR(&c, 4, 5)
            Self.run(&c, Self.x31(7, 1, 3, 4)) // lvebx: byte 5 is element 5
            #expect(Self.getV(c, 1)[5] == 0xA5)
            Self.setR(&c, 4, 7) // lvehx: the halfword at 6, element 3
            Self.run(&c, Self.x31(39, 2, 3, 4))
            #expect(Array(Self.getV(c, 2)[6..<8]) == [0xA6, 0xA7])
            Self.setR(&c, 4, 0x1B) // lvewx: the word at 0x18, element 2
            Self.run(&c, Self.x31(71, 3, 3, 4))
            #expect(Self.getW(c, 3)[2] == 0xB8B9_BABB)
            // Element stores write only the element.
            Self.setV(&c, 4, (0..<16).map { UInt8(0x10 + $0) })
            Self.setR(&c, 4, 3)
            Self.run(&c, Self.x31(135, 4, 3, 4)) // stvebx: element 3 to byte 3
            Self.setR(&c, 4, 9)
            Self.run(&c, Self.x31(167, 4, 3, 4)) // stvehx: element 4 (bytes 8, 9) to 8
            Self.setR(&c, 4, 0x1E)
            Self.run(&c, Self.x31(199, 4, 3, 4)) // stvewx: element 3 (bytes 12-15) to 0x1C
            let m = (0..<32).map { rd8(Self.data + UInt32($0)) }
            var want = (0..<32).map { UInt8(0xA0 + $0) }
            want[3] = 0x13
            want[8] = 0x18; want[9] = 0x19
            want[0x1C] = 0x1C; want[0x1D] = 0x1D; want[0x1E] = 0x1E; want[0x1F] = 0x1F
            #expect(m == want)
        }

        /// lvsl and lvsr for every alignment, and the unaligned load idiom
        /// (two lvx, lvsl, vperm) at each.
        @Test func lvslLvsrAndTheUnalignedLoad() {
            bootMachine()
            ppc_altivec = true
            for i in 0..<64 { wr8(Self.data + UInt32(i), UInt8(i * 3 & 0xFF)) }
            for sh in 0..<16 {
                var c = Self.cpu()
                Self.setR(&c, 3, Self.data + UInt32(sh))
                Self.setR(&c, 4, 0)
                Self.run(&c, Self.x31(6, 1, 0, 3)) // lvsl v1,0,r3
                Self.run(&c, Self.x31(38, 2, 0, 3)) // lvsr v2,0,r3
                #expect(Self.getV(c, 1) == (0..<16).map { UInt8(sh + $0) })
                #expect(Self.getV(c, 2) == (0..<16).map { UInt8(16 - sh + $0) })
                Self.setR(&c, 5, 15)
                Self.run(&c, Self.x31(103, 3, 0, 3)) // lvx v3,0,r3
                Self.run(&c, Self.x31(103, 4, 5, 3)) // lvx v4,r5,r3
                Self.run(&c, Self.va(43, 5, 3, 4, 1)) // vperm v5,v3,v4,v1
                #expect(Self.getV(c, 5) == (0..<16).map { UInt8((sh + $0) * 3 & 0xFF) }, "offset \(sh)")
            }
        }

        // ---- The status register, VRSAVE, the processor

        @Test func vscrSaturationIsStickyAndMovesThroughVectors() {
            ppc_altivec = true
            var c = Self.cpu()
            #expect(c.vscr == UInt32(PPC_VSCR_NJ))
            Self.setV(&c, 2, Array(repeating: 0xF0, count: 16))
            Self.setV(&c, 3, Array(repeating: 0x20, count: 16))
            Self.run(&c, Self.vx(512, 1, 2, 3)) // vaddubs: saturates
            #expect(Self.getV(c, 1) == Array(repeating: 0xFF, count: 16))
            #expect(c.vscr == 0x0001_0001)
            Self.setV(&c, 3, Array(repeating: 0x01, count: 16))
            Self.run(&c, Self.vx(512, 1, 3, 3)) // no saturation: SAT stays
            #expect(c.vscr & 1 == 1)
            Self.run(&c, Self.vx(1540, 7, 0, 0)) // mfvscr v7
            #expect(Self.getW(c, 7) == [0, 0, 0, 0x0001_0001])
            Self.setV(&c, 8, Self.words([0xFFFF_FFFF, 0xFFFF_FFFF, 0xFFFF_FFFF, 0x0000_0000]))
            Self.run(&c, Self.vx(1604, 0, 0, 8)) // mtvscr v8: only the last word counts
            #expect(c.vscr == 0)
            Self.setV(&c, 8, Self.words([0, 0, 0, 0xFFFF_FFFF]))
            Self.run(&c, Self.vx(1604, 0, 0, 8))
            #expect(c.vscr == 0x0001_0001)
        }

        @Test func vrsavePvrAndMsr() {
            var c = Self.cpu()
            ppc_altivec = true
            Self.setR(&c, 3, 0xFFFF_0000)
            Self.run(&c, 0x7C60_43A6) // mtspr 256,r3 (mtvrsave)
            Self.run(&c, 0x7C80_42A6) // mfspr r4,256
            #expect(c.vrsave == 0xFFFF_0000 && Self.getR(c, 4) == 0xFFFF_0000)
            Self.run(&c, 0x7CBF_42A6) // mfpvr r5
            #expect(Self.getR(c, 5) == 0x000C_0209)
            Self.run(&c, 0x7CC0_00A6) // mfmsr r6
            #expect(Self.getR(c, 6) & 0x0200_0000 != 0)
            ppc_altivec = false
            defer { ppc_altivec = true }
            Self.run(&c, 0x7CBF_42A6)
            Self.run(&c, 0x7CC0_00A6)
            #expect(Self.getR(c, 5) == 0x0008_0200 && Self.getR(c, 6) & 0x0200_0000 == 0)
        }

        /// On a G3 the vector instructions are illegal, as there.
        @Test func aG3HasNoVectorUnit() {
            ppc_altivec = false
            defer { ppc_altivec = true }
            for op in [Self.vx(0, 1, 2, 3), Self.x31(103, 1, 0, 3), Self.x31(342, 0, 3, 4)] {
                var c = Self.cpu()
                Self.run(&c, op)
                #expect(c.halted, "\(String(op, radix: 16))")
            }
        }

        @Test func gestaltSaysWhichProcessor() {
            bootMachine()
            var r: UInt32 = 0
            for g4 in [true, false] {
                ppc_altivec = g4
                gestalt_powerpc()
                #expect(tb_Gestalt(Int32(bitPattern: fourCC("cput")), &r) == 0 && r == (g4 ? 0x010C : 0x0108))
                #expect(tb_Gestalt(Int32(bitPattern: fourCC("cpuf")), &r) == 0 && r == (g4 ? 0x010C : 0x0108))
                #expect(tb_Gestalt(Int32(bitPattern: fourCC("ppcf")), &r) == 0 && (r >> 4 & 1 == 1) == g4 && r & 3 == 3)
            }
            ppc_altivec = true
            gestalt_powerpc()
        }

        // ---- Compares and CR6

        @Test func comparesSetCR6() {
            ppc_altivec = true
            var c = Self.cpu()
            Self.setV(&c, 2, Array(0..<16).map(UInt8.init))
            Self.setV(&c, 3, Array(0..<16).map(UInt8.init))
            Self.run(&c, Self.vc(6, 1, 2, 3, rc: true)) // vcmpequb.: all equal
            #expect(c.cr >> 4 & 0xF == 0x8 && Self.getV(c, 1) == Array(repeating: 0xFF, count: 16))
            Self.run(&c, Self.vc(518, 1, 2, 3, rc: true)) // vcmpgtub.: none greater
            #expect(c.cr >> 4 & 0xF == 0x2)
            Self.setV(&c, 3, Array(repeating: 7, count: 16))
            Self.run(&c, Self.vc(518, 1, 2, 3, rc: true)) // some
            #expect(c.cr >> 4 & 0xF == 0x0)
            // Without Rc the CR stays as it was.
            c.cr = 0xFFFF_FFFF
            Self.run(&c, Self.vc(518, 1, 2, 3))
            #expect(c.cr == 0xFFFF_FFFF)
            // vcmpbfp.: in bounds, out, and a NaN (both bits).
            Self.setV(&c, 2, Self.words([0x3F00_0000, 0xBF00_0000, 0x4000_0000, 0x7FC0_0000]))
            Self.setV(&c, 3, Self.words([0x3F80_0000, 0x3F80_0000, 0x3F80_0000, 0x3F80_0000]))
            Self.run(&c, Self.vc(966, 1, 2, 3, rc: true))
            #expect(Self.getW(c, 1) == [0, 0, 0x8000_0000, 0xC000_0000] && c.cr >> 4 & 0xF == 0)
            Self.setV(&c, 2, Self.words([0x3F00_0000, 0xBF00_0000, 0x3F80_0000, 0xBF80_0000]))
            Self.run(&c, Self.vc(966, 1, 2, 3, rc: true))
            #expect(Self.getW(c, 1) == [0, 0, 0, 0] && c.cr >> 4 & 0xF == 0x2)
        }

        // ---- Floating point: NaNs, denormals, fused multiply-add, estimates, conversions

        @Test func nansAndDenormals() {
            ppc_altivec = true
            var c = Self.cpu()
            // vaddfp: an operand NaN comes back quieted (vA's first); inf - inf is the default NaN.
            Self.setV(&c, 2, Self.words([0x7F80_0001, 0x3F80_0000, 0x7F80_0000, 0xFFC0_1234]))
            Self.setV(&c, 3, Self.words([0x7FC0_0005, 0xFF80_0002, 0xFF80_0000, 0x7F80_0009]))
            Self.run(&c, Self.vx(10, 1, 2, 3))
            #expect(Self.getW(c, 1) == [0x7FC0_0001, 0xFFC0_0002, 0x7FC0_0000, 0xFFC0_1234])
            // A denormal operand and result: kept in Java mode, zero (of its sign) in non-Java mode.
            Self.setV(&c, 2, Self.words([0x0000_0001, 0x8000_0003, 0x0080_0000, 0x0000_0000]))
            Self.setV(&c, 3, Self.words([0x0000_0000, 0x0000_0000, 0x8000_0001, 0x807F_FFFF]))
            c.vscr = 0
            Self.run(&c, Self.vx(10, 1, 2, 3))
            #expect(Self.getW(c, 1) == [0x0000_0001, 0x8000_0003, 0x007F_FFFF, 0x807F_FFFF])
            c.vscr = UInt32(PPC_VSCR_NJ)
            Self.run(&c, Self.vx(10, 1, 2, 3))
            #expect(Self.getW(c, 1) == [0, 0, 0x0080_0000, 0]) // +0 + -0 is +0
            // A normal result below the normal range is flushed too: 2^-126 * 0.5 by vmaddfp.
            Self.setV(&c, 2, Self.words([0x0080_0000, 0, 0, 0]))
            Self.setV(&c, 4, Self.words([0x3F00_0000, 0, 0, 0]))
            Self.setV(&c, 3, Self.words([0x8000_0000, 0, 0, 0]))
            Self.run(&c, Self.va(46, 1, 2, 3, 4))
            #expect(Self.getW(c, 1)[0] == 0)
            c.vscr = 0
            Self.run(&c, Self.va(46, 1, 2, 3, 4))
            #expect(Self.getW(c, 1)[0] == 0x0040_0000)
        }

        @Test func multiplyAddIsFused() {
            ppc_altivec = true
            var c = Self.cpu()
            // (1 + 2^-12)^2 - (1 + 2^-11) = 2^-24 rounded once; separately it would be 0.
            Self.setV(&c, 2, Self.words(Array(repeating: 0x3F80_0800, count: 4)))
            Self.setV(&c, 4, Self.words(Array(repeating: 0x3F80_0800, count: 4)))
            Self.setV(&c, 3, Self.words(Array(repeating: 0xBF80_1000, count: 4)))
            Self.run(&c, Self.va(46, 1, 2, 3, 4)) // vmaddfp v1,v2,v4,v3
            #expect(Self.getW(c, 1) == Array(repeating: 0x3380_0000, count: 4))
            // vnmsubfp: -(a*c - b); an exact zero comes out -0.
            Self.setV(&c, 3, Self.words(Array(repeating: 0x3F80_0800, count: 4)))
            Self.setV(&c, 4, Self.words(Array(repeating: 0x3F80_0000, count: 4)))
            Self.run(&c, Self.va(47, 1, 2, 3, 4))
            #expect(Self.getW(c, 1) == Array(repeating: 0x8000_0000, count: 4))
        }

        /// The estimates are within the manual's bounds (they are exact here,
        /// rounded to single), with its special cases.
        @Test func estimates() {
            ppc_altivec = true
            var c = Self.cpu()
            var seed: UInt32 = 0x1357_9BDF
            for _ in 0..<500 {
                var w = [UInt32]()
                for _ in 0..<4 {
                    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5
                    w.append(0x3000_0000 &+ (seed % 0x1E00_0000)) // positive normals, about 2^-31 to 2^29
                }
                Self.setV(&c, 3, Self.words(w))
                Self.run(&c, Self.vx(266, 1, 0, 3)) // vrefp
                Self.run(&c, Self.vx(330, 2, 0, 3)) // vrsqrtefp
                Self.run(&c, Self.vx(458, 4, 0, 3)) // vlogefp
                let r = Self.getW(c, 1), q = Self.getW(c, 2), l = Self.getW(c, 4)
                for i in 0..<4 {
                    let x = Double(Float(bitPattern: w[i]))
                    #expect(abs(Double(Float(bitPattern: r[i])) * x - 1) <= 1.0 / 4096)
                    #expect(abs(Double(Float(bitPattern: q[i])) * x.squareRoot() - 1) <= 1.0 / 4096)
                    #expect(abs(Double(Float(bitPattern: l[i])) - log2(x)) <= 1.0 / 32)
                }
                var e = [UInt32]()
                for i in 0..<4 { e.append((Float(Int32(bitPattern: w[i]) % 200 - 100) / 3).bitPattern) }
                Self.setV(&c, 3, Self.words(e))
                Self.run(&c, Self.vx(394, 5, 0, 3)) // vexptefp
                for (i, v) in Self.getW(c, 5).enumerated() {
                    let x = Double(Float(bitPattern: e[i]))
                    #expect(abs(Double(Float(bitPattern: v)) / exp2(x) - 1) <= 1.0 / 16)
                }
            }
            // Specials: 1/±0 = ±inf, 1/±inf = ±0; 1/sqrt(-0) = -inf, of a negative a NaN;
            // log2(0) = -inf, of a negative a NaN; 2^-inf = 0.
            Self.setV(&c, 3, Self.words([0x0000_0000, 0x8000_0000, 0x7F80_0000, 0xFF80_0000]))
            Self.run(&c, Self.vx(266, 1, 0, 3))
            #expect(Self.getW(c, 1) == [0x7F80_0000, 0xFF80_0000, 0, 0x8000_0000])
            Self.setV(&c, 3, Self.words([0x0000_0000, 0x8000_0000, 0x7F80_0000, 0xBF80_0000]))
            Self.run(&c, Self.vx(330, 1, 0, 3))
            #expect(Self.getW(c, 1) == [0x7F80_0000, 0xFF80_0000, 0, 0x7FC0_0000])
            Self.run(&c, Self.vx(458, 1, 0, 3))
            #expect(Self.getW(c, 1) == [0xFF80_0000, 0xFF80_0000, 0x7F80_0000, 0x7FC0_0000])
            Self.setV(&c, 3, Self.words([0xFF80_0000, 0x7F80_0000, 0, 0x3F80_0000]))
            Self.run(&c, Self.vx(394, 1, 0, 3))
            #expect(Self.getW(c, 1) == [0, 0x7F80_0000, 0x3F80_0000, 0x4000_0000])
        }

        @Test func conversionsSaturate() {
            ppc_altivec = true
            var c = Self.cpu()
            c.vscr = 0
            // vctsxs with a scale of 2^1: 1.75 -> 3, -3e9 -> min, NaN -> 0, 2^31 -> max.
            Self.setV(&c, 3, Self.words([0x3FE0_0000, 0xCF32_D05E, 0x7FC0_0000, 0x4F00_0000]))
            Self.run(&c, Self.vx(970, 1, 1, 3))
            #expect(Self.getW(c, 1) == [3, 0x8000_0000, 0, 0x7FFF_FFFF] && c.vscr == 1)
            c.vscr = 0
            // vctuxs: negative -> 0 (saturating), -0.5 -> 0 (not), large -> max.
            Self.setV(&c, 3, Self.words([0xBF80_0000, 0xBF00_0000, 0x4F80_0000, 0x4040_0000]))
            Self.run(&c, Self.vx(906, 1, 0, 3))
            #expect(Self.getW(c, 1) == [0, 0, 0xFFFF_FFFF, 3] && c.vscr == 1)
            c.vscr = 0
            Self.setV(&c, 3, Self.words([0, 0xBF00_0000, 0x4040_0000, 0x7F80_0001]))
            Self.run(&c, Self.vx(906, 1, 0, 3))
            #expect(c.vscr == 0)
            // vcfsx and vcfux with a scale: -6 / 4 = -1.5; 0xFFFFFFFF / 2^31 rounds to 2.0.
            Self.setV(&c, 3, Self.words([0xFFFF_FFFA, 0xFFFF_FFFF, 1, 0x7FFF_FFFF]))
            Self.run(&c, Self.vx(842, 1, 2, 3))
            #expect(Self.getW(c, 1)[0] == Float(-1.5).bitPattern)
            Self.run(&c, Self.vx(778, 1, 31, 3))
            #expect(Self.getW(c, 1)[1] == Float(2).bitPattern)
            // vrfin rounds ties to even, keeping the sign of a zero.
            Self.setV(&c, 3, Self.words([Float(2.5).bitPattern, Float(-0.5).bitPattern, Float(3.5).bitPattern, Float(-1.5).bitPattern]))
            Self.run(&c, Self.vx(522, 1, 0, 3))
            #expect(Self.getW(c, 1) == [Float(2).bitPattern, 0x8000_0000, Float(4).bitPattern, Float(-2).bitPattern])
        }

        // ---- Integer odds and ends the random cases cover less

        @Test func packsMergesAndSums() {
            ppc_altivec = true
            var c = Self.cpu()
            c.vscr = 0
            Self.setV(&c, 2, Self.halves([0x0001, 0x00FF, 0x0100, 0x7FFF, 0x8000, 0xFFFF, 0xFF80, 0x007F]))
            Self.setV(&c, 3, Self.halves([0x0000, 0x0080, 0xFF7F, 0x0002, 0x1234, 0xABCD, 0x0003, 0x0004]))
            Self.run(&c, Self.vx(398, 1, 2, 3)) // vpkshss
            #expect(Self.getV(c, 1) == [0x01, 0x7F, 0x7F, 0x7F, 0x80, 0xFF, 0x80, 0x7F, 0x00, 0x7F, 0x80, 0x02, 0x7F, 0x80, 0x03, 0x04])
            #expect(c.vscr == 1)
            Self.run(&c, Self.vx(14, 1, 2, 3)) // vpkuhum
            #expect(Self.getV(c, 1) == [0x01, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x80, 0x7F, 0x00, 0x80, 0x7F, 0x02, 0x34, 0xCD, 0x03, 0x04])
            Self.run(&c, Self.vx(12, 1, 2, 3)) // vmrghb
            #expect(Self.getV(c, 1) == [0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xFF, 0x80, 0x01, 0xFF, 0x00, 0x7F, 0x7F, 0x00, 0xFF, 0x02])
            // vsumsws: the sum of vA's words and vB's last into the last, saturating.
            c.vscr = 0
            Self.setV(&c, 2, Self.words([0x7FFF_FFFF, 1, 0, 0]))
            Self.setV(&c, 3, Self.words([9, 9, 9, 0]))
            Self.run(&c, Self.vx(1928, 1, 2, 3))
            #expect(Self.getW(c, 1) == [0, 0, 0, 0x7FFF_FFFF] && c.vscr == 1)
            // vpkpx and vupkhpx: 8:8:8:8 to 1:5:5:5 and back.
            Self.setV(&c, 2, Self.words([0x01FF_8040, 0x00F8_0808, 0xFF07_0707, 0x0000_00FF]))
            Self.run(&c, Self.vx(782, 1, 2, 2))
            #expect(Array(Self.getV(c, 1)[0..<8]) == Self.halves([0xFE08, 0x7C21, 0x8000, 0x001F]))
            Self.run(&c, Self.vx(846, 4, 0, 1))
            #expect(Self.getW(c, 4) == [0xFF1F_1008, 0x001F_0101, 0xFF00_0000, 0x0000_001F])
        }

        /// The destination may be any source.
        @Test func destinationMayBeASource() {
            ppc_altivec = true
            var c = Self.cpu()
            Self.setV(&c, 2, Array(0..<16).map(UInt8.init))
            Self.setV(&c, 3, Array(16..<32).map(UInt8.init))
            Self.setV(&c, 4, Array((0..<16).reversed()).map { UInt8($0 * 2) })
            Self.run(&c, Self.va(43, 4, 2, 3, 4)) // vperm v4,v2,v3,v4
            #expect(Self.getV(c, 4) == Array((0..<16).reversed()).map { UInt8($0 * 2) })
            Self.run(&c, Self.va(44, 2, 2, 3, 3)) // vsldoi v2,v2,v3,3
            #expect(Self.getV(c, 2) == Array(3..<19).map(UInt8.init))
        }

        // ---- The disassembler

        @Test func disassembly() {
            let cases: [(UInt32, String)] = [
                (Self.va(43, 5, 3, 4, 1), "vperm     v5,v3,v4,v1"),
                (Self.va(46, 1, 2, 3, 4), "vmaddfp   v1,v2,v4,v3"),
                (Self.va(44, 2, 2, 3, 3), "vsldoi    v2,v2,v3,3"),
                (Self.vc(6, 1, 2, 3, rc: true), "vcmpequb. v1,v2,v3"),
                (Self.vc(966, 1, 2, 3), "vcmpbfp   v1,v2,v3"),
                (Self.vx(1156, 1, 2, 2), "vmr       v1,v2"),
                (Self.vx(1156, 1, 2, 3), "vor       v1,v2,v3"),
                (Self.vx(908, 3, 31, 0), "vspltisw  v3,-1"),
                (Self.vx(652, 3, 2, 4), "vspltw    v3,v4,2"),
                (Self.vx(970, 1, 1, 3), "vctsxs    v1,v3,1"),
                (Self.vx(266, 1, 0, 3), "vrefp     v1,v3"),
                (Self.vx(1540, 7, 0, 0), "mfvscr    v7"),
                (Self.vx(1604, 0, 0, 8), "mtvscr    v8"),
                (Self.x31(103, 1, 3, 4), "lvx       v1,r3,r4"),
                (Self.x31(231, 5, 0, 4), "stvx      v5,0,r4"),
                (0x7C60_43A6, "mtvrsave  r3"),
                (0x7C80_42A6, "mfvrsave  r4"),
                (Self.x31(342, 1, 3, 4) | 0x0200_0000, "dstt      r3,r4,1"),
                (Self.x31(374, 2, 3, 4), "dstst     r3,r4,2"),
                (Self.x31(822, 0, 0, 0) | 0x0200_0000, "dssall"),
            ]
            for (op, want) in cases {
                var buf = [CChar](repeating: 0, count: 128)
                ppc_disasm(0, op, &buf, Int32(buf.count))
                #expect(String(cString: buf) == want, "\(String(op, radix: 16))")
            }
            // Every instruction the interpreter knows has a name.
            let url = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
                .appendingPathComponent("PPC/altivec_vectors.txt")
            if let text = try? String(contentsOf: url, encoding: .utf8) {
                for line in text.split(separator: "\n") where !line.hasPrefix("#") {
                    let f = line.split(separator: " ")
                    let op = UInt32(f[1], radix: 16)!
                    var buf = [CChar](repeating: 0, count: 128)
                    ppc_disasm(0, op, &buf, Int32(buf.count))
                    let s = String(cString: buf)
                    let name = s.split(separator: " ").first.map(String.init) ?? ""
                    #expect(name == f[0] || (f[0] == "vor" && name == "vmr") || (f[0] == "vnor" && name == "vnot"),
                            "\(f[0]): \(s)")
                    #expect(ppc_vec_insn(op))
                }
            }
        }

        // ---- The JIT leaves vector code to the interpreter

        /// A loop of vector loads, adds and stores runs the same with the JIT.
        @Test(arguments: [false, true])
        func vectorLoopUnderTheJIT(_ jit: Bool) {
            if jit {
                guard ppc_jit_available() else { return }
                ppc_jit_enable(true, nil, nil)
            }
            defer { if jit { ppc_jit_enable(false, nil, nil) } }
            bootMachine()
            ppc_altivec = true
            for i in 0..<256 { wr8(Self.data + UInt32(i), UInt8(i)) }
            // li r5,0; li r6,16; mtctr r6; vspltisb v2,1;
            // loop: lvx v1,r3,r5; vaddubm v1,v1,v2; stvx v1,r4,r5; addi r5,r5,16; bdnz loop; blr
            let code: [UInt32] = [
                0x38A0_0000, 0x38C0_0010, 0x7CC9_03A6, Self.vx(780, 2, 1, 0),
                Self.x31(103, 1, 3, 5), Self.vx(0, 1, 1, 2), Self.x31(231, 1, 4, 5), 0x38A5_0010, 0x4200_FFF0,
                0x4E80_0020,
            ]
            for (i, w) in code.enumerated() { wr32(Self.codeAt + UInt32(i * 4), w) }
            wr32(Self.returnAt, PPC_NATIVE_OP)
            var c = Self.cpu()
            c.native = { c, _ in c?.pointee.halted = true }
            c.pc = Self.codeAt
            c.lr = Self.returnAt
            Self.setR(&c, 3, Self.data)
            Self.setR(&c, 4, Self.data + 0x1000)
            _ = ppc_run(&c, 10000)
            let out: [UInt8] = (0..<256).map { rd8(Self.data + 0x1000 + UInt32($0)) }
            let want: [UInt8] = (0..<256).map { UInt8(($0 + 1) & 0xFF) }
            #expect(out == want)
            #expect(c.icount == 4 + 16 * 5 + 2, "\(c.icount)")
        }
    }
}
