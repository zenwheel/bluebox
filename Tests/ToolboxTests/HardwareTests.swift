import CCore
import CPU68K
import Darwin
import Testing
import Toolbox

/// A classic Mac by name, booted for a test; the default machine again after.
func withMachine(_ name: String, _ body: () -> Void) {
    let depth = machine_screen_depth_setting()
    var w: Int32 = 0, h: Int32 = 0
    machine_screen_size(&w, &h)
    #expect(hw_select_machine(name))
    defer {
        _ = hw_select_machine("quadra")
        machine_set_screen(w, h)
        machine_set_screen_depth(depth)
        machine_set_cpu(68040)
    }
    body()
}

private func hwRead(_ a: UInt32, _ size: Int32 = 1) -> UInt32 { g_hw_read!(a, size) }
private func hwWrite(_ a: UInt32, _ v: UInt32, _ size: Int32 = 1) { g_hw_write!(a, size, v) }

/// VIA register addresses from the pointer in low memory, as programs form them.
private func via(_ reg: Int) -> UInt32 { rd32(UInt32(LM_VIA)) + UInt32(reg * 512) }
private enum V {
    static let bufB = 0, dirB = 2, dirA = 3, t1c = 4, t1ch = 5, t1l = 6, t1lh = 7, t2c = 8, t2ch = 9
    static let sr = 10, acr = 11, pcr = 12, ifr = 13, ier = 14, bufA = 15
}

/// The chips as a classic Mac's programs reach them.
extension BlueBox {
    @Suite struct Hardware {
        init() { bootMachine() }

        /// The default machine has its chips at a Quadra's addresses, in low
        /// memory's pointers.
        @Test func defaultMachineChipAddresses() {
            #expect(rd32(UInt32(LM_VIA)) == 0x50F0_0000)
            #expect(rd32(UInt32(LM_SCCRd)) == 0x50F0_4000)
            #expect(rd32(UInt32(LM_IWM)) == 0x50F1_6000)
            #expect(rd16(UInt32(LM_ROM85)) == 0x3FFF)
        }

        /// A 68k program reads and writes the VIA with ordinary instructions.
        @Test func viaFromAProgram() {
            hwWrite(via(V.ier), 0x82)  // enable CA1
            // MOVEA.L LM_VIA,A0; MOVE.B $1C00(A0),D7 (IER); MOVE.B #$FF,$1C00(A0)? no: just read
            let reason = run68k([0x2078, UInt16(LM_VIA), 0x1E28, 0x1C00, 0xA9F4])
            #expect(reason == "ExitToShell")
            #expect(g_cpu!.pointee.d.7 & 0xFF == 0x82)
            hwWrite(via(V.ier), 0x02) // disable again
        }

        /// Timer 2 counts down on the 783,360 Hz clock and raises its flag; a
        /// program polling for it gets there (headless time passes in the poll).
        @Test func timerTwoOneShot() {
            // MOVEA.L LM_VIA,A0; MOVE.B #20,vT2C(A0); CLR.B vT2CH(A0);
            // loop: MOVE.B vIFR(A0),D0; BTST #5,D0; BEQ loop; ExitToShell
            let reason = run68k([0x2078, UInt16(LM_VIA), 0x117C, 20, 0x1000, 0x4228, 0x1200,
                                 0x1028, 0x1A00, 0x0800, 0x0005, 0x67F6, 0xA9F4], max: 5_000_000)
            #expect(reason == "ExitToShell")
            #expect(hwRead(via(V.ifr)) & 0x20 != 0)
            _ = hwRead(via(V.t2c))   // reading the low counter clears it
            #expect(hwRead(via(V.ifr)) & 0x20 == 0)
        }

        /// Timer 1 free-running: the counter moves, the flag comes round again
        /// and clears on a read of the low byte.
        @Test func timerOneFreeRuns() {
            hwWrite(via(V.acr), 0x40)
            hwWrite(via(V.t1c), 0x00)
            hwWrite(via(V.t1ch), 0x10)  // 4096 + 2 clocks a period (about 5 ms)
            machine_skip_time(2_000_000)
            let a = hwRead(via(V.t1ch)) << 8 | hwRead(via(V.t1c))
            #expect(a < 0x1000)
            machine_skip_time(10_000_000)
            #expect(hwRead(via(V.ifr)) & 0x40 != 0)
            _ = hwRead(via(V.t1c))
            #expect(hwRead(via(V.ifr)) & 0x40 == 0)
            #expect(hwRead(via(V.t1l)) == 0 && hwRead(via(V.t1lh)) == 0x10)
            hwWrite(via(V.acr), 0)
        }

        /// The vertical retrace and the one-second interrupt set their flags
        /// as time goes by and clear when written.
        @Test func retraceAndSecondFlags() {
            hwWrite(via(V.ifr), 0x7F)
            #expect(hwRead(via(V.ifr)) & 0x03 == 0)
            machine_skip_time(20_000_000)
            #expect(hwRead(via(V.ifr)) & 0x02 != 0) // CA1: retrace
            hwWrite(via(V.ifr), 0x02)
            #expect(hwRead(via(V.ifr)) & 0x02 == 0)
            machine_skip_time(1_100_000_000)
            #expect(hwRead(via(V.ifr)) & 0x01 != 0) // CA2: one second
            hwWrite(via(V.ifr), 0x7F)
        }

        /// IFR's top bit says an enabled interrupt is pending.
        @Test func interruptSummaryBit() {
            hwWrite(via(V.ifr), 0x7F)
            hwWrite(via(V.ier), 0x7F)  // clear all
            machine_skip_time(20_000_000)
            #expect(hwRead(via(V.ifr)) & 0x80 == 0)
            hwWrite(via(V.ier), 0x82)
            #expect(hwRead(via(V.ifr)) & 0x80 != 0)
            hwWrite(via(V.ier), 0x7F)
            hwWrite(via(V.ifr), 0x7F)
        }

        /// A handler in Lvl1DT (the retrace's entry) runs at interrupt time
        /// when its interrupt is enabled.
        @Test func level1DispatchTableHandler() {
            launchTestApp()
            let flag = tb_NewPtr(2, false, true)
            // MOVEA.L LM_VIA,A0; MOVE.B #2,vIFR(A0); MOVE.W #1,flag; RTS
            let words: [UInt16] = [0x2078, UInt16(LM_VIA), 0x117C, 2, 0x1A00,
                                   0x33FC, 1, UInt16(flag >> 16), UInt16(flag & 0xFFFF), 0x4E75]
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            wr32(UInt32(LM_Lvl1DT) + 4, code)
            hwWrite(via(V.ier), 0x82)
            machine_skip_time(20_000_000)
            time_pump()
            #expect(rd16(flag) == 1)
            hwWrite(via(V.ier), 0x7F)
            wr32(UInt32(LM_Lvl1DT) + 4, 0)
        }

        /// A program that took the level 1 vector itself gets the interrupt,
        /// its RTE returning into the interrupted code.
        @Test func level1VectorHandler() {
            launchTestApp()
            let flag = tb_NewPtr(2, false, true)
            let words: [UInt16] = [0x2078, UInt16(LM_VIA), 0x117C, 2, 0x1A00,
                                   0x33FC, 7, UInt16(flag >> 16), UInt16(flag & 0xFFFF), 0x4E73] // ...; RTE
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            let saved = rd32(0x64)
            wr32(0x64, code)
            hwWrite(via(V.ier), 0x82)
            machine_skip_time(20_000_000)
            let sp = g_cpu!.pointee.a.7
            time_pump()
            #expect(rd16(flag) == 7)
            #expect(g_cpu!.pointee.a.7 == sp)
            hwWrite(via(V.ier), 0x7F)
            wr32(0x64, saved)
        }

        /// The SCC's register-pointer protocol: write the pointer, then the
        /// register; reads and writes of a register count back.
        @Test func sccRegisterPointer() {
            let ctlA = rd32(UInt32(LM_SCCWr)) + 2, rdA = rd32(UInt32(LM_SCCRd)) + 2
            hwWrite(ctlA, 12)     // point at WR12
            hwWrite(ctlA, 10)     // the baud rate's low time constant
            hwWrite(ctlA, 13)
            hwWrite(ctlA, 0)
            hwWrite(ctlA, 12)
            #expect(hwRead(rdA) == 10)  // RR12 reads WR12 back
            hwWrite(ctlA, 13)
            #expect(hwRead(rdA) == 0)
            let rr0 = hwRead(rdA)       // the pointer is back at 0
            #expect(rr0 & 0x04 != 0)    // transmit buffer empty
            #expect(rr0 & 0x01 == 0)    // nothing received
        }

        /// A program sending through the SCC's data register reaches the
        /// serial port; what arrives there is read from the receive register.
        @Test func sccPolledSerialLoopback() {
            #expect(machine_set_serial(0, "pty"))
            defer { _ = machine_set_serial(0, "none") }
            launchTestApp()
            let ctl = rd32(UInt32(LM_SCCWr)) + 2, data = rd32(UInt32(LM_SCCWr)) + 6
            let rctl = rd32(UInt32(LM_SCCRd)) + 2, rdata = rd32(UInt32(LM_SCCRd)) + 6
            func wr(_ reg: UInt32, _ v: UInt32) { hwWrite(ctl, reg); hwWrite(ctl, v) }
            wr(9, 0xC0)      // hardware reset
            wr(4, 0x44)      // x16 clock, 1 stop bit
            wr(12, 10)       // 9600 baud
            wr(13, 0)
            wr(14, 1)        // baud generator on
            wr(3, 0xC1)      // 8 bits, receiver on
            wr(5, 0xEA)      // 8 bits, transmitter on, DTR, RTS
            let master = serial_port_fd(0)
            #expect(master >= 0)
            // the other end of the pty, as a terminal program would hold it
            let fd = open(ptsname(master), O_RDWR | O_NOCTTY | O_NONBLOCK)
            #expect(fd >= 0)
            defer { close(fd) }
            var raw = termios()
            tcgetattr(fd, &raw)
            cfmakeraw(&raw)
            tcsetattr(fd, TCSANOW, &raw)
            hwWrite(data, 0x41)
            hwWrite(data, 0x42)
            var got = [UInt8](repeating: 0, count: 2)
            var n = 0
            for _ in 0..<200 where n < 2 {
                let r = read(fd, &got[n], 2 - n)
                if r > 0 { n += r } else { usleep(5000) }
            }
            #expect(got == [0x41, 0x42])
            var c: UInt8 = 0x5A
            #expect(write(fd, &c, 1) == 1)
            var seen = false
            for _ in 0..<200 where !seen {
                serial_poll()
                if hwRead(rctl) & 1 != 0 { seen = true } else { usleep(5000) }
            }
            #expect(seen)
            #expect(hwRead(rdata) == 0x5A)
            #expect(hwRead(rctl) & 1 == 0)
        }

        /// The IWM with no drive: the status register's sense line reads high.
        @Test func iwmWithoutADrive() {
            let base = rd32(UInt32(LM_IWM))
            _ = hwRead(base + 12 * 512 + 512) // Q6 high
            _ = hwRead(base + 14 * 512)       // Q7 low
            #expect(hwRead(base) & 0x80 != 0)
        }

        /// The mouse and keys programs read straight from low memory.
        @Test func inputGlobalsStayCurrent() {
            launchTestApp()
            tb_FlushEvents(-1, 0)
            machine_post_mouse(120, 80, 1)
            events_sync_lowmem()
            #expect(rd32(UInt32(LM_MTemp)) == (80 << 16 | 120))
            #expect(rd32(UInt32(LM_MouseLocation)) == (80 << 16 | 120))
            #expect(rd8(UInt32(LM_MBState)) == 0)
            machine_post_key(0x61, 0, 0, 1, 0)
            events_sync_lowmem()
            #expect(rd16(0x0184) != 0)
            machine_post_key(0x61, 0, 0, 0, 0)
            machine_post_mouse(120, 80, 0)
            events_sync_lowmem()
            #expect(rd8(UInt32(LM_MBState)) == 0x80)
            tb_FlushEvents(-1, 0)
        }
    }

    /// A Macintosh Plus: the compact Mac's I/O map, screen and sound.
    @Suite struct CompactMac {
        func plus(_ body: () -> Void) {
            withMachine("plus") {
                launchTestApp()
                body()
            }
        }

        @Test func plusAddressesAndIdentity() {
            plus {
                #expect(rd32(UInt32(LM_VIA)) == 0xEFE1FE)
                #expect(rd32(UInt32(LM_SCCRd)) == 0x9FFFF8)
                #expect(rd32(UInt32(LM_SCCWr)) == 0xBFFFF9)
                #expect(rd32(UInt32(LM_IWM)) == 0xDFE1FF)
                #expect(rd16(UInt32(LM_ROM85)) == 0x7FFF)
                var v: UInt32 = 0
                #expect(tb_Gestalt(Int32(bitPattern: fourCC("mach")), &v) == 0 && v == 4)
                #expect(tb_Gestalt(Int32(bitPattern: fourCC("proc")), &v) == 0 && v == 1)
                #expect(tb_Gestalt(Int32(bitPattern: fourCC("sysv")), &v) == 0 && v == 0x0608)
                #expect(tb_Gestalt(Int32(bitPattern: fourCC("evnt")), &v) != 0) // no Apple Events in System 6
                var w: Int32 = 0, h: Int32 = 0
                machine_screen_size(&w, &h)
                #expect(w == 512 && h == 342 && machine_screen_depth() == 1)
            }
        }

        /// The VIA at its hard-coded address, in the 24-bit I/O map.
        @Test func viaAtItsFixedAddress() {
            plus {
                hwWrite(0xEFE1FE + 14 * 512, 0xC0)  // enable T1 and T2
                #expect(hwRead(0xEFE1FE + 14 * 512) == 0xC0)
                // as a 68k program writes it: MOVE.B #$40,$EFFFFE (vBufA)
                run68k([0x13FC, 0x0040, 0x00EF, 0xFFFE, 0xA9F4])
                #expect(hw_via_read(15) & 0 == 0)
            }
        }

        /// The alternate screen page: the video circuitry shows the buffer
        /// below the main one when VIA port A's bit 6 is clear.
        @Test func alternateScreenPage() {
            plus {
                var px = [UInt8](repeating: 0, count: 4)
                let main = UInt32(GA_SCREEN), alt = main - 0x8000
                gmemset(main, 0, 64 * 342)
                gmemset(alt, 0, 64 * 342)
                wr8(alt, 0x80)   // the alternate's top-left pixel is black
                hwWrite(0xEFE1FE + 3 * 512, 0x40)  // port A bit 6 an output
                hwWrite(0xEFE1FE + 15 * 512, 0x40) // main page
                machine_screen_rgba(0, 0, 1, 1, &px)
                #expect(px[0] == 255)
                hwWrite(0xEFE1FE + 15 * 512, 0x00) // alternate page
                #expect(hw_screen_alternate())
                machine_screen_rgba(0, 0, 1, 1, &px)
                #expect(px[0] == 0)
                hwWrite(0xEFE1FE + 15 * 512, 0x40)
            }
        }

        nonisolated(unsafe) static var played: [(frames: Int32, rate: Double)] = []

        /// The sound buffer: 370 samples a retrace, from the main or the
        /// alternate buffer, played at 22.25 kHz when the sound is on.
        @Test func soundBufferPlays() {
            plus {
                Self.played = []
                machine_set_sound_handler({ _, pcm, frames, _, rate, _, _ in
                    if pcm != nil { CompactMac.played.append((frames, rate)) }
                }, nil)
                defer { machine_set_sound_handler(nil, nil) }
                let buf = hw_sound_buffer(false)
                #expect(rd32(UInt32(LM_SoundBase)) == buf)
                #expect(hw_sound_buffer(true) == buf - 0x5C00)
                for i in 0..<370 { wr8(buf + UInt32(i * 2), UInt8(i & 1 == 0 ? 0xC0 : 0x40)) }
                hwWrite(0xEFE1FE + 2 * 512, 0x80)   // port B bit 7 an output
                hwWrite(0xEFE1FE, 0x80)             // sound off
                hwWrite(0xEFE1FE + 3 * 512, 0x0F)
                hwWrite(0xEFE1FE + 15 * 512, 0x4F)  // main buffer, full volume
                machine_skip_time(40_000_000)
                time_pump()
                #expect(Self.played.isEmpty)
                hwWrite(0xEFE1FE, 0x00)             // sound on
                machine_skip_time(40_000_000)
                time_pump()
                #expect(!Self.played.isEmpty)
                #expect(Self.played.allSatisfy { $0.frames == 370 && abs($0.rate - 22254.5454) < 1 })
            }
        }

        /// SCC read and write halves of the I/O map decode alike.
        @Test func sccCompactAddresses() {
            plus {
                hwWrite(0xBFFFF9 + 2, 12)           // A control: point at WR12
                hwWrite(0xBFFFF9 + 2, 0x33)
                hwWrite(0xBFFFF9 + 2, 12)
                #expect(hwRead(0x9FFFF8 + 2) == 0x33)
            }
        }
    }
}
