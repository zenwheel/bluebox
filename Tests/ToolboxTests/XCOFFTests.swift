import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// XCOFF containers, which the first PowerPC tools wrote (Boingo
    /// Electro, Bumbler Bee-Luxe) and the Code Fragment Manager loaded as
    /// it did PEF.
    @Suite struct XCOFFContainers {
        func be32(_ v: UInt32) -> [UInt8] { [UInt8(v >> 24), UInt8((v >> 16) & 0xFF), UInt8((v >> 8) & 0xFF), UInt8(v & 0xFF)] }
        func be16(_ v: UInt16) -> [UInt8] { [UInt8(v >> 8), UInt8(v & 0xFF)] }

        func bytes(_ b: [UInt8]) -> UInt32 {
            let p = tb_NewPtr(Int32(max(b.count, 1)), false, true)
            for (i, x) in b.enumerated() { wr8(p + UInt32(i), x) }
            return p
        }

        /// .data at 0x1000: an import's slot, main's descriptor (code, TOC)
        /// and an exported word; .text at 0x2000; .bss at 0x3000.  The
        /// linker's addresses, which the loader moves.
        func container() -> [UInt8] {
            let text: [UInt8] = [0x38, 0x60, 0x00, 0x07, 0x4E, 0x80, 0x00, 0x20] // li r3,7; blr
            let data = be32(0) + be32(0x2000) + be32(0x1000) + be32(0x3000)
            // The loader section: header, three symbols, four relocations,
            // the import file IDs, the string table.
            let ids = Array("/usr/lib\0\0\0\0InterfaceLib\0\0".utf8)
            var strings = be16(18) + Array("GetCurrentProcess".utf8) + [0]
            let counterName = UInt32(strings.count + 2)
            strings += be16(7) + Array("Counter".utf8) + [0]
            let nsyms: UInt32 = 3, nreloc: UInt32 = 4
            let impoff = 32 + nsyms * 24 + nreloc * 12
            let stoff = impoff + UInt32(ids.count)
            var loader = be32(1) + be32(nsyms) + be32(nreloc) + be32(UInt32(ids.count)) + be32(2) + be32(impoff)
            loader += be32(UInt32(strings.count)) + be32(stoff)
            // GetCurrentProcess, imported from InterfaceLib as a descriptor.
            loader += be32(0) + be32(2) + be32(0) + be16(0) + [0x40, 10] + be32(1) + be32(0)
            // Counter, exported data at 0x100C.
            loader += be32(0) + be32(counterName) + be32(0x100C) + be16(1) + [0x10, 5] + be32(0) + be32(0)
            // main, a descriptor at 0x1004.
            loader += Array("main\0\0\0\0".utf8) + be32(0x1004) + be16(1) + [0x10, 10] + be32(0) + be32(0)
            loader += be32(0x1000) + be32(3) + [31, 0] + be16(1) // the import's address
            loader += be32(0x1004) + be32(0) + [31, 0] + be16(1) // main's code: by .text
            loader += be32(0x1008) + be32(1) + [31, 0] + be16(1) // its TOC: by .data
            loader += be32(0x100C) + be32(2) + [31, 0] + be16(1) // a pointer into .bss
            loader += ids + strings
            let opthdr = 72, sections = 4
            let dataOff = UInt32(20 + opthdr + sections * 40)
            let textOff = dataOff + UInt32(data.count), loaderOff = textOff + UInt32(text.count)
            var x = be16(0x01DF) + be16(UInt16(sections)) + be32(0) + be32(0) + be32(0) + be16(UInt16(opthdr)) + be16(0x1002)
            var aux = be16(0x010B) + be16(1) + be32(8) + be32(16) + be32(16) + be32(0x1004) + be32(0x2000) + be32(0x1000)
            aux += be32(0x1000) + be16(1) + be16(2) + be16(1) + be16(1) + be16(4) + be16(3) + be16(2) + be16(3)
            aux += [0x31, 0x4C, 0, 0] + be32(0) + be32(0)
            x += aux + [UInt8](repeating: 0, count: opthdr - aux.count)
            func header(_ name: String, _ vaddr: UInt32, _ size: UInt32, _ ptr: UInt32, _ flags: UInt32) -> [UInt8] {
                Array(name.utf8) + [UInt8](repeating: 0, count: 8 - name.utf8.count) + be32(vaddr) + be32(vaddr) + be32(size)
                    + be32(ptr) + be32(0) + be32(0) + be16(0) + be16(0) + be32(flags)
            }
            x += header(".data", 0x1000, UInt32(data.count), dataOff, 0x40)
            x += header(".text", 0x2000, UInt32(text.count), textOff, 0x20)
            x += header(".loader", 0, UInt32(loader.count), loaderOff, 0x1000)
            x += header(".bss", 0x3000, 16, 0, 0x80)
            return x + data + text + loader
        }

        @Test func loadsRelocatesAndExports() {
            bootMachine()
            _ = cfm_ppc_cpu()
            let image = container()
            let conn = bytes(be32(0)), mainp = bytes(be32(0)), errp = bytes([UInt8](repeating: 0, count: 256))
            let regs: [UInt32] = [bytes(image), UInt32(image.count), bytes([1, 0x58]), 1, conn, mainp, errp]
            withUnsafeMutableBytes(of: &g_ppc.gpr) { gpr in
                for (i, a) in regs.enumerated() { gpr.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
            }
            guard let get = cfm_export_fn("GetMemFragment") else { Issue.record("no GetMemFragment"); return }
            get(&g_ppc)
            let result = withUnsafeBytes(of: g_ppc.gpr) { $0.load(fromByteOffset: 12, as: UInt32.self) }
            #expect(result == 0)
            let main = rd32(mainp)
            #expect(main != 0)
            // main's descriptor: the moved code, and the TOC at .data.
            let code = rd32(main), toc = rd32(main + 4)
            #expect(rd32(code) == 0x3860_0007)
            #expect(toc == main - 4)
            // The import's slot holds GetCurrentProcess's transition vector.
            let tv = rd32(toc)
            #expect(tv != 0 && rd32(tv) != 0)
            // Counter is exported where .data went, and points into .bss.
            let find = cfm_export_fn("FindSymbol")!
            let addr = bytes(be32(0)), cls = bytes([0, 0])
            let fsRegs: [UInt32] = [rd32(conn), bytes([7] + Array("Counter".utf8)), addr, cls]
            withUnsafeMutableBytes(of: &g_ppc.gpr) { gpr in
                for (i, a) in fsRegs.enumerated() { gpr.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
            }
            find(&g_ppc)
            #expect(rd32(addr) == toc + 12)
            let bss = rd32(toc + 12)
            #expect(bss != 0x3000 && bss != 0 && rd32(bss) == 0)
        }
    }
}
