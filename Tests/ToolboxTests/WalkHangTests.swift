import CCore
import CPU68K
import CPUPPC
import Foundation
import ForkStore
import Testing
import Toolbox

extension BlueBox {
    /// Programs the walk of the archive found going round for ever: calls
    /// that answered so that a program's loop never ended.
    @Suite struct WalkHangs {
        @discardableResult
        func call(_ name: String, _ args: [UInt32]) -> UInt32 {
            withUnsafeMutableBytes(of: &g_ppc.gpr) { gpr in
                for (i, a) in args.enumerated() { gpr.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
            }
            guard let fn = cfm_export_fn(name) else {
                Issue.record("no export \(name)")
                return 0
            }
            fn(&g_ppc)
            return withUnsafeBytes(of: g_ppc.gpr) { $0.load(fromByteOffset: 12, as: UInt32.self) }
        }

        func pname(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
            return p
        }

        /// Open Transport lists its ports and then says there are no more:
        /// OTGetIndexedPort says false at the end of them (IPNetMonitor lists
        /// them until it does), and OTFindPort false for a name that isn't one.
        @Test func openTransportPortListEnds() {
            bootMachine()
            _ = cfm_ppc_cpu()
            let record = tb_NewPtr(400, false, true)
            var n = 0
            while call("OTGetIndexedPort", [record, UInt32(n)]) & 0xFF != 0 && n < 100 { n += 1 }
            #expect(n >= 1 && n < 100)
            let name = pname("nosuchport")
            #expect(call("OTFindPort", [record, name + 1]) & 0xFF == 0)
        }

        /// The walker dismisses dialogs by windowKind: a document window
        /// with a handle where a dialog's item list would be (Tex-Edit
        /// Plus's) is not one, and keys typed into it went on for ever.
        @Test func walkerTakesOnlyDialogsForDialogs() {
            let items = ditl([ditlItem(4, (160, 220, 180, 290), Array("OK".utf8))])
            launchTestApp([TestResource(type: "DLOG", id: 200, data: dlog(items: 200)),
                           TestResource(type: "DITL", id: 200, data: items)])
            _ = initToolbox()
            let r = tb_NewPtr(8, false, true)
            for (i, v) in [40, 10, 300, 500].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            let storage = tb_NewPtr(400, false, true)
            let title = tb_NewPtr(8, false, true)
            let w = tb_NewWindow(storage, r, title, 1, 0, 0xFFFF_FFFF, 1, 0)
            wr32(w + UInt32(DialogRecord_items_off), tb_NewHandle(64, false, true)) // the program's own data
            #expect(tb_FrontWindow() == w && dialog_in_front() == 0)
            let d = tb_GetNewDialog(200, 0, 0xFFFF_FFFF)
            #expect(tb_FrontWindow() == d && dialog_in_front() == d)
            tb_DisposeDialog(d)
        }

        /// PBCatSearch on a volume that isn't there is nsvErr: VISE
        /// installers search vRefNum -1, -2 and on until one isn't, and
        /// found every one there.
        @Test func catSearchOfNoVolumeIsAnError() {
            launchTestApp()
            let pb = tb_NewPtr(80, false, true)
            let matches = tb_NewPtr(70 * 10, false, true)
            let i1 = tb_NewPtr(108, false, true), i2 = tb_NewPtr(108, false, true)
            wr32(pb + 24, matches)
            wr32(pb + 28, 10)
            wr32(pb + 36, 8)                     // fsSBFlAttrib: attributes, masked to none
            wr32(pb + 40, i1)
            wr32(pb + 44, i2)
            wr16(pb + 22, UInt16(bitPattern: -1))
            let err = tb_PBCatSearch(pb, false)
            #expect((err == 0 || err == -39) && rd32(pb + 32) > 0)   // the boot volume's entries
            wr32(pb + 52, 0)
            wr16(pb + 22, UInt16(bitPattern: -2))
            #expect(tb_PBCatSearch(pb, false) == -35)   // nsvErr
        }

        /// A VBL task runs over an interrupt's stack: the handler's saved
        /// registers, then the interrupted SR and PC.  A task that changes
        /// the PC there sends the interrupted code on elsewhere (MPW Shell
        /// waits in BRA.S * for its VBL task to step it past the loop).
        @Test func vblTaskCanChangeTheInterruptedPC() {
            bootMachine()
            let cpu = g_cpu!
            let loop = tb_NewPtr(4, false, false)
            wr16(loop, 0x60FE)                                   // BRA.S *
            wr16(loop + 2, 0x4E71)                               // NOP
            let after = loop + 2
            let code = tb_NewPtr(16, false, false)
            let words: [UInt16] = [
                0x206F, 0x0026,                                  // MOVEA.L 38(A7),A0: the interrupted PC
                0x0C50, 0x60FE,                                  // CMPI.W #$60FE,(A0): waiting in BRA.S *?
                0x6604,                                          // BNE.S +4
                0x54AF, 0x0026,                                  // ADDQ.L #2,38(A7): step past it
                0x4E75,                                          // RTS
            ]
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            let v = tb_NewPtr(Int32(VBLTask_SIZEOF), false, true)
            wr16(v + 4, 1)                                       // vType
            wr32(v + UInt32(VBLTask_vblAddr_off), code)
            wr16(v + UInt32(VBLTask_vblCount_off), 1)
            #expect(tb_VInstall(v) == 0)
            let sp = tb_NewPtr(4096, false, true) + 4000
            cpu.pointee.a.7 = sp
            cpu.pointee.a.0 = 0x1234
            cpu.pointee.pc = loop
            time_vbl_task()
            #expect(cpu.pointee.pc == after)
            #expect(cpu.pointee.a.7 == sp && cpu.pointee.a.0 == 0x1234)
            #expect(tb_VRemove(v) == 0)
        }

        /// A resource search ends at a TopMapHndl that holds no map: one a
        /// program wrote over led the chain round through low memory.
        @Test func resourceSearchEndsAtABrokenChain() {
            launchTestApp()
            let top = rd32(UInt32(LM_TopMapHndl))
            let empty = tb_NewHandle(0, false, true)
            tb_EmptyHandle(empty)                                // master pointer nil
            let vector = rd32(0x10)
            wr32(0x10, empty)                                    // its "next", read from low memory: itself
            wr32(UInt32(LM_TopMapHndl), empty)
            #expect(tb_GetResource(Int32(bitPattern: fourCC("STR#")), 129) == 0)
            wr32(UInt32(LM_TopMapHndl), top)
            wr32(0x10, vector)
        }

        /// An alert asking for another depth ends the run to start it again
        /// at that depth; what the program added to its resource files is
        /// written first, as when it quits (ChordLab's preferences).
        @Test func depthRelaunchWritesResourceFiles() {
            let dir = launchTestApp()
            let spec = tb_NewPtr(70, false, true)
            _ = tb_FSMakeFSSpec(0, 0, pname("Prefs"), spec)
            tb_FSpCreateResFile(spec, Int32(bitPattern: fourCC("CRea")), Int32(bitPattern: fourCC("pref")), 0)
            let rn = tb_FSpOpenResFile(spec, 3)
            let h = tb_NewHandle(16, false, true)
            tb_AddResource(h, Int32(bitPattern: fourCC("StuP")), 0, pname(""))
            #expect(tb_ResError() == 0)
            machine_set_depth_alerts(true)
            defer { machine_set_depth_alerts(false) }
            #expect(depth_alert("This application needs 256 colors to work properly."))
            g_cpu!.pointee.halted = false
            var f = mac_file()
            var err = [CChar](repeating: 0, count: 256)
            #expect(appledouble_read(dir.appendingPathComponent("Prefs").path, &f, &err, err.count))
            #expect(f.rsrc_len > 286)                            // the map and the new resource
            mac_file_free(&f)
            tb_CloseResFile(rn)
        }

        /// A fork longer than HFS allows is a full disk: ResKnife writes
        /// its preferences with a count of $F0800000, and SetEOF and
        /// FSWrite past 2 GB went round for ever making room.
        @Test func forksPastTwoGigabytesAreAFullDisk() {
            launchTestApp()
            let spec = tb_NewPtr(70, false, true)
            _ = tb_FSMakeFSSpec(0, 0, pname("Big"), spec)
            #expect(tb_FSpCreate(spec, Int32(bitPattern: fourCC("CRea")), Int32(bitPattern: fourCC("TEXT")), 0) == 0)
            let ref = tb_NewPtr(2, false, true)
            #expect(tb_FSpOpenDF(spec, 3, ref) == 0)
            let pb = tb_NewPtr(80, false, true)
            wr16(pb + 24, rd16(ref))                             // ioRefNum
            wr32(pb + 28, 0xFFFF_FFF0)                           // ioMisc: the new length
            #expect(tb_PBSetEOF(pb, false) == -34)               // dskFulErr
            let buf = tb_NewPtr(16, false, true)
            wr32(pb + 32, buf)                                   // ioBuffer
            wr32(pb + 36, 0x9000_0000)                           // ioReqCount
            wr16(pb + 44, 1)                                     // ioPosMode: fsFromStart
            wr32(pb + 46, 0)                                     // ioPosOffset
            #expect(tb_PBWrite(pb, false) == -34)
            wr32(pb + 28, 100)
            #expect(tb_PBSetEOF(pb, false) == 0)
        }

        /// OTUtilityLib's string and memory helpers do what they say
        /// (IPNetRouter compares names with OTStrEqual and OTMemcmp, which
        /// said "not supported", taken as true).
        @Test func openTransportStringHelpers() {
            bootMachine()
            _ = cfm_ppc_cpu()
            func cstr(_ s: String) -> UInt32 {
                let p = tb_NewPtr(64, false, true)
                for (i, b) in Array(s.utf8).enumerated() { wr8(p + UInt32(i), b) }
                return p
            }
            func read(_ p: UInt32) -> String {
                var bytes: [UInt8] = []
                var i: UInt32 = 0
                while rd8(p + i) != 0 { bytes.append(rd8(p + i)); i += 1 }
                return String(decoding: bytes, as: UTF8.self)
            }
            let a = cstr("enet"), b = cstr("enet"), d = cstr("")
            #expect(call("OTStrEqual", [a, b]) & 0xFF != 0)
            #expect(call("OTStrEqual", [a, cstr("ene")]) & 0xFF == 0)
            #expect(call("OTMemcmp", [a, b, 4]) & 0xFF != 0)
            #expect(call("OTMemcmp", [a, cstr("enXt"), 4]) & 0xFF == 0)
            call("OTStrCopy", [d, a])
            call("OTStrCat", [d, cstr("0")])
            #expect(read(d) == "enet0")
        }
    }
}
