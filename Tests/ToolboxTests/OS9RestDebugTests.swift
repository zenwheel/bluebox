import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// DebugLib: DebugAssert, the components and their options, TaskLevel.
    @Suite struct OS9RestDebug {
        init() {
            bootMachine()
            _ = cfm_ppc_cpu()
        }

        func pstr(_ s: String) -> UInt32 {
            let p = Rest.ptr(256)
            guestPString(s, at: p)
            return p
        }

        func fourCC(_ s: String) -> UInt32 { s.utf8.reduce(0) { $0 << 8 | UInt32($1) } }

        /// A component callback that notes the selector and command it was
        /// given and the setting it was passed, and answers 1 to a get.
        func callback(_ stash: UInt32) -> UInt32 {
            Rest.routine(Rest.stash(stash) + [0x906B_0000, 0x908B_0004, 0x8805_0000, 0x980B_0008, 0x3800_0001,
                                              0x2C04_0001, 0x4082_0008, 0x9805_0000, 0x4E80_0020])
        }

        @Test func taskLevelAtTaskTime() {
            #expect(Rest.call("TaskLevel") == 0)
        }

        @Test func componentsAndOptions() {
            let sig = fourCC("Test"), stash = Rest.ptr(16)
            let cb = callback(stash)
            #expect(Rest.signed("NewDebugComponent", [sig, 0, cb]) == -13885)
            #expect(Rest.signed("NewDebugOption", [sig, 1, pstr("x")]) == -13883) // no such component
            #expect(Rest.signed("NewDebugComponent", [sig, pstr("Test Component"), cb]) == 0)
            #expect(Rest.signed("NewDebugComponent", [sig, pstr("Again"), cb]) == -13881)
            #expect(Rest.signed("NewDebugOption", [sig, 5, pstr("Verbose")]) == 0)
            #expect(Rest.signed("NewDebugOption", [sig, 5, pstr("Verbose")]) == -13882)
            #expect(Rest.signed("NewDebugOption", [sig, 0, pstr("Break")]) == -13882) // 0 is the component's own
            #expect(Rest.signed("NewDebugOption", [sig, 6, 0]) == -13885)
            // a component with no callback has no options
            let bare = fourCC("Bare")
            #expect(Rest.signed("NewDebugComponent", [bare, pstr("Bare"), 0]) == 0)
            #expect(Rest.signed("NewDebugOption", [bare, 1, pstr("x")]) == -13886)

            let s = Rest.ptr(4), name = Rest.ptr(256)
            #expect(Rest.signed("GetDebugComponentInfo", [1, s, name]) == 0)
            #expect(rd32(s) == sig && pstring(at: name) == "Test Component")
            #expect(Rest.signed("GetDebugComponentInfo", [2, s, name]) == 0 && rd32(s) == bare)
            #expect(Rest.signed("GetDebugComponentInfo", [3, s, name]) == -13887)
            #expect(Rest.signed("GetDebugComponentInfo", [0, s, name]) == -13887)

            let sel = Rest.ptr(4), setting = Rest.ptr(2)
            #expect(Rest.signed("GetDebugOptionInfo", [1, sig, sel, name, setting]) == 0)
            #expect(rd32(sel) == 5 && pstring(at: name) == "Verbose" && rd8(setting) == 1) // the callback's answer
            #expect(rd32(stash) == 5 && rd32(stash + 4) == 1) // asked to get option 5
            #expect(Rest.signed("GetDebugOptionInfo", [0, sig, sel, name, setting]) == 0 && rd32(sel) == 0)
            #expect(Rest.signed("GetDebugOptionInfo", [2, sig, sel, name, setting]) == -13887)
            #expect(Rest.signed("GetDebugOptionInfo", [0, fourCC("None"), sel, name, setting]) == -13883)

            #expect(Rest.signed("SetDebugOptionValue", [sig, 5, 0]) == 0)
            #expect(rd32(stash) == 5 && rd32(stash + 4) == 2 && rd8(stash + 8) == 0) // told to set it to 0
            #expect(Rest.signed("SetDebugOptionValue", [sig, 5, 1]) == 0 && rd8(stash + 8) == 1)
            #expect(Rest.signed("SetDebugOptionValue", [sig, 99, 1]) == -13884)
            #expect(Rest.signed("SetDebugOptionValue", [fourCC("None"), 5, 1]) == -13883)

            #expect(Rest.signed("DisposeDebugComponent", [sig]) == 0)
            #expect(Rest.signed("DisposeDebugComponent", [sig]) == -13883)
            #expect(Rest.signed("GetDebugComponentInfo", [1, s, name]) == 0 && rd32(s) == bare)
        }

        @Test func assertionsReachTheHandler() {
            let stash = Rest.ptr(48)
            let handler = Rest.routine(Rest.stash(stash) + [0x906B_0000, 0x908B_0004, 0x90AB_0008, 0x90CB_000C,
                                                            0x90EB_0010, 0x910B_0014, 0x912B_0018,
                                                            0x914B_001C, 0x8181_0038, 0x918B_0020, 0x4E80_0020])
            let assertion = Rest.cstr("x != 0"), exception = Rest.cstr("exit"), file = Rest.cstr("main.c")
            Rest.call("InstallDebugAssertOutputHandler", [handler])
            Rest.call("DebugAssert", [fourCC("Tst "), 7, assertion, exception, 0, file, 42, 0x1234])
            #expect(rd32(stash) == fourCC("Tst ") && rd32(stash + 4) == 7 && rd32(stash + 8) == assertion)
            #expect(rd32(stash + 12) == exception && rd32(stash + 16) == 0 && rd32(stash + 20) == file)
            #expect(rd32(stash + 28) == 0x1234)
            let msg = rd32(stash + 32)
            #expect(msg != 0)
            #expect(pstring(at: msg) == "x != 0 (exit) in main.c at line 42 value 0x1234")
            // none: back to the default, which returns
            wr32(stash, 0)
            Rest.call("InstallDebugAssertOutputHandler", [0])
            Rest.call("DebugAssert", [fourCC("Tst "), 7, assertion, 0, 0, 0, 0, 0])
            #expect(rd32(stash) == 0)
        }
    }
}
