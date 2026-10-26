import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// The accessors of LowMem.h that gen.py's table lacks (the rows of
    /// lowmem_acc_data.c), on the guest's low memory: values of each size
    /// and sign, an address, a record by pointer, an array by index.
    @Suite struct LowMemoryTableAccessors {
        init() { bootMachine() }

        @Test func valuesOfEachSizeAndSign() {
            // 4 bytes
            ilCall("LMSetADBBase", [0xDEAD_BEEF])
            #expect(rd32(0xCF8) == 0xDEAD_BEEF)
            #expect(ilCall("LMGetADBBase", []) == 0xDEAD_BEEF)
            // 1 byte: only the low byte is stored, the next byte is left, and it reads back unsigned
            let next = rd8(0x220)
            ilCall("LMSetAlarmState", [0x1F0])
            #expect(rd8(0x21F) == 0xF0 && rd8(0x220) == next)
            wr8(0x12C, 0xFE)
            #expect(ilCall("LMGetDskVerify", []) == 0xFE)
            // 2 bytes: signed ones come back sign-extended
            ilCall("LMSetAt2MenuBottom", [0x1_8001])
            #expect(rd16(0xB28) == 0x8001)
            #expect(Int32(bitPattern: ilCall("LMGetAt2MenuBottom", [])) == -0x7FFF)
            wr16(0xB28, 0x1234)
            #expect(ilCall("LMGetAt2MenuBottom", []) == 0x1234)
        }

        @Test func addressesAndRecords() {
            // An array or a record comes back as its address in low memory...
            #expect(ilCall("LMGetBootTmp8", []) == 0xB36)
            #expect(ilCall("LMGetDiskVars", []) == 0x222)
            #expect(ilCall("LMGetTheCrsr", []) == 0x844)
            // ...and the Set copies in what it is given.
            let src = tb_NewPtr(80, false, true)
            for i in 0..<8 { wr8(src + UInt32(i), UInt8(0x10 + i)) }
            ilCall("LMSetBootTmp8", [src])
            #expect((0..<8).allSatisfy { rd8(0xB36 + UInt32($0)) == UInt8(0x10 + $0) })
            for i in 0..<68 { wr8(src + UInt32(i), UInt8(truncatingIfNeeded: i * 3)) }
            ilCall("LMSetTheCrsr", [src])
            #expect((0..<68).allSatisfy { rd8(0x844 + UInt32($0)) == UInt8(truncatingIfNeeded: $0 * 3) })

            // A Get through a pointer copies out: a Point from the mouse, six bytes of colour.
            wr32(0x830, 0x0050_0060)
            let out = tb_NewPtr(80, false, true)
            ilCall("LMGetMouse", [out])
            #expect(rd32(out) == 0x0050_0060)
            for i in 0..<6 { wr8(0xC10 + UInt32(i), UInt8(0xA0 + i)) }
            wr8(out + 6, 0x77)
            ilCall("LMGetRGBBlack", [out])
            #expect((0..<6).allSatisfy { rd8(out + UInt32($0)) == UInt8(0xA0 + $0) } && rd8(out + 6) == 0x77)
            // Params is two pieces of memory in 50 bytes
            for i in 0..<50 { wr8(0x3A4 + UInt32(i), UInt8(i + 1)) }
            ilCall("LMGetParams", [out])
            #expect((0..<50).allSatisfy { rd8(out + UInt32($0)) == UInt8($0 + 1) })
        }

        @Test func aStringIsCopiedWhole() {
            // ExtraPrint is a Str12 (13 bytes, the length byte first), set by pointer and got by address.
            let str = tb_NewPtr(16, false, true)
            let text: [UInt8] = [5, 0x48, 0x65, 0x6C, 0x6C, 0x6F, 0, 0, 0, 0, 0, 0, 0]
            for (i, b) in text.enumerated() { wr8(str + UInt32(i), b) }
            wr8(0x947 + 13, 0x5A)
            ilCall("LMSetExtraPrint", [str])
            let at = ilCall("LMGetExtraPrint", [])
            #expect(at == 0x947)
            #expect((0..<13).allSatisfy { rd8(at + UInt32($0)) == text[$0] } && rd8(0x947 + 13) == 0x5A)
        }

        @Test func indexedArrays() {
            // The index is a short, its sign extended: -1 is the element before the first.
            for i in 0..<4 { wr32(0x1E00 + UInt32(i * 4), 0x1000 + UInt32(i)) }
            #expect(ilCall("LMGetMemVectors24", [3]) == 0x1003)
            #expect(ilCall("LMGetMemVectors24", [0x1_0002]) == 0x1002) // only the low half counts
            ilCall("LMSetMemVectors24", [0xCAFE_F00D, 1])
            #expect(rd32(0x1E04) == 0xCAFE_F00D)
            wr32(0x1DFC, 0x77)
            #expect(ilCall("LMGetMemVectors24", [0xFFFF]) == 0x77)
            ilCall("LMSetMemVectors24", [0x88, 0xFFFF])
            #expect(rd32(0x1DFC) == 0x88)
            // Words: signed ones are sign-extended.
            ilCall("LMSetOmegaSANE", [0x8001, 5])
            #expect(rd16(0xB6C + 10) == 0x8001)
            #expect(Int32(bitPattern: ilCall("LMGetOmegaSANE", [5])) == -0x7FFF)
        }

        @Test func everyRowIsExported() {
            for name in ["LMGetADBBase", "LMSetVectors", "LMGetExtraPrint", "LMSetExtraPrint"] {
                #expect(cfm_export_fn(name) != nil, "\(name)")
            }
        }
    }
}
