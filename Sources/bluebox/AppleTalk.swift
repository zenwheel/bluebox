// `bluebox appletalk`: AppleTalk on its own, with no program running, to
// see what a network looks like from Blue Box: this node's address, the
// router and its zones, NBP lookups, and a name of its own for others to
// find.  For checking a router (TashRouter, jrouter ...) before running
// programs over it.

import Foundation
import CCore
import ForkStore
import Toolbox

/// "object:type@zone" as NBP's packed entity (zone "*" when left out).
private func packedEntity(_ spec: String) -> [UInt8]? {
    var rest = Substring(spec)
    var zone: Substring = "*"
    if let at = rest.lastIndex(of: "@") {
        zone = rest[rest.index(after: at)...]
        rest = rest[..<at]
    }
    guard let colon = rest.firstIndex(of: ":") else { return nil }
    var out = [UInt8]()
    for part in [rest[..<colon], rest[rest.index(after: colon)...], zone] {
        var buf = [UInt8](repeating: 0, count: 33)
        let n = utf8_to_macroman(String(part), &buf, 32)
        guard n > 0 else { return nil }
        out.append(UInt8(n))
        out += buf[0..<n]
    }
    return out
}

/// A lookup tuple (network, node, socket, enumerator, entity) as text.
private func tupleText(_ t: UnsafePointer<UInt8>, _ len: Int) -> String {
    var parts = [String]()
    var k = 5
    for _ in 0..<3 where k < len {
        let n = min(Int(t[k]), len - k - 1)
        parts.append(macRoman((0..<n).map { t[k + 1 + $0] }))
        k += 1 + n
    }
    while parts.count < 3 { parts.append("") }
    let net = Int(t[0]) << 8 | Int(t[1])
    return "\(parts[0]):\(parts[1])@\(parts[2])  network \(net) node \(t[2]) socket \(t[3])"
}

/// A ZIP list from C (char (*)[33], max): the names.
private func zoneNames<T>(_ fn: (UnsafeMutablePointer<T>?, Int32) -> Int32) -> [String] {
    let max = 256
    let raw = UnsafeMutableRawPointer.allocate(byteCount: 33 * max, alignment: 1)
    defer { raw.deallocate() }
    raw.initializeMemory(as: UInt8.self, repeating: 0, count: 33 * max)
    let n = Int(fn(raw.bindMemory(to: T.self, capacity: max), Int32(max)))
    let bytes = raw.bindMemory(to: UInt8.self, capacity: 33 * max)
    return (0..<n).map { k in macRoman((0..<32).map { bytes[k * 33 + $0] }.prefix { $0 != 0 }.map { $0 }) }
}

private final class Answers {
    var lines = [String]()
}

/// bluebox appletalk [--appletalk A] [--ltoudp G:P] [--appletalk-zone Z]
///     [info] [zones] [lookup ENTITY]... [register ENTITY] [--for SECONDS]
func appletalkCommand(_ args: [String]) {
    var actions = [(String, String)]()
    var seconds = 30.0
    var i = 0
    while i < args.count {
        let a = args[i]
        func value() -> String {
            i += 1
            guard i < args.count else { usage() }
            return args[i]
        }
        switch a {
        case "--appletalk":
            let v = value()
            if v == "off" || !machine_set_appletalk(v) { fail("--appletalk expects local, lan, lan:IF, bridge:IF or router:HOST[:PORT]") }
        case "--ltoudp":
            guard machine_set_ltoudp(value()) else { fail("--ltoudp expects GROUP:PORT, GROUP or :PORT") }
        case "--appletalk-zone": machine_set_appletalk_zone(value())
        case "--for":
            guard let s = Double(value()), s >= 0 else { fail("--for expects seconds") }
            seconds = s
        case "info", "zones": actions.append((a, ""))
        case "lookup", "register":
            let v = value()
            guard packedEntity(v) != nil else { fail("\(a) expects object:type[@zone]") }
            actions.append((a, v))
        default: usage()
        }
        i += 1
    }
    if actions.isEmpty { actions = [("info", ""), ("zones", "")] }

    // A machine with nothing in it: AppleTalk needs its memory and its CPU.
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("bluebox-appletalk-\(getpid())")
    try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: dir) }
    machine_set_disk(dir.appendingPathComponent("Macintosh HD").path)
    machine_set_system_folder(dir.appendingPathComponent("System Folder").path)
    guard machine_boot(false) else { fail("the machine did not start") }
    guard at_open() == 0 else { fail("AppleTalk did not open (is the interface there?)") }
    at_router_wait()

    var registered = false
    for (what, arg) in actions {
        switch what {
        case "info":
            var rnet: UInt16 = 0
            var rnode: UInt8 = 0
            var zone = [CChar](repeating: 0, count: 33)
            at_my_zone(&zone)
            let zoneBytes = zone.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }
            print("this node: network \(at_network()) node \(at_node())")
            print(at_router(&rnet, &rnode) ? "router: network \(rnet) node \(rnode)" : "router: none heard")
            print("zone: \(macRoman(zoneBytes))")
        case "zones":
            let names = zoneNames(at_zone_list)
            print(names.isEmpty ? "zones: none (no router)" : "zones (\(names.count)):")
            for name in names { print("  " + name) }
        case "lookup":
            let entity = packedEntity(arg)!
            let answers = Answers()
            let ctx = Unmanaged.passRetained(answers).toOpaque()
            let h = entity.withUnsafeBufferPointer { e in
                at_nbp_start(e.baseAddress, Int32(e.count), 4, 750, { ctx, t, len in
                    guard let ctx, let t else { return }
                    Unmanaged<Answers>.fromOpaque(ctx).takeUnretainedValue().lines.append(tupleText(t, Int(len)))
                }, ctx)
            }
            while h != 0 && at_nbp_busy(h) {
                usleep(5000)
                appletalk_poll()
            }
            at_nbp_stop(h)
            Unmanaged<Answers>.fromOpaque(ctx).release()
            var seen = Set<String>()
            let found = answers.lines.filter { seen.insert($0).inserted }
            print("lookup \(arg): \(found.isEmpty ? "nothing answered" : "\(found.count) found")")
            for line in found { print("  " + line) }
        case "register":
            let entity = packedEntity(arg)!
            let skt = at_open_socket(0, nil)
            let nte = entity.withUnsafeBufferPointer { at_register_name($0.baseAddress, Int32($0.count), UInt8(skt)) }
            if skt == 0 || nte == 0 { fail("could not register \(arg)") }
            print("registered \(arg) on network \(at_network()) node \(at_node()) socket \(skt)")
            registered = true
        default: break
        }
    }
    if registered {
        // Answering lookups for the name meanwhile.
        print("answering lookups for \(Int(seconds)) seconds")
        let until = Date().addingTimeInterval(seconds)
        while Date() < until {
            usleep(5000)
            appletalk_poll()
        }
    }
    exit(0)
}
