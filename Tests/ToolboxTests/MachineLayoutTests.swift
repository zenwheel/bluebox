import CCore
import CPU68K
import Testing
import Toolbox

/// Whether the megabyte at `a` holds a device window.
private func mapped(_ a: UInt32) -> Bool { hw_mapped(a) }

extension BlueBox {
    /// A machine chosen by name is one machine: its processor, FPU and
    /// addressing (which --cpu, --fpu and --addressing can change), its
    /// memory laid out as the real one's, and every report agreeing.
    @Suite struct MachineLayouts {
        func gestalt(_ s: String) -> UInt32 {
            var r: UInt32 = 0
            _ = tb_Gestalt(Int32(bitPattern: fourCC(s)), &r)
            return r
        }

        /// What SysEnvirons says: machine type, processor, FPU.
        func sysEnv() -> (machine: UInt16, proc: UInt16, fpu: Bool) {
            let env = tb_NewPtr(16, false, true)
            #expect(tb_SysEnvirons(2, env) == 0)
            return (rd16(env + 2), rd16(env + 6), rd8(env + 8) != 0)
        }

        /// A Plus: a 68000, 24-bit, 4 MB with the screen near its top.
        @Test func plusIsA68000WithItsOwnMemory() {
            withMachine("plus") {
                launchTestApp()
                #expect(g_cpu!.pointee.model == 68000 && g_cpu!.pointee.fpu == 0)
                #expect(g_mem_24bit && g_mem_layout == Int32(MEM_LAYOUT_COMPACT))
                #expect(rd32(UInt32(LM_MemTop)) == 0x40_0000)
                #expect(rd32(UInt32(LM_ScrnBase)) == 0x3F_A700)
                #expect(rd32(UInt32(LM_SoundBase)) == 0x3F_FD00)
                #expect(rd32(UInt32(LM_ROMBase)) == 0x40_0000)
                #expect(rd8(UInt32(LM_CPUFlag)) == 0 && rd8(UInt32(LM_MMUType)) == 0 && rd8(UInt32(LM_MMU32Bit)) == 0)
                #expect(rd16(UInt32(LM_SCSIFlags)) == 0xC000)
                #expect(gestalt("mach") == 4 && gestalt("proc") == 1 && gestalt("fpu ") == 0 && gestalt("addr") == 0)
                #expect(gestalt("ram ") == 0x40_0000)
                let env = sysEnv()
                #expect(env.machine == 2 && env.proc == 1 && !env.fpu)
                // The partition is under the alternate screen page, and the
                // heaps and the stack are all in the 4 MB.
                let a5 = rd32(UInt32(LM_CurrentA5))
                #expect(a5 < 0x3F_2700 && a5 > 0x10_0000)
                #expect(rd32(UInt32(LM_ApplLimit)) < a5)
                #expect(rd32(UInt32(LM_BufPtr)) <= 0x3F_2700)
                // The system heap keeps room for what System 6 put there
                // (fonts, desk accessories, INITs).
                let sysFree = tb_FreeMem(true)
                #expect(sysFree > 0x8_0000)
                // A word on an odd address faults: the strict interpreter,
                // with the devices still reached through it.
                #expect(run68k([0x13FC, 0x0040, 0x00EF, 0xFFFE, 0xA9F4]) == "ExitToShell")  // MOVE.B #$40,vBufA
                #expect(hw_via_read(15) & 0x40 != 0)
                #expect(run68k([0x3039, 0x0000, 0x1001, 0xA9F4]).contains("address error"))  // MOVE.W $1001,D0
                // SwapMMUMode can't leave 24-bit mode on a 68000.
                var mode: UInt32 = 1
                tb_SwapMMUMode(&mode)
                #expect(mode & 0xFF == 0 && rd8(UInt32(LM_MMU32Bit)) == 0)
            }
        }

        /// --cpu and --fpu change a named machine's processor and nothing else
        /// of it: an SE with a 68030 accelerator is still an SE.
        @Test func explicitProcessorOverridesTheMachines() {
            withMachine("se") {
                machine_set_cpu(68030)
                machine_set_fpu(68882)
                launchTestApp()
                #expect(g_cpu!.pointee.model == 68030 && g_cpu!.pointee.fpu == 68882)
                #expect(g_mem_layout == Int32(MEM_LAYOUT_COMPACT))  // the SE's bus is still 24 bits
                #expect(gestalt("mach") == 5 && gestalt("proc") == 4 && gestalt("fpu ") == 2 && gestalt("mmu ") == 3)
                #expect(rd8(UInt32(LM_CPUFlag)) == 3 && rd8(UInt32(LM_MMUType)) == 3)
                #expect(rd16(UInt32(LM_SCSIFlags)) == 0xDC00)  // the SE's own bits, an FPU and an MMU
                let env = sysEnv()
                #expect(env.machine == 3 && env.proc == 4 && env.fpu)
            }
        }

        /// A Mac II: a 68020 with a 68881 in 24-bit mode, 8 MB of RAM, the
        /// ROM at $800000, the screen in slot 9, the chips at $F00000 and,
        /// mirrored, at $50F00000.
        @Test func maciiHasTheMacIIsMap() {
            withMachine("macii") {
                launchTestApp()
                #expect(g_cpu!.pointee.model == 68020 && g_cpu!.pointee.fpu == 68881)
                #expect(g_mem_layout == Int32(MEM_LAYOUT_MACII))
                #expect(rd32(UInt32(LM_MemTop)) == 0x80_0000 && rd32(UInt32(LM_ROMBase)) == 0x80_0000)
                #expect(rd32(UInt32(LM_ScrnBase)) == 0x90_0000 && machine_screen_depth() == 8)
                #expect(gestalt("mach") == 6 && gestalt("proc") == 3 && gestalt("fpu ") == 1 && gestalt("mmu ") == 1)
                #expect(rd8(UInt32(LM_CPUFlag)) == 2 && rd16(UInt32(LM_SCSIFlags)) == 0xF400)
                let env = sysEnv()
                #expect(env.machine == 4 && env.proc == 3 && env.fpu)
                // VIA 1's interrupt enable register by both of its addresses.
                g_hw_write!(0xF0_0000 + 14 * 512, 1, 0xA0)
                #expect(g_hw_read!(0x50F0_0000 + 14 * 512, 1) & 0x7F == 0x20)
                #expect(run68k([0x13FC, 0x0000, 0x00F0, 0x1C00, 0xA9F4]) == "ExitToShell")  // MOVE.B #0,$F01C00 (vIER)
                #expect(hw_via_read(14) & 0x7F == 0x20)  // writing 0 clears nothing
                // The SCC at $F04000.
                #expect(run68k([0x1039, 0x00F0, 0x4000, 0xA9F4]) == "ExitToShell")  // MOVE.B $F04000,D0
                // A Mac II's ROM wasn't 32-bit clean: not capable.
                #expect(gestalt("addr") == 0)
            }
        }

        /// --addressing 32 on a Mac II: the flat layout, the chips at $50F00000.
        @Test func maciiIn32BitMode() {
            withMachine("macii") {
                machine_set_addressing(32)
                launchTestApp()
                #expect(!g_mem_24bit && g_cpu!.pointee.model == 68020)
                #expect(rd32(UInt32(LM_VIA)) == 0x50F0_0000)
                #expect(mapped(0x50F0_0000) && !mapped(0xF0_0000))
            }
        }

        /// The default machine is unchanged by all this: a 68040 and the
        /// flat 32-bit space, the JIT allowed; with --cpu 68000 Blue Box's
        /// own 24-bit layout, the chips at $F00000.
        @Test func defaultMachineKeepsItsLayout() {
            launchTestApp(sizeFlags: 0x0080)
            #expect(!g_mem_24bit && g_cpu!.pointee.model == 68040 && !g_hw_jit_off)
            #expect(gestalt("mach") == 35 && rd16(UInt32(LM_SCSIFlags)) == 0xFC00)
            machine_set_cpu(68000)
            defer { machine_set_cpu(68040) }
            launchTestApp()
            #expect(g_mem_layout == Int32(MEM_LAYOUT_24) && rd32(UInt32(LM_MemTop)) == UInt32(GA24_TOP))
            #expect(mapped(0xF0_0000))
            #expect(gestalt("mach") == 5 && sysEnv().machine == 3)
        }
    }
}
