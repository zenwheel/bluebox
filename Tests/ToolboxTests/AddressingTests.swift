import CCore
import CPU68K
import ForkStore
import Foundation
import Testing
import Toolbox

/// Boot with 24-bit addressing (and a processor), then back to the defaults.
func boot24(cpu: Int32 = 68040, _ body: () -> Void) {
    machine_set_addressing(24)
    machine_set_cpu(cpu)
    bootMachine()
    body()
    machine_set_addressing(0)
    machine_set_cpu(68040)
    bootMachine()
}

extension BlueBox {
    /// M22's 24-bit addressing: the top byte of every address ignored, the
    /// ROM and screen inside the 16 MB, the master pointers' flags.
    @Suite struct Addressing24 {
        @Test func theTopByteIsIgnoredEverywhere() {
            boot24 {
                #expect(g_mem_24bit)
                let p = tb_NewPtr(16, false, true)
                wr32(p | 0x8000_0000, 0x1234_5678)  // the Toolbox's own accesses
                #expect(rd32(p) == 0x1234_5678)
                #expect(rd32(p | 0x4300_0000) == 0x1234_5678)
                // The CPU's, through a pointer with flags in it.
                let reason = run68k([
                    0x207C, UInt16(0x8000 | (p >> 16)), UInt16(p & 0xFFFF),  // MOVEA.L #p|$80000000,A0
                    0x2E10,                                                  // MOVE.L (A0),D7
                    0x217C, 0xCAFE, 0xF00D, 0x0004,                          // MOVE.L #$CAFEF00D,4(A0)
                    0xA9F4,
                ])
                #expect(reason == "ExitToShell")
                #expect(g_cpu!.pointee.d.7 == 0x1234_5678)
                #expect(rd32(p + 4) == 0xCAFE_F00D)
                // An access off the top of the space wraps to address 0.
                wr16(0, 0xBEEF)
                #expect(rd32(0xFFFF_FFFE) & 0xFFFF == 0xBEEF)
            }
            #expect(!g_mem_24bit)
        }

        @Test func layoutIsInsideSixteenMegabytes() {
            boot24 {
                #expect(rd32(UInt32(LM_ROMBase)) == UInt32(GA24_ROM))
                #expect(rd32(UInt32(LM_ScrnBase)) == UInt32(GA24_SCREEN))
                #expect(rd32(UInt32(LM_MemTop)) == UInt32(GA24_TOP))
                #expect(rd8(UInt32(LM_MMU32Bit)) == 0)
                // Trap table entries and exception vectors point into the 24-bit ROM.
                let entry = rd32(0x0E00 + 0x1F4 * 4)  // a Toolbox trap
                #expect(entry >= UInt32(GA24_ROM) && entry < 0x0100_0000)
                #expect(rd32(4 * 4) < 0x0100_0000)
                var r: UInt32 = 0
                #expect(tb_Gestalt(Int32(bitPattern: fourCC("addr")), &r) == 0 && r == 0x4)
                // The screen is as deep as fits a card's megabyte: 8 bits at 1024 by 768.
                #expect(machine_screen_depth() == 8)
            }
            var r: UInt32 = 0
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("addr")), &r) == 0 && r == 0x7)
            #expect(rd8(UInt32(LM_MMU32Bit)) == 1)
        }

        @Test func stripAddressSwapMMUModeAndTranslate() {
            boot24 {
                #expect(tb_StripAddress(0xC012_3456) == 0x0012_3456)
                #expect(tb_Translate24To32(0x8045_6789) == 0x0045_6789)
                // SwapMMUMode to 32-bit and back: D0 answers the mode it was in.
                let reason = run68k([
                    0x7001, 0xA05D, 0x2E00,  // MOVEQ #1,D0; _SwapMMUMode; MOVE.L D0,D7
                    0x1C38, 0x0CB2,          // MOVE.B MMU32Bit,D6
                    0x7000, 0xA05D, 0x2A00,  // MOVEQ #0,D0; _SwapMMUMode; MOVE.L D0,D5
                    0xA9F4,
                ])
                #expect(reason == "ExitToShell")
                #expect(g_cpu!.pointee.d.7 & 0xFF == 0 && g_cpu!.pointee.d.6 & 0xFF == 1)
                #expect(g_cpu!.pointee.d.5 & 0xFF == 1)
                #expect(rd8(UInt32(LM_MMU32Bit)) == 0 && g_cpu!.pointee.addr_mask == 0x00FF_FFFF)
            }
            // Started in 32-bit mode it stays there.
            #expect(tb_StripAddress(0xC012_3456) == 0xC012_3456)
            var d0: UInt32 = 0
            tb_SwapMMUMode(&d0)
            #expect(d0 == 1 && g_cpu!.pointee.addr_mask == 0xFFFF_FFFF)
            // The old mode alone comes back in D0, the rest of it cleared
            // (Prince of Persia 2 counts a loop in D0's low word after it).
            d0 = 0x0049_0001
            tb_SwapMMUMode(&d0)
            #expect(d0 == 1)
        }

        /// The flags are the master pointer's top byte: HLock sets bit 7
        /// there, and a lock set there directly (BSET #7,(A0)) keeps the
        /// block from moving.
        @Test func masterPointerFlags() {
            boot24 {
                let z = rd32(UInt32(LM_ApplZone))
                let h = tb_NewHandle(100, false, true)
                let p = rd32(h)
                #expect(p >> 24 == 0)
                tb_HLock(h)
                #expect(rd32(h) == p | 0x8000_0000)
                tb_HPurge(h)
                tb_HSetRBit(h)
                #expect(rd8(h) == 0xE0 && UInt8(bitPattern: tb_HGetState(h)) == 0xE0)
                tb_HUnlock(h)
                tb_HNoPurge(h)
                tb_HClrRBit(h)
                #expect(rd32(h) == p && tb_HGetState(h) == 0)
                // HSetState keeps the whole byte, as it was the pointer's top.
                tb_HSetState(h, Int8(bitPattern: 0x81))
                #expect(rd32(h) == p | 0x8100_0000)
                tb_HSetState(h, 0)

                // Locked by the program itself: a block freed before it
                // doesn't make it move, and the size and handle are found.
                let before = tb_NewHandle(4000, false, true)
                let h2 = tb_NewHandle(64, false, true)
                let p2 = rd32(h2)
                wr8(h2, rd8(h2) | 0x80)
                tb_DisposeHandle(before)
                _ = tb_CompactMem(0x7FFF_FFFF, false)
                #expect(rd32(h2) == p2 | 0x8000_0000)
                #expect(tb_GetHandleSize(h2) == 64 && memErr() == 0)
                #expect(tb_RecoverHandle(rd32(h2), false) == h2)
                #expect(tb_HGetState(h2) & Int8(bitPattern: 0x80) != 0)
                // Unlocked again the same way, it moves, flags and all kept.
                wr8(h2, 0x40)  // purgeable only
                _ = tb_CompactMem(0x7FFF_FFFF, false)
                #expect(rd32(h2) & 0x00FF_FFFF != p2 && rd8(h2) == 0x40)
                // SetHandleSize moving it keeps its flags.
                tb_SetHandleSize(h2, 50000)
                #expect(memErr() == 0 && rd8(h2) == 0x40 && tb_GetHandleSize(h2) == 50000)
                // Purged: the master pointer is NIL, flags and all.
                tb_EmptyHandle(h2)
                #expect(rd32(h2) == 0)
                #expect(mm_check_zone(z))
            }
        }

        /// A disposed handle's master pointer holds the next free one; in
        /// 24-bit mode the "header" twelve bytes before that can be a locked
        /// handle's master pointer, whose $80 reads as a relocatable block.
        /// Calls on the disposed handle leave that handle alone (Pararena
        /// unlocks a handle it has disposed).
        @Test func disposedHandleLeavesTheNextMasterPointerAlone() {
            boot24 {
                let z = rd32(UInt32(LM_ApplZone))
                tb_MoreMasters()
                let a = tb_NewHandle(96, false, true)
                let b = tb_NewHandle(16, false, true)
                let c = tb_NewHandle(16, false, true)
                let d = tb_NewHandle(16, false, true)
                #expect(b == a + 4 && c == a + 8 && d == a + 12)
                tb_HLock(a)
                let pa = rd32(a)
                #expect(pa >> 24 == 0x80)
                tb_DisposeHandle(d)
                tb_DisposeHandle(b)
                #expect(rd32(b) == a + 12)  // the free list: b, then d
                tb_HUnlock(b)
                #expect(memErr() == -111)  // memWZErr
                tb_HSetState(b, 0)
                tb_HLock(b)
                #expect(tb_GetHandleSize(b) == -111)
                tb_DisposeHandle(b)  // disposed twice
                #expect(memErr() == -111)
                #expect(rd32(a) == pa && rd32(b) == a + 12)
                #expect(mm_check_zone(z))
                // A program's own copy of a master pointer still names the block.
                let copy = tb_NewPtr(4, false, true)
                wr32(copy, rd32(c))
                #expect(tb_GetHandleSize(copy) == 16 && memErr() == 0)
                tb_HUnlock(a)
                tb_DisposeHandle(a)
                tb_DisposeHandle(c)
                #expect(mm_check_zone(z))
            }
        }

        /// A program whose SIZE resource doesn't say it is 32-bit clean
        /// asks for 24-bit addressing, and gets its partition under the
        /// screen.
        @Test func sizeResourceChoosesAndPartitionFits() throws {
            let dir = tempDir()
            defer { try? FileManager.default.removeItem(at: dir) }
            func app(_ name: String, flags: UInt16?, ppc: Bool = false, minimum: UInt32 = 0x80000) -> String {
                let jt = cat(be16(0), be16(0x3F3C), be16(1), be16(0xA9F0))
                var res = [
                    TestResource(type: "CODE", id: 0, data: cat(be32(40), be32(256), be32(8), be32(32), jt)),
                    TestResource(type: "CODE", id: 1, data: cat(be16(0), be16(1), be16(0xA9F4))),
                ]
                if let flags { res.append(TestResource(type: "SIZE", id: -1, data: cat(be16(Int(flags)), be32(minimum), be32(minimum)))) }
                if ppc { res.append(TestResource(type: "cfrg", id: 0, data: [UInt8](repeating: 0, count: 32))) }
                var fork = buildFork(res)
                let path = dir.appendingPathComponent(name).path
                var f = mac_file()
                var empty: [UInt8] = []
                empty.withUnsafeMutableBufferPointer { f.data = $0.baseAddress; f.data_len = 0 }
                fork.withUnsafeMutableBufferPointer { f.rsrc = $0.baseAddress; f.rsrc_len = $0.count }
                var err = [CChar](repeating: 0, count: 256)
                #expect(appledouble_write(path, &f, &err, err.count))
                return path
            }
            let dirty = app("Dirty", flags: 0x5800), clean = app("Clean", flags: 0x5880), old = app("Old", flags: nil)
            #expect(machine_app_addressing(dirty) == 24)
            #expect(machine_app_addressing(clean) == 32)
            #expect(machine_app_addressing(old) == 24)
            #expect(machine_app_addressing(app("Fat", flags: 0x5800, ppc: true)) == 32)
            // A minimum partition no 24-bit Mac had room for: run in 32-bit mode.
            #expect(machine_app_addressing(app("Big", flags: 0x0000, minimum: 16_000 * 1024)) == 32)
            #expect(machine_app_addressing(app("Fits", flags: 0x0000, minimum: 6 * 1024 * 1024)) == 24)
            boot24 {
                var err = [CChar](repeating: 0, count: 256)
                #expect(machine_launch(clean, &err, err.count))  // chosen for it: 24-bit all the same
                let a5 = rd32(UInt32(LM_CurrentA5))
                #expect(a5 < UInt32(GA24_TOP) && a5 > 0x0040_0000)
                #expect(rd32(UInt32(LM_ApplLimit)) < a5)
            }
        }
    }
}
