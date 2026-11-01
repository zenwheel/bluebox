// Desk accessories in a file: the DRVR resources whose names start with
// a NUL, the mark that put them in the Apple menu.

import ForkStore

/// A resource's name (rsrc_entry.name), without the NUL a desk
/// accessory's starts with.
func resourceName(_ raw: UnsafeRawBufferPointer) -> String {
    let rest = raw.first == 0 ? raw.dropFirst() : raw[...]
    return macRomanString(rest.prefix { $0 != 0 })
}

/// The names of a resource fork's desk accessories, without the NUL.
public func deskAccessoryNames(fork: UnsafePointer<UInt8>?, length: Int) -> [String] {
    guard let fork, length > 0 else { return [] }
    var idx = rsrc_index()
    var err = [CChar](repeating: 0, count: 256)
    guard rsrc_parse(fork, length, &idx, &err, err.count) else { return [] }
    defer { rsrc_index_free(&idx) }
    let drvr: UInt32 = 0x4452_5652 // 'DRVR'
    return (0..<idx.count).compactMap { i -> String? in
        let e = idx.entries[i]
        guard e.type == drvr else { return nil }
        return withUnsafeBytes(of: e.name) { raw -> String? in
            guard raw.count > 1, raw[0] == 0, raw[1] != 0 else { return nil }
            return resourceName(raw)
        }
    }
}
