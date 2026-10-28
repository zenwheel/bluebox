import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// SoundLib's MACE compression and the descriptors it exports.
    @Suite struct OS9RestSound {
        init() { Rest.boot() }

        func wave(_ n: Int, phase: Double = 0, amp: Double = 90) -> [UInt8] {
            (0..<n).map { UInt8(128 + Int(amp * sin(Double($0) * 0.07 + phase))) }
        }

        func put(_ bytes: [UInt8]) -> UInt32 {
            let p = Rest.ptr(max(bytes.count, 1))
            for (i, b) in bytes.enumerated() { wr8(p + UInt32(i), b) }
            return p
        }

        func read(_ p: UInt32, _ n: Int) -> [UInt8] { (0..<n).map { rd8(p + UInt32($0)) } }

        func meanError(_ a: [UInt8], _ b: [UInt8]) -> Double {
            zip(a, b).reduce(0.0) { $0 + abs(Double($1.0) - Double($1.1)) } / Double(a.count)
        }

        func roundTrip(_ comp: String, _ exp: String, packetBytes: Int, samples: [UInt8], channels: Int = 1) -> [[UInt8]] {
            let packets = samples.count / (6 * channels)
            let input = put(samples)
            let packed = Rest.ptr(packets * packetBytes * channels)
            let state = Rest.ptr(64)
            for ch in 1...channels {
                Rest.call(comp, [input, packed, UInt32(packets), 0, state, UInt32(channels), UInt32(ch)])
            }
            // each channel on its own through a state block of its own
            var out = [[UInt8]]()
            for ch in 1...channels {
                let decoded = Rest.ptr(packets * 6 * channels)
                Rest.call(exp, [packed, decoded, UInt32(packets), 0, 0, UInt32(channels), UInt32(ch)])
                let all = read(decoded, packets * 6 * channels)
                out.append(stride(from: ch - 1, to: all.count, by: channels).map { all[$0] })
            }
            return out
        }

        @Test func mace3RoundTripsASmoothSound() {
            let src = wave(6 * 60)
            let out = roundTrip("Comp3to1", "Exp1to3", packetBytes: 2, samples: src)[0]
            #expect(out.count == src.count)
            #expect(meanError(src, out) < 8, "mean error \(meanError(src, out))")
        }

        @Test func mace6RoundTripsASmoothSound() {
            let src = wave(6 * 80, amp: 60)
            let out = roundTrip("Comp6to1", "Exp1to6", packetBytes: 1, samples: src)[0]
            #expect(out.count == src.count)
            #expect(meanError(src, out) < 25, "mean error \(meanError(src, out))")
        }

        @Test func silenceStaysQuiet() {
            let src = [UInt8](repeating: 128, count: 6 * 20)
            let out = roundTrip("Comp3to1", "Exp1to3", packetBytes: 2, samples: src)[0]
            #expect(out.allSatisfy { abs(Int($0) - 128) < 4 })
        }

        @Test func stereoChannelsInterleaveByPacket() {
            let packets = 40
            let left = wave(6 * packets, phase: 0), right = wave(6 * packets, phase: 2, amp: 70)
            var interleaved = [UInt8]()
            for p in 0..<packets { for i in 0..<6 { interleaved.append(left[p * 6 + i]); interleaved.append(right[p * 6 + i]) } }
            let out = roundTrip("Comp3to1", "Exp1to3", packetBytes: 2, samples: interleaved, channels: 2)
            #expect(meanError(left, out[0]) < 10 && meanError(right, out[1]) < 10)
            // and not each other's
            #expect(meanError(left, out[1]) > meanError(left, out[0]))
        }

        @Test func theStateCarriesOverACall() {
            let src = wave(6 * 20)
            let input = put(src)
            let whole = Rest.ptr(40), halves = Rest.ptr(40), state = Rest.ptr(64)
            Rest.call("Comp3to1", [input, whole, 20, 0, 0, 1, 1])
            Rest.call("Comp3to1", [input, halves, 10, 0, state, 1, 1])
            Rest.call("Comp3to1", [input + 60, halves + 20, 10, state, state, 1, 1])
            #expect(read(whole, 40) == read(halves, 40))
            // a call without a state, on the second half, begins afresh
            let fresh = Rest.ptr(20)
            Rest.call("Comp3to1", [input + 60, fresh, 10, 0, 0, 1, 1])
            #expect(read(fresh, 20) != read(halves + 20, 20))
        }

        @Test func badArgumentsDoNothing() {
            let out = Rest.ptr(8)
            wr8(out, 0x55)
            Rest.call("Comp3to1", [0, out, 4, 0, 0, 1, 1])
            Rest.call("Comp3to1", [put(wave(24)), out, 0, 0, 0, 1, 1])
            Rest.call("Comp3to1", [put(wave(24)), out, 4, 0, 0, 1, 2])
            Rest.call("Comp6to1", [put(wave(24)), out, 4, 0, 0, 0, 0])
            #expect(rd8(out) == 0x55)
        }

        @Test func underscoredNamesAndDescriptors() {
            for n in ["Comp3to1", "Comp6to1", "Exp1to3", "Exp1to6"] {
                #expect(cfm_export_fn("__" + n) != nil)
                let d = ppc_data_export("g__\(n)RD")
                #expect(ppc_data_export_known("g__\(n)RD") && d != 0)
                #expect(rd16(d) == 0xAAFE) // goMixedModeTrap: a routine descriptor
            }
            #expect(ppc_data_export("g__MACEVersionLongRD") != 0)
            #expect(!ppc_data_export_known("gMChannelDataCompleteRD"))
        }
    }
}
