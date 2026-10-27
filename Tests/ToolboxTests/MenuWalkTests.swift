import CCore
import CPU68K
import Foundation
import ForkStore
import Testing
import Toolbox

extension BlueBox {
    /// The menu walker (--walk-menus) with synthesized programs that have
    /// no menu bar of their own.
    @Suite(.serialized) struct MenuWalk {
        /// A one-segment application of the given code, launched.
        func launch(_ code: [UInt8]) throws -> URL {
            bootMachine()
            let jt = cat(be16(0), be16(0x3F3C), be16(1), be16(0xA9F0)) // entry 0: seg 1, offset 0
            let code0 = cat(be32(40), be32(256), be32(8), be32(32), jt)
            var fork = buildFork([
                TestResource(type: "CODE", id: 0, data: code0),
                TestResource(type: "CODE", id: 1, data: cat(be16(0), be16(1), code)),
            ])
            let dir = tempDir()
            let path = dir.appendingPathComponent("NoMenus").path
            var f = mac_file()
            var empty: [UInt8] = []
            empty.withUnsafeMutableBufferPointer { f.data = $0.baseAddress; f.data_len = 0 }
            fork.withUnsafeMutableBufferPointer { f.rsrc = $0.baseAddress; f.rsrc_len = $0.count }
            var err = [CChar](repeating: 0, count: 256)
            #expect(appledouble_write(path, &f, &err, err.count))
            #expect(machine_launch(path, &err, err.count))
            return dir
        }

        /// A title that waits for a click by polling Button (ANSI Vision's)
        /// sees the walk's click and goes on.
        @Test func aTitlePollingButtonSeesTheClick() throws {
            let dir = try launch(cat(
                be16(0x554F),                 // loop: SUBQ #2,SP
                be16(0xA974),                 // _Button
                be16(0x4A1F),                 // TST.B (SP)+
                be16(0x67FA),                 // BEQ loop
                be16(0xA9F4)))                // _ExitToShell
            defer { try? FileManager.default.removeItem(at: dir) }
            machine_set_menu_walk(true)
            defer { machine_set_menu_walk(false) }
            #expect(machine_run(400_000_000) == RUN_HALTED)
            #expect(String(cString: machine_halt_reason()) == "ExitToShell")
        }
    }
}
