import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

/// Every test that touches the guest arena or the machine runs inside this
/// suite, one at a time: there is a single address space per process.
@Suite(.serialized) enum Classic {}

func fourCC(_ s: String) -> UInt32 {
    Array(s.utf8).reduce(0) { ($0 << 8) | UInt32($1) }
}

/// Concatenate byte arrays (cheaper to type-check than long `+` chains).
func cat(_ parts: [UInt8]...) -> [UInt8] { parts.flatMap { $0 } }

func be16(_ v: Int) -> [UInt8] { [UInt8((v >> 8) & 0xFF), UInt8(v & 0xFF)] }
func be32(_ v: UInt32) -> [UInt8] { [UInt8(v >> 24), UInt8((v >> 16) & 0xFF), UInt8((v >> 8) & 0xFF), UInt8(v & 0xFF)] }

struct TestResource {
    var type: String
    var id: Int16
    var name: String? = nil
    var attrs: UInt8 = 0
    var data: [UInt8]
}

/// Build a resource fork in the on-disk format.
func buildFork(_ resources: [TestResource]) -> [UInt8] {
    var data = [UInt8]()
    var offsets = [Int]()
    for r in resources {
        offsets.append(data.count)
        data += be32(UInt32(r.data.count)) + r.data
    }
    // Group by type, keeping first-seen order.
    var types = [String]()
    for r in resources where !types.contains(r.type) { types.append(r.type) }
    var names = [UInt8]()
    var typeList = be16(types.count - 1)
    var refList = [UInt8]()
    let typeListSize = 2 + types.count * 8
    for t in types {
        let members = resources.indices.filter { resources[$0].type == t }
        typeList += cat(Array(t.utf8), be16(members.count - 1), be16(typeListSize + refList.count))
        for i in members {
            let r = resources[i]
            var nameOff = 0xFFFF
            if let n = r.name {
                nameOff = names.count
                names += [UInt8(n.utf8.count)] + Array(n.utf8)
            }
            refList += cat(be16(Int(UInt16(bitPattern: r.id))), be16(nameOff))
            refList += cat([r.attrs], Array(be32(UInt32(offsets[i]))[1...]), be32(0))
        }
    }
    let typeListOffset = 28
    let nameListOffset = typeListOffset + typeList.count + refList.count
    let dataStart = 256
    let mapStart = dataStart + data.count
    let mapLen = nameListOffset + names.count
    let header = cat(be32(UInt32(dataStart)), be32(UInt32(mapStart)), be32(UInt32(data.count)), be32(UInt32(mapLen)))
    let map = cat(header, be32(0), be16(0), be16(0), be16(typeListOffset), be16(nameListOffset),
                  typeList, refList, names)
    return cat(header, [UInt8](repeating: 0, count: dataStart - 16), data, map)
}

/// A 'dcmp' 0 compressed resource around the given compressed body.
func dcmp0Resource(decompressedSize: Int, body: [UInt8]) -> [UInt8] {
    cat([0xA8, 0x9F, 0x65, 0x72], be16(0x12), be16(0x0801), be32(UInt32(decompressedSize)),
        [0x00, 0x00], be16(0), be16(0), body)
}

/// A fresh, empty machine.
func bootMachine() {
    #expect(machine_boot(false))
}

/// Pascal string in guest memory.
func guestPString(_ s: String, at addr: UInt32) {
    let bytes = Array(s.utf8)
    wr8(addr, UInt8(bytes.count))
    for (i, b) in bytes.enumerated() { wr8(addr + 1 + UInt32(i), b) }
}

func memErr() -> Int16 { Int16(bitPattern: rd16(UInt32(LM_MemErr))) }
func resErr() -> Int16 { Int16(bitPattern: rd16(UInt32(LM_ResErr))) }

func tempDir() -> URL {
    let url = FileManager.default.temporaryDirectory.appendingPathComponent("classic-tests-\(UUID().uuidString)")
    try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    return url
}
