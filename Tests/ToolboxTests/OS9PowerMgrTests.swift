import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// PowerMgrLib's remaining _PowerDispatch selectors on a desktop:
    /// settings kept, no batteries, no wakeup hardware.
    @Suite struct OS9PowerMgr {
        init() { bootMachine() }

        @Test func settingsAreKept() {
            #expect(ppcExport("GetSleepTimeout", []) == 0)
            ppcExport("SetSleepTimeout", [7])
            #expect(ppcExport("GetSleepTimeout", []) == 7)
            ppcExport("SetHardDiskTimeout", [12])
            #expect(ppcExport("GetHardDiskTimeout", []) == 12)
            ppcExport("SetDimmingTimeout", [3])
            #expect(ppcExport("GetDimmingTimeout", []) == 3)
            #expect(ppcExport("IsAutoSlpControlDisabled", []) & 0xFF == 0)
            ppcExport("AutoSleepControl", [0])
            #expect(ppcExport("IsAutoSlpControlDisabled", []) & 0xFF == 1)
            ppcExport("DimmingControl", [0])
            #expect(ppcExport("IsDimmingControlDisabled", []) & 0xFF == 1)
            ppcExport("SetSpindownDisable", [1])
            #expect(ppcExport("IsSpindownDisabled", []) & 0xFF == 1)
            ppcExport("EnableProcessorCycling", [1])
            #expect(ppcExport("IsProcessorCyclingEnabled", []) & 0xFF == 1)
            ppcExport("SetSCSIDiskModeAddress", [5])
            #expect(ppcExport("GetSCSIDiskModeAddress", []) == 5)
            ppcExport("SetDimSuspendState", [1])
            #expect(ppcExport("GetDimSuspendState", []) & 0xFF == 1)
            // A new run starts again from nothing.
            bootMachine()
            #expect(ppcExport("GetSleepTimeout", []) == 0 && ppcExport("GetHardDiskTimeout", []) == 0)
        }

        @Test func the68kSelectorsAgree() {
            let reason = run68k([
                0x203C, 0x0009, 0x0005, 0xA09E,   // MOVE.L #$00090005,D0; SetHardDiskTimeout(9)
                0x7004, 0xA09E, 0x3E00,           // GetHardDiskTimeout; MOVE.W D0,D7
                0x7006, 0xA09E, 0x1C00,           // HardDiskPowered; MOVE.B D0,D6
                0xA9F4,
            ])
            #expect(reason == "ExitToShell")
            #expect(g_cpu!.pointee.d.7 & 0xFF == 9 && g_cpu!.pointee.d.6 & 0xFF != 0)
            #expect(ppcExport("GetHardDiskTimeout", []) == 9)
        }

        @Test func noBatteriesNoHardware() {
            let rec = tb_NewPtr(16, false, true)
            for i in 0..<16 { wr8(rec + UInt32(i), 0xFF) }
            ppcExport("GetScaledBatteryInfo", [0, rec])
            #expect(rd32(rec) == 0 && rd8(rec + 4) == 0xFF) // a zero record: no battery
            ppcExport("GetBatteryTimes", [0, rec])
            #expect((0..<4).allSatisfy { rd32(rec + UInt32($0 * 4)) == 0 })
            #expect(ppcExport("GetBatteryVoltage", [0]) == 0)
            #expect(ppcExport("GetIntModemInfo", []) == 0)
            #expect(ppcExport("HardDiskPowered", []) & 0xFF == 1)
            #expect(ppcExport("FullProcessorSpeed", []) & 0xFF == 1)
            #expect(ppcExport("MaximumProcessorSpeed", []) == 33 && ppcExport("CurrentProcessorSpeed", []) == 33)
            #expect(ppcErr("GetStartupTimer", [rec]) == -4 && ppcErr("SetStartupTimer", [rec]) == -4)
            #expect(ppcErr("GetSoundMixerState", [rec]) == -4 && ppcErr("SetSoundMixerState", [rec]) == -4)
            // The wakeup time can be set but not acted on.
            wr32(rec, 5000); wr8(rec + 4, 1)
            ppcExport("SetWakeupTimer", [rec])
            let back = tb_NewPtr(8, false, true)
            ppcExport("GetWakeupTimer", [back])
            #expect(rd32(back) == 5000 && rd8(back + 4) == 1)
        }

        @Test func activityAndQueues() {
            wr32(UInt32(LM_Ticks), 1234)
            #expect(ppcErr("UpdateSystemActivity", [1]) == 0) // user activity
            let info = tb_NewPtr(8, false, true)
            wr16(info, 1)
            #expect(ppcErr("GetLastActivity", [info]) == 0 && rd32(info + 2) == 1234)
            wr16(info, 0)
            #expect(ppcErr("GetLastActivity", [info]) == 0 && rd32(info + 2) == 1234) // overall too
            wr16(info, 3)
            #expect(ppcErr("GetLastActivity", [info]) == 0 && rd32(info + 2) == 0)    // no disk activity yet
            #expect(ppcErr("UpdateSystemActivity", [9]) == -50)
            #expect(ppcErr("DelaySystemIdle", []) == 0)

            let e = tb_NewPtr(20, false, true)
            #expect(ppcErr("HardDiskQInstall", [e]) == 0 && ppcErr("HardDiskQRemove", [e]) == 0)
            #expect(ppcErr("HardDiskQRemove", [e]) == -1)
            #expect(ppcErr("PMgrStateQInstall", [e]) == 0 && ppcErr("PMgrStateQRemove", [e]) == 0)
        }
    }
}
