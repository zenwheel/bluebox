import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// FileSigningLib on a Mac with no keychain: a file's signatures are its
    /// 'sig#' resources, which can be found, loaded and removed, while
    /// signing and verifying one need modules that aren't there.
    @Suite struct OS9FileSigning {
        func err(_ name: String, _ args: [UInt32]) -> Int32 { Int32(bitPattern: ilCall(name, args)) }

        func spec(_ name: String) -> UInt32 {
            let n = tb_NewPtr(64, false, true), s = tb_NewPtr(70, false, true)
            guestPString(name, at: n)
            _ = tb_FSMakeFSSpec(0, 0, n, s)
            return s
        }

        @Test func signaturesAreSigResources() {
            launchTestApp()
            let plain = spec("Plain"), signed = spec("Signed")
            tb_FSpCreateResFile(plain, Int32(bitPattern: fourCC("ttxt")), Int32(bitPattern: fourCC("TEXT")), 0)
            tb_FSpCreateResFile(signed, Int32(bitPattern: fourCC("ttxt")), Int32(bitPattern: fourCC("TEXT")), 0)
            let rn = tb_FSpOpenResFile(signed, 3)
            #expect(rn > 0)
            let h = tb_NewHandle(8, false, true)
            wr32(rd32(h), 0x5349_4721)
            tb_AddResource(h, Int32(bitPattern: fourCC("sig#")), 1, 0)
            tb_CloseResFile(rn)

            #expect(ilCall("SecMacHasSignature", [plain, 1]) & 0xFF == 0)
            #expect(ilCall("SecMacHasSignature", [signed, 1]) & 0xFF == 1)
            let which = tb_NewPtr(2, false, true), out = tb_NewPtr(4, false, true)
            #expect(err("SecMacFindSignatureToVerify", [plain, 1, which]) == -13836) // errSecNoSignatureFound
            #expect(err("SecMacFindSignatureToVerify", [signed, 1, which]) == 0 && rd16(which) == 1)
            #expect(err("SecMacLoadSigPound", [signed, out]) == 0)
            #expect(tb_GetHandleSize(rd32(out)) == 8 && rd32(rd32(rd32(out))) == 0x5349_4721)

            // Verifying: nothing to verify, or no certificate module to do it.
            #expect(err("SecMacVerifyFileSimple", [plain, 0, 0, 0]) == -13836)
            #expect(err("SecMacVerifyFileSimple", [signed, 0, 0, 0]) == -25313)
            #expect(err("SecMacSignFileSimple", [plain, 0, 0]) == -25313)

            #expect(err("SecMacRemoveAllSignatures", [signed, 1]) == 0)
            #expect(ilCall("SecMacHasSignature", [signed, 1]) & 0xFF == 0)
            #expect(err("SecMacRemoveAllSignatures", [signed, 1]) == -13836)
            #expect(err("SecSetProgressCallback", [0x1000, 0]) == 0)
            #expect(err("SecRemoveProgressCallback", [0x1000]) == 0)
        }
    }
}
