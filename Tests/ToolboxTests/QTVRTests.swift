import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

/// A call to a QuickTimeVRLib export with floats in f1 on (each also
/// taking a word of the argument list, as the PowerPC convention has it):
/// r3 and f1 back.
@discardableResult
private func vrCall(_ name: String, _ args: [UInt32], floats: [Float] = []) -> (r: UInt32, f: Float) {
    guard let fn = cfm_export_fn(name) else {
        Issue.record("no export \(name)")
        return (0, 0)
    }
    if vrUseMachineCPU { // the machine's processor, for calls that run the program's routines
        return vrCallOn(&g_ppc, fn, args, floats, keepStack: true)
    }
    var c = ppc_init_for_test()
    return vrCallOn(&c, fn, args, floats, keepStack: false)
}

nonisolated(unsafe) private var vrUseMachineCPU = false

private func vrCallOn(_ c: inout ppc_cpu, _ fn: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void,
                      _ args: [UInt32], _ floats: [Float], keepStack: Bool) -> (r: UInt32, f: Float) {
    let area = tb_NewPtr(256, false, true)
    withUnsafeMutableBytes(of: &c.gpr) { raw in
        if !keepStack { raw.storeBytes(of: area, toByteOffset: 4, as: UInt32.self) }
        for (i, v) in args.prefix(8).enumerated() { raw.storeBytes(of: v, toByteOffset: (3 + i) * 4, as: UInt32.self) }
    }
    for (i, f) in floats.enumerated() { ppc_set_fpr(&c, Int32(1 + i), Double(f)) }
    fn(&c)
    let r = withUnsafeBytes(of: c.gpr) { $0.load(fromByteOffset: 12, as: UInt32.self) }
    return (r, Float(ppc_fpr(&c, 1)))
}

private func ppc_init_for_test() -> ppc_cpu {
    var c = ppc_cpu()
    ppc_init(&c)
    return c
}

private func getFloat(_ p: UInt32) -> Float { Float(bitPattern: rd32(p)) }
private func bef(_ f: Float) -> [UInt8] { be32(f.bitPattern) }

/// A QT atom: its header (size, type, ID, child count) and its data or children.
private func atom(_ type: String, _ id: UInt32, _ body: [UInt8], children: Int = 0) -> [UInt8] {
    cat(be32(UInt32(20 + body.count)), Array(type.utf8), be32(id), be16(0), be16(children), be32(0), body)
}

/// An atom container's bytes: the header, then the root holding these.
private func container(_ atoms: [[UInt8]]) -> [UInt8] {
    let body = atoms.flatMap { $0 }
    return [UInt8](repeating: 0, count: 12) + atom("sean", 1, body, children: atoms.count)
}

private func handle(_ b: [UInt8]) -> UInt32 {
    let h = tb_NewHandle(Int32(b.count), false, false)
    gput(rd32(h), b, UInt32(b.count))
    return h
}

/// A 'raw ' image description at a size and depth (8 bits: grey, so the
/// hot spot track's pixels are its indices).
private func rawDesc(_ w: Int, _ h: Int, _ depth: UInt16) -> UInt32 {
    handle(cat(be32(86), Array("raw ".utf8), [0, 0, 0, 0, 0, 0], be16(0), [UInt8](repeating: 0, count: 16),
               be16(w), be16(h), be32(72 << 16), be32(72 << 16), be32(0), be16(1),
               [UInt8](repeating: 0, count: 32), be16(Int(depth)), be16(0xFFFF)))
}

/// The test panorama, upright: 360 columns across 360 degrees (column u
/// at pan 360 - u), 100 rows; red is u / 2, green the row.  Hot spot 5
/// covers columns 100 to 139 (pans 221 to 260), rows 20 to 79.
private let W = 360, H = 100
private func pano(_ u: Int, _ v: Int) -> (UInt8, UInt8, UInt8) { (UInt8(u / 2), UInt8(v * 2), 77) }
private func hotID(_ u: Int, _ v: Int) -> UInt8 { (100..<140).contains(u) && (20..<80).contains(v) ? 5 : 0 }

/// A video track of tiles of the panorama as QTVR stores it: turned a
/// quarter turn anticlockwise (stored row r is upright column W-1-r), in
/// 4 tiles of H by W/4.
private func tileTrack(_ movie: Int32, hot: Bool, nodes: Int = 1) -> Int32 {
    let track = tb_NewMovieTrack(movie, Int32(H) << 16, Int32(W / 4) << 16, 0)
    let media = tb_NewTrackMedia(track, Int32(bitPattern: fourCC("vide")), 600, 0, 0)
    #expect(tb_BeginMediaEdits(media) == 0)
    let desc = rawDesc(H, W / 4, hot ? 40 : 32)
    for n in 0..<nodes {
    for k in 0..<4 {
        var b: [UInt8] = []
        for r in 0..<(W / 4) {
            let u = W - 1 - (k * (W / 4) + r)
            for v in 0..<H {
                if hot {
                    b.append(n == 0 ? hotID(u, v) : 0)
                } else {
                    let p = pano(u, v)
                    b += [255, p.0, p.1, n == 0 ? p.2 : 200] // node 2: blue 200
                }
            }
        }
        #expect(tb_AddMediaSample(media, Int32(bitPattern: handle(b)), 0, Int32(b.count), 150,
                                  Int32(bitPattern: desc), 1, 0, 0) == 0)
    }
    }
    #expect(tb_EndMediaEdits(media) == 0)
    #expect(tb_InsertMediaIntoTrack(track, 0, 0, Int32(600 * nodes), 0x10000) == 0)
    tb_SetTrackEnabled(track, 0)
    return track
}

/// A QTVR 2 panorama movie: QTVR track, panorama track, image and hot spot
/// tracks, made with the Movie Toolbox.  With two nodes, node 1's hot spot
/// links to node 2 (whose blue is 200) instead of back to itself.
private func makePanorama(nodes: Int = 1) -> UInt32 {
    let movie = UInt32(bitPattern: tb_NewMovie(1))
    let M = Int32(bitPattern: movie)
    let image = tileTrack(M, hot: false, nodes: nodes), hots = tileTrack(M, hot: true, nodes: nodes)
    // the panorama track: its 'pdat'
    let pdat = cat(be16(2), be16(0), be32(1), be32(1),
                   bef(0), bef(360), bef(-30), bef(30), bef(10), bef(50), bef(90), bef(0), bef(40),
                   be32(UInt32(H)), be32(UInt32(W)), be16(1), be16(4),
                   be32(UInt32(H)), be32(UInt32(W)), be16(1), be16(4), be32(0), be32(0), be32(0))
    let panoTrack = tb_NewMovieTrack(M, 200 << 16, 100 << 16, 0)
    let panoMedia = tb_NewTrackMedia(panoTrack, Int32(bitPattern: fourCC("pano")), 600, 0, 0)
    let panoSample = container([atom("pdat", 1, pdat)])
    for _ in 0..<nodes {
        #expect(tb_AddMediaSample(panoMedia, Int32(bitPattern: handle(panoSample)), 0, Int32(panoSample.count), 600,
                                  Int32(bitPattern: handle(cat(be32(16), be32(0), be32(0), be32(1)))), 1, 0, 0) == 0)
    }
    #expect(tb_InsertMediaIntoTrack(panoTrack, 0, 0, Int32(600 * nodes), 0x10000) == 0)
    // the QTVR track: the world in its description, the node in its sample
    let world = container([
        atom("vrsc", 1, cat(be16(2), be16(0), be32(0), be32(1), be32(0), be32(0), be32(0))),
        atom("vrnp", 1, atom("vrni", 1, atom("nloc", 1, cat(be16(2), be16(0), Array("pano".utf8),
                                                            be32(0), be32(0), be32(0), be32(0))), children: 1),
             children: 1),
    ])
    let hsin = cat(be16(2), be16(0), Array("link".utf8), be32(0), be32(0), be32(0), be32(0), be32(0),
                   bef(0), bef(0), bef(0), bef(0), bef(0), be16(20), be16(100), be16(80), be16(140),
                   be32(0), be32(0), be32(0))
    // a link back to this node, turning to pan 30, tilt 5
    let link = cat(be16(2), be16(0), be32(UInt32(nodes)), be32(0), bef(0), bef(0), bef(0), bef(0), bef(0),
                   be32(3), bef(30), bef(5), bef(0), bef(0), bef(0), bef(0), be32(0), be32(0), be32(0))
    let node = container([
        atom("ndhd", 1, cat(be16(2), be16(0), Array("pano".utf8), be32(1), be32(0), be32(0), be32(0), be32(0))),
        atom("hspa", 1, atom("hots", 5, atom("hsin", 1, hsin) + atom("link", 1, link), children: 2), children: 1),
    ])
    let vrTrack = tb_NewMovieTrack(M, 200 << 16, 100 << 16, 0)
    let vrMedia = tb_NewTrackMedia(vrTrack, Int32(bitPattern: fourCC("qtvr")), 600, 0, 0)
    let vrDesc = handle(cat(be32(UInt32(16 + world.count)), Array("qtvr".utf8), be32(0), be32(1), world))
    #expect(tb_AddMediaSample(vrMedia, Int32(bitPattern: handle(node)), 0, Int32(node.count), 600,
                              Int32(bitPattern: vrDesc), 1, 0, 0) == 0)
    if nodes > 1 {
        let node2 = container([atom("ndhd", 1, cat(be16(2), be16(0), Array("pano".utf8), be32(2), be32(0),
                                                    be32(0), be32(0), be32(0)))])
        #expect(tb_AddMediaSample(vrMedia, Int32(bitPattern: handle(node2)), 0, Int32(node2.count), 600,
                                  Int32(bitPattern: vrDesc), 1, 0, 0) == 0)
    }
    #expect(tb_InsertMediaIntoTrack(vrTrack, 0, 0, Int32(600 * nodes), 0x10000) == 0)
    let idx = tb_NewPtr(4, false, true)
    #expect(tb_AddTrackReference(vrTrack, panoTrack, Int32(bitPattern: fourCC("pano")), idx) == 0)
    #expect(tb_AddTrackReference(panoTrack, image, Int32(bitPattern: fourCC("imgt")), idx) == 0)
    #expect(tb_AddTrackReference(panoTrack, hots, Int32(bitPattern: fourCC("hott")), idx) == 0)
    // shown 200 by 100 at the port's origin
    let box = tb_NewPtr(8, false, true)
    wr16(box + 4, 100)
    wr16(box + 6, 200)
    tb_SetMovieBox(movie, box)
    return movie
}

/// A QTVR 2 object movie: 3 columns (pans 0, 120, 240 of a whole turn) by
/// 2 rows (tilts 30 and -30), each view one 8 by 8 frame whose red is its
/// column and green its row; two view states.
private func makeObject() -> UInt32 {
    let movie = UInt32(bitPattern: tb_NewMovie(1))
    let M = Int32(bitPattern: movie)
    let image = tb_NewMovieTrack(M, 8 << 16, 8 << 16, 0)
    let media = tb_NewTrackMedia(image, Int32(bitPattern: fourCC("vide")), 600, 0, 0)
    let desc = rawDesc(8, 8, 32)
    for state in 0..<2 {
        for row in 0..<2 {
            for col in 0..<3 {
                var b: [UInt8] = []
                for _ in 0..<64 { b += [255, UInt8(col * 100), UInt8(row * 100), UInt8(state * 200)] }
                #expect(tb_AddMediaSample(media, Int32(bitPattern: handle(b)), 0, 256, 60,
                                          Int32(bitPattern: desc), 1, 0, 0) == 0)
            }
        }
    }
    #expect(tb_InsertMediaIntoTrack(image, 0, 0, 720, 0x10000) == 0)
    tb_SetTrackEnabled(image, 0)
    let obji = cat(be16(2), be16(0), be16(1), be16(2), be16(1), be16(2), be32(60), be32(3), be32(2), bef(180),
                   bef(0), bef(360), bef(120), bef(30), bef(-30), bef(30), bef(1), bef(180), bef(180),
                   bef(0), bef(0), bef(1), bef(1), be32(0), be32(1 | 4))
    let objTrack = tb_NewMovieTrack(M, 64 << 16, 64 << 16, 0)
    let objMedia = tb_NewTrackMedia(objTrack, Int32(bitPattern: fourCC("obje")), 600, 0, 0)
    let sample = container([atom("obji", 1, obji)])
    #expect(tb_AddMediaSample(objMedia, Int32(bitPattern: handle(sample)), 0, Int32(sample.count), 720,
                              Int32(bitPattern: handle(cat(be32(16), be32(0), be32(0), be32(1)))), 1, 0, 0) == 0)
    #expect(tb_InsertMediaIntoTrack(objTrack, 0, 0, 720, 0x10000) == 0)
    let world = container([atom("vrsc", 1, cat(be16(2), be16(0), be32(0), be32(1), be32(0), be32(0), be32(0)))])
    let node = container([atom("ndhd", 1, cat(be16(2), be16(0), Array("obje".utf8), be32(1), be32(0), be32(0),
                                               be32(0), be32(0)))])
    let vrTrack = tb_NewMovieTrack(M, 64 << 16, 64 << 16, 0)
    let vrMedia = tb_NewTrackMedia(vrTrack, Int32(bitPattern: fourCC("qtvr")), 600, 0, 0)
    let vrDesc = handle(cat(be32(UInt32(16 + world.count)), Array("qtvr".utf8), be32(0), be32(1), world))
    #expect(tb_AddMediaSample(vrMedia, Int32(bitPattern: handle(node)), 0, Int32(node.count), 720,
                              Int32(bitPattern: vrDesc), 1, 0, 0) == 0)
    #expect(tb_InsertMediaIntoTrack(vrTrack, 0, 0, 720, 0x10000) == 0)
    let idx = tb_NewPtr(4, false, true)
    #expect(tb_AddTrackReference(vrTrack, objTrack, Int32(bitPattern: fourCC("obje")), idx) == 0)
    #expect(tb_AddTrackReference(objTrack, image, Int32(bitPattern: fourCC("imgt")), idx) == 0)
    let box = tb_NewPtr(8, false, true)
    wr16(box + 4, 64)
    wr16(box + 6, 64)
    tb_SetMovieBox(movie, box)
    return movie
}

/// What the program's routines saw.
nonisolated(unsafe) private var interceptAdd: Float = 0
nonisolated(unsafe) private var interceptCancel = false
nonisolated(unsafe) private var overCalls: [(UInt32, UInt32)] = []

/// An intercept routine: (qtvr, msg, refcon, cancel) adds to the angle the
/// record points at, or cancels the call.
private let interceptProc: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c = c else { return }
    let msg = c.pointee.gpr.4, cancel = c.pointee.gpr.6
    let p = rd32(msg + 20)
    wr32(p, (Float(bitPattern: rd32(p)) + interceptAdd).bitPattern)
    wr8(cancel, interceptCancel ? 1 : 0)
    c.pointee.pc = c.pointee.lr
}

/// Node routines: leaving (qtvr, from, to, cancel*, refcon), entering
/// (qtvr, nodeID, refcon).
nonisolated(unsafe) private var nodeLog: [String] = []
nonisolated(unsafe) private var cancelLeaving = false
private let leavingProc: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c = c else { return }
    nodeLog.append("leave \(c.pointee.gpr.4)->\(c.pointee.gpr.5)")
    wr8(c.pointee.gpr.6, cancelLeaving ? 1 : 0)
    c.pointee.gpr.3 = 0
    c.pointee.pc = c.pointee.lr
}
private let enteringProc: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c = c else { return }
    nodeLog.append("enter \(c.pointee.gpr.4)")
    c.pointee.gpr.3 = 0
    c.pointee.pc = c.pointee.lr
}

/// A back buffer routine: (qtvr, drawRect, areaIndex, flagsIn, flagsOut*,
/// refcon) paints its rectangle red in the port it is given.
nonisolated(unsafe) private var backRects: [(Int16, Int16, Int16, Int16)] = []
private let backProc: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c = c else { return }
    let r = c.pointee.gpr.4
    backRects.append((Int16(bitPattern: rd16(r)), Int16(bitPattern: rd16(r + 2)), Int16(bitPattern: rd16(r + 4)),
                      Int16(bitPattern: rd16(r + 6))))
    let rgb = tb_NewPtr(6, false, true)
    wr16(rgb, 0xFFFF)
    tb_RGBForeColor(rgb)
    tb_PaintRect(r)
    wr32(c.pointee.gpr.7, 1) // kQTVRBackBufferFlagDidDraw
    c.pointee.gpr.3 = 0
    c.pointee.pc = c.pointee.lr
}

/// A mouse-over routine: (qtvr, hotSpotID, flags, refcon).
private let overProc: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c = c else { return }
    overCalls.append((c.pointee.gpr.4, c.pointee.gpr.5))
    c.pointee.gpr.3 = 0
    c.pointee.pc = c.pointee.lr
}

/// A QTVR 5 cubic panorama: six 16 by 16 faces (front, right, back, left,
/// top, bottom) whose blue is 40 times the face's number; the front face's
/// left half red.  Stored turned a quarter turn anticlockwise as QTVR does.
private func makeCube() -> UInt32 {
    let movie = UInt32(bitPattern: tb_NewMovie(1))
    let M = Int32(bitPattern: movie)
    let image = tb_NewMovieTrack(M, 16 << 16, 16 << 16, 0)
    let media = tb_NewTrackMedia(image, Int32(bitPattern: fourCC("vide")), 600, 0, 0)
    let desc = rawDesc(16, 16, 32)
    for face in 0..<6 {
        var b: [UInt8] = []
        for r in 0..<16 { // stored row r: upright column 15 - r
            let u = 15 - r
            for _ in 0..<16 { b += [255, face == 0 && u < 8 ? 255 : 0, 0, UInt8(face * 40)] }
        }
        #expect(tb_AddMediaSample(media, Int32(bitPattern: handle(b)), 0, Int32(b.count), 100,
                                  Int32(bitPattern: desc), 1, 0, 0) == 0)
    }
    #expect(tb_InsertMediaIntoTrack(image, 0, 0, 600, 0x10000) == 0)
    tb_SetTrackEnabled(image, 0)
    let pdat = cat(be16(2), be16(0), be32(1), be32(0),
                   bef(0), bef(0), bef(0), bef(0), bef(0), bef(0), bef(0), bef(0), bef(0),
                   be32(16), be32(96), be16(1), be16(6), be32(0), be32(0), be16(0), be16(0), be32(0),
                   Array("cube".utf8), be32(0))
    let cuvw = cat(bef(-180), bef(180), bef(-90), bef(90), bef(10), bef(120), bef(0), bef(0), bef(90))
    let panoTrack = tb_NewMovieTrack(M, 64 << 16, 64 << 16, 0)
    let panoMedia = tb_NewTrackMedia(panoTrack, Int32(bitPattern: fourCC("pano")), 600, 0, 0)
    let sample = container([atom("pdat", 1, pdat), atom("cuvw", 1, cuvw)])
    #expect(tb_AddMediaSample(panoMedia, Int32(bitPattern: handle(sample)), 0, Int32(sample.count), 600,
                              Int32(bitPattern: handle(cat(be32(16), be32(0), be32(0), be32(1)))), 1, 0, 0) == 0)
    #expect(tb_InsertMediaIntoTrack(panoTrack, 0, 0, 600, 0x10000) == 0)
    let world = container([atom("vrsc", 1, cat(be16(2), be16(0), be32(0), be32(1), be32(0), be32(0), be32(0)))])
    let node = container([atom("ndhd", 1, cat(be16(2), be16(0), Array("pano".utf8), be32(1), be32(0), be32(0),
                                               be32(0), be32(0)))])
    let vrTrack = tb_NewMovieTrack(M, 64 << 16, 64 << 16, 0)
    let vrMedia = tb_NewTrackMedia(vrTrack, Int32(bitPattern: fourCC("qtvr")), 600, 0, 0)
    let vrDesc = handle(cat(be32(UInt32(16 + world.count)), Array("qtvr".utf8), be32(0), be32(1), world))
    #expect(tb_AddMediaSample(vrMedia, Int32(bitPattern: handle(node)), 0, Int32(node.count), 600,
                              Int32(bitPattern: vrDesc), 1, 0, 0) == 0)
    #expect(tb_InsertMediaIntoTrack(vrTrack, 0, 0, 600, 0x10000) == 0)
    let idx = tb_NewPtr(4, false, true)
    #expect(tb_AddTrackReference(vrTrack, panoTrack, Int32(bitPattern: fourCC("pano")), idx) == 0)
    #expect(tb_AddTrackReference(panoTrack, image, Int32(bitPattern: fourCC("imgt")), idx) == 0)
    let box = tb_NewPtr(8, false, true)
    wr16(box + 4, 64)
    wr16(box + 6, 64)
    tb_SetMovieBox(movie, box)
    return movie
}

extension BlueBox {
    @Suite struct QTVR {
        /// Every QuickTimeVRLib export is there.
        @Test func exports() {
            for name in ["QTVRGetQTVRInstance", "QTVRSetPanAngle", "QTVRGetHotSpotRegion", "QTVRMouseUpExtended",
                         "QTVRSetBackBufferImagingProc", "QTVRGetCacheMemInfo", "QTVRWrapAndConstrain"] {
                #expect(cfm_export_fn(name) != nil, "\(name)")
            }
        }

        /// A panorama's node, angles in both units, constraints and their
        /// status, coordinates and the view drawn from the cylinder.
        @Test func panorama() {
            qtMachine()
            let movie = makePanorama()
            let track = vrCall("QTVRGetQTVRTrack", [movie, 1]).r
            #expect(track != 0)
            #expect(vrCall("QTVRGetQTVRTrack", [movie, 2]).r == 0)
            let out = tb_NewPtr(16, false, true)
            #expect(vrCall("QTVRGetQTVRInstance", [out, track, 0]).r == 0)
            let vr = rd32(out)
            #expect(vr != 0)
            #expect(vrCall("QTVRGetCurrentNodeID", [vr]).r == 1)
            #expect(vrCall("QTVRGetNodeType", [vr, 1]).r == fourCC("pano"))
            #expect(vrCall("QTVRGetAngularUnits", [vr]).r == 1) // radians
            #expect(vrCall("QTVRSetAngularUnits", [vr, 0]).r == 0)
            // the default view
            #expect(abs(vrCall("QTVRGetPanAngle", [vr]).f - 90) < 0.01)
            #expect(abs(vrCall("QTVRGetFieldOfView", [vr]).f - 40) < 0.01)
            // the field of view is held within the picture's tilt (60 degrees)
            #expect(vrCall("QTVRSetFieldOfView", [vr], floats: [80]).r == 0)
            #expect(abs(vrCall("QTVRGetFieldOfView", [vr]).f - 50) < 0.01)
            #expect(vrCall("QTVRSetFieldOfView", [vr], floats: [20]).r == 0)
            // tilt within the picture less half the view
            vrCall("QTVRSetTiltAngle", [vr], floats: [40])
            #expect(abs(vrCall("QTVRGetTiltAngle", [vr]).f - 20) < 0.01)
            #expect(vrCall("QTVRGetConstraintStatus", [vr]).r & 4 != 0) // can't pan up
            vrCall("QTVRSetTiltAngle", [vr], floats: [0])
            // pan wraps round a whole circle
            vrCall("QTVRSetPanAngle", [vr], floats: [-30])
            #expect(abs(vrCall("QTVRGetPanAngle", [vr]).f - 330) < 0.01)
            let res = tb_NewPtr(4, false, true)
            #expect(vrCall("QTVRWrapAndConstrain", [vr, 0, 0, res], floats: [725]).r == 0)
            #expect(abs(getFloat(res) - 5) < 0.01)
            // radians
            vrCall("QTVRSetAngularUnits", [vr, 1])
            vrCall("QTVRSetPanAngle", [vr], floats: [Float.pi / 2])
            vrCall("QTVRSetAngularUnits", [vr, 0])
            #expect(abs(vrCall("QTVRGetPanAngle", [vr]).f - 90) < 0.01)
            // constraints narrower than the file's
            #expect(vrCall("QTVRSetConstraints", [vr, 2, 0, 0], floats: [15, 30]).r == 0)
            #expect(abs(vrCall("QTVRGetFieldOfView", [vr]).f - 20) < 0.01)
            vrCall("QTVRSetFieldOfView", [vr], floats: [45])
            #expect(abs(vrCall("QTVRGetFieldOfView", [vr]).f - 30) < 0.01)
            let lo = tb_NewPtr(8, false, true)
            vrCall("QTVRGetConstraints", [vr, 2, lo, lo + 4])
            #expect(getFloat(lo) == 15 && getFloat(lo + 4) == 30)
            vrCall("QTVRGetViewingLimits", [vr, 2, lo, lo + 4])
            #expect(getFloat(lo) == 10 && getFloat(lo + 4) == 50)
            #expect(Int16(bitPattern: UInt16(vrCall("QTVRSetConstraints", [vr, 2, 0, 0], floats: [5, 30]).r & 0xFFFF))
                == -30549) // limitReachedErr
            vrCall("QTVRSetConstraints", [vr, 2, 0, 0], floats: [10, 50])
            // picture coordinates: column u is pan 360 - u
            let pt = tb_NewPtr(8, false, true)
            vrCall("QTVRAnglesToCoord", [vr, 0, 0, pt], floats: [100, 0])
            #expect(abs(getFloat(pt) - 260) < 0.01 && abs(getFloat(pt + 4) - 50) < 0.01)
            vrCall("QTVRCoordToAngles", [vr, pt, lo, lo + 4])
            #expect(abs(getFloat(lo) - 100) < 0.01 && abs(getFloat(lo + 4)) < 0.01)
            // drawn: the view's centre shows the column at the pan
            vrCall("QTVRSetFieldOfView", [vr], floats: [30])
            vrCall("QTVRSetPanAngle", [vr], floats: [200])
            #expect(vrCall("QTVRUpdate", [vr, 0]).r == 0)
            let c = tb_NewPtr(6, false, true)
            tb_GetCPixel(100, 50, c)
            #expect(abs(Int(rd16(c) >> 8) - 80) <= 2)        // red: column 160 / 2
            #expect(abs(Int(rd16(c + 2) >> 8) - 100) <= 3)   // green: row 50 * 2
            #expect(rd16(c + 4) >> 8 == 77)
            // a point's angles: right of centre is a smaller pan
            vrCall("QTVRPtToAngles", [vr, 50 << 16 | 150, lo, lo + 4])
            #expect(getFloat(lo) < 200 && getFloat(lo) > 180 && abs(getFloat(lo + 4)) < 0.5)
            vrCall("QTVRPtToAngles", [vr, 10 << 16 | 100, lo, lo + 4])
            #expect(getFloat(lo + 4) > 5) // above centre: tilted up
            tb_DisposeMovie(movie)
        }

        /// Hot spots: found under a point, their type and region, disabled,
        /// listed when visible, and a link triggered.
        @Test func hotSpots() {
            qtMachine()
            let movie = makePanorama()
            let out = tb_NewPtr(16, false, true)
            #expect(vrCall("QTVRGetQTVRInstance", [out, vrCall("QTVRGetQTVRTrack", [movie, 1]).r, 0]).r == 0)
            let vr = rd32(out)
            vrCall("QTVRSetAngularUnits", [vr, 0])
            vrCall("QTVRSetFieldOfView", [vr], floats: [30])
            vrCall("QTVRSetPanAngle", [vr], floats: [240])
            vrCall("QTVRUpdate", [vr, 0])
            let id = tb_NewPtr(4, false, true)
            #expect(vrCall("QTVRPtToHotSpotID", [vr, 50 << 16 | 100, id]).r == 0)
            #expect(rd32(id) == 5)
            vrCall("QTVRPtToHotSpotID", [vr, 50 << 16 | 5, id]) // left of it (pan 267)
            #expect(rd32(id) == 0)
            let type = tb_NewPtr(4, false, true)
            #expect(vrCall("QTVRGetHotSpotType", [vr, 5, type]).r == 0 && rd32(type) == fourCC("link"))
            let list = tb_NewHandle(0, false, false)
            #expect(vrCall("QTVRGetVisibleHotSpots", [vr, list]).r == 1)
            #expect(rd32(rd32(list)) == 5)
            let rgn = tb_NewRgn()
            #expect(vrCall("QTVRGetHotSpotRegion", [vr, 5, rgn]).r == 0)
            #expect(tb_PtInRgn(50 << 16 | 100, rgn) != 0 && tb_PtInRgn(50 << 16 | 5, rgn) == 0)
            // disabled by ID, then all enabled again
            #expect(vrCall("QTVREnableHotSpot", [vr, 0, 5, 0]).r == 0)
            vrCall("QTVRPtToHotSpotID", [vr, 50 << 16 | 100, id])
            #expect(rd32(id) == 0)
            vrCall("QTVREnableHotSpot", [vr, 2, 0, 1])
            vrCall("QTVRPtToHotSpotID", [vr, 50 << 16 | 100, id])
            #expect(rd32(id) == 5)
            // the link: to this node, panned to 30 and tilted to 5
            #expect(vrCall("QTVRTriggerHotSpot", [vr, 5, 0, 0]).r == 0)
            #expect(abs(vrCall("QTVRGetPanAngle", [vr]).f - 30) < 0.01)
            #expect(abs(vrCall("QTVRGetTiltAngle", [vr]).f - 5) < 0.01)
            // a click that stays on the hot spot triggers it too
            vrCall("QTVRSetPanAngle", [vr], floats: [240])
            vrCall("QTVRSetTiltAngle", [vr], floats: [0])
            vrCall("QTVRMouseDown", [vr, 50 << 16 | 100, 0, 0, id, 0])
            #expect(rd32(id) == 5)
            vrCall("QTVRMouseUp", [vr, 50 << 16 | 100, id, 0])
            #expect(abs(vrCall("QTVRGetPanAngle", [vr]).f - 30) < 0.01)
            #expect(vrCall("QTVRGoToNodeID", [vr, 7]).r == UInt32(bitPattern: -30544)) // invalidNodeIDErr
            tb_DisposeMovie(movie)
        }

        /// An object: columns and rows from pan and tilt, wrapping, view
        /// states, and its frame drawn.
        @Test func object() {
            qtMachine()
            let movie = makeObject()
            let out = tb_NewPtr(16, false, true)
            #expect(vrCall("QTVRGetQTVRInstance", [out, vrCall("QTVRGetQTVRTrack", [movie, 1]).r, 0]).r == 0)
            let vr = rd32(out)
            #expect(vrCall("QTVRGetNodeType", [vr, 0]).r == fourCC("obje"))
            vrCall("QTVRSetAngularUnits", [vr, 0])
            #expect(abs(vrCall("QTVRGetPanAngle", [vr]).f - 120) < 0.01)
            #expect(vrCall("QTVRPanToColumn", [vr], floats: [240]).r == 2)
            #expect(vrCall("QTVRTiltToRow", [vr], floats: [-30]).r == 1)
            #expect(abs(vrCall("QTVRColumnToPan", [vr, 1]).f - 120) < 0.01)
            #expect(abs(vrCall("QTVRRowToTilt", [vr, 1]).f + 30) < 0.01)
            #expect(vrCall("QTVRGetViewStateCount", [vr]).r == 2)
            #expect(vrCall("QTVRGetCurrentViewDuration", [vr]).r == 60)
            let c = tb_NewPtr(6, false, true)
            func shown() -> (Int, Int, Int) {
                _ = vrCall("QTVRUpdate", [vr, 0])
                tb_GetCPixel(32, 32, c)
                return (Int(rd16(c) >> 8), Int(rd16(c + 2) >> 8), Int(rd16(c + 4) >> 8))
            }
            #expect(shown() == (100, 0, 0))
            vrCall("QTVRSetPanAngle", [vr], floats: [240])
            vrCall("QTVRSetTiltAngle", [vr], floats: [-30])
            #expect(shown() == (200, 100, 0))
            // wraps: a nudge right from the last column comes to the first
            #expect(vrCall("QTVRNudge", [vr, 0]).r == 0)
            #expect(abs(vrCall("QTVRGetPanAngle", [vr]).f) < 0.01)
            #expect(shown() == (0, 100, 0))
            #expect(vrCall("QTVRSetViewState", [vr, 2, 2]).r == 0) // the current state
            #expect(shown() == (0, 100, 200))
            #expect(vrCall("QTVRSetViewState", [vr, 2, 3]).r == UInt32(bitPattern: -30545))
            let b = tb_NewPtr(2, false, true)
            #expect(vrCall("QTVRGetControlSetting", [vr, 1, b]).r == 0 && rd8(b) == 1) // wraps
            #expect(vrCall("QTVRSetAnimationSetting", [vr, 1, 1]).r == 0)
            #expect(vrCall("QTVRGetAnimationSetting", [vr, 1, b]).r == 0 && rd8(b) == 1)
            #expect(vrCall("QTVRSetViewCurrentTime", [vr, 60]).r == UInt32(bitPattern: -30546))
            let fp = tb_NewPtr(8, false, true)
            #expect(vrCall("QTVRGetViewCenter", [vr, fp]).r == 0 && getFloat(fp) == 4 && getFloat(fp + 4) == 4)
            tb_DisposeMovie(movie)
        }

        /// The program's intercept routine changes or cancels an angle; its
        /// mouse-over routine hears hot spots entered and left.
        @Test func interceptsAndRoutines() {
            qtMachine()
            cfm_reset() // native routines' transition vectors made afresh in this machine
            _ = cfm_ppc_cpu()
            let movie = makePanorama()
            let out = tb_NewPtr(16, false, true)
            #expect(vrCall("QTVRGetQTVRInstance", [out, vrCall("QTVRGetQTVRTrack", [movie, 1]).r, 0]).r == 0)
            let vr = rd32(out)
            g_ppc_mode = true // the routines are PowerPC ones
            vrUseMachineCPU = true
            defer {
                g_ppc_mode = false
                vrUseMachineCPU = false
            }
            vrCall("QTVRSetAngularUnits", [vr, 0])
            let ip = mm_new_descriptor(cfm_native_tvector(interceptProc, "intercept"), 0x3FC0, 1)
            #expect(vrCall("QTVRInstallInterceptProc", [vr, 0x2000, ip, 0, 0]).r == 0)
            interceptAdd = 10
            interceptCancel = false
            vrCall("QTVRSetPanAngle", [vr], floats: [100])
            #expect(abs(vrCall("QTVRGetPanAngle", [vr]).f - 110) < 0.01)
            interceptCancel = true
            vrCall("QTVRSetPanAngle", [vr], floats: [200])
            #expect(abs(vrCall("QTVRGetPanAngle", [vr]).f - 110) < 0.01)
            #expect(vrCall("QTVRInstallInterceptProc", [vr, 0x2000, 0, 0, 0]).r == 0)
            vrCall("QTVRSetPanAngle", [vr], floats: [240])
            #expect(abs(vrCall("QTVRGetPanAngle", [vr]).f - 240) < 0.01)
            // hot spot 5 under the centre at pan 240
            vrCall("QTVRSetFieldOfView", [vr], floats: [30])
            vrCall("QTVRUpdate", [vr, 0])
            let op = mm_new_descriptor(cfm_native_tvector(overProc, "over"), 0x3FE0, 1)
            #expect(vrCall("QTVRSetMouseOverHotSpotProc", [vr, op, 0, 0]).r == 0)
            overCalls = []
            let id = tb_NewPtr(4, false, true)
            vrCall("QTVRMouseEnter", [vr, 50 << 16 | 100, id, 0])
            #expect(rd32(id) == 5)
            vrCall("QTVRMouseWithin", [vr, 50 << 16 | 101, id, 0])
            vrCall("QTVRMouseLeave", [vr, 50 << 16 | 101, 0])
            #expect(overCalls.count == 3)
            #expect(overCalls.map { $0.1 } == [0, 1, 2])
            #expect(overCalls.allSatisfy { $0.0 == 5 })
            tb_DisposeMovie(movie)
        }

        /// Two nodes: a link goes to the other, the program's leaving and
        /// entering routines hear of it (and can stop it), and the
        /// previous node is remembered.
        @Test func nodes() {
            qtMachine()
            cfm_reset()
            _ = cfm_ppc_cpu()
            let movie = makePanorama(nodes: 2)
            let out = tb_NewPtr(16, false, true)
            #expect(vrCall("QTVRGetQTVRInstance", [out, vrCall("QTVRGetQTVRTrack", [movie, 1]).r, 0]).r == 0)
            let vr = rd32(out)
            g_ppc_mode = true
            vrUseMachineCPU = true
            defer {
                g_ppc_mode = false
                vrUseMachineCPU = false
            }
            vrCall("QTVRSetAngularUnits", [vr, 0])
            #expect(vrCall("QTVRGetCurrentNodeID", [vr]).r == 1)
            #expect(vrCall("QTVRGetNodeType", [vr, 2]).r == fourCC("pano"))
            let lp = mm_new_descriptor(cfm_native_tvector(leavingProc, "leaving"), 0xFFE0, 1)
            let ep = mm_new_descriptor(cfm_native_tvector(enteringProc, "entering"), 0x0FE0, 1)
            #expect(vrCall("QTVRSetLeavingNodeProc", [vr, lp, 0, 0]).r == 0)
            #expect(vrCall("QTVRSetEnteringNodeProc", [vr, ep, 0, 0]).r == 0)
            nodeLog = []
            #expect(vrCall("QTVRTriggerHotSpot", [vr, 5, 0, 0]).r == 0)
            #expect(vrCall("QTVRGetCurrentNodeID", [vr]).r == 2)
            #expect(nodeLog == ["leave 1->2", "enter 2"])
            #expect(abs(vrCall("QTVRGetPanAngle", [vr]).f - 30) < 0.01)
            let c = tb_NewPtr(6, false, true)
            vrCall("QTVRUpdate", [vr, 0])
            tb_GetCPixel(100, 50, c)
            #expect(rd16(c + 4) >> 8 == 200) // node 2's picture
            // a leaving routine that cancels keeps the node
            cancelLeaving = true
            #expect(vrCall("QTVRGoToNodeID", [vr, 0x8000_0000]).r == 0) // kQTVRPreviousNode
            #expect(vrCall("QTVRGetCurrentNodeID", [vr]).r == 2)
            cancelLeaving = false
            #expect(vrCall("QTVRGoToNodeID", [vr, 0x8000_0000]).r == 0)
            #expect(vrCall("QTVRGetCurrentNodeID", [vr]).r == 1)
            vrCall("QTVRUpdate", [vr, 0])
            tb_GetCPixel(100, 50, c)
            #expect(rd16(c + 4) >> 8 == 77)
            #expect(vrCall("QTVRGoToNodeID", [vr, 0x8000_0001]).r == 0) // kQTVRDefaultNode
            #expect(vrCall("QTVRGetCurrentNodeID", [vr]).r == 1)
            tb_DisposeMovie(movie)
        }

        /// The program's back buffer routine draws into the panorama: its
        /// paint shows in the view; refreshing brings back the movie's.
        @Test func backBuffer() {
            qtMachine()
            cfm_reset()
            _ = cfm_ppc_cpu()
            let movie = makePanorama()
            let out = tb_NewPtr(16, false, true)
            #expect(vrCall("QTVRGetQTVRInstance", [out, vrCall("QTVRGetQTVRTrack", [movie, 1]).r, 0]).r == 0)
            let vr = rd32(out)
            g_ppc_mode = true
            vrUseMachineCPU = true
            defer {
                g_ppc_mode = false
                vrUseMachineCPU = false
            }
            vrCall("QTVRSetAngularUnits", [vr, 0])
            vrCall("QTVRSetPanAngle", [vr], floats: [200])
            backRects = []
            let bp = mm_new_descriptor(cfm_native_tvector(backProc, "back buffer"), 0x0003_FBE0, 1)
            // the whole buffer, no areas: one call with all of it
            #expect(vrCall("QTVRSetBackBufferImagingProc", [vr, bp, 0, 0, 0]).r == 0)
            #expect(backRects.count == 1)
            #expect(backRects.first.map { $0.2 == Int16(W) && $0.3 == Int16(H) } == true) // turned: W rows of H
            let c = tb_NewPtr(6, false, true)
            vrCall("QTVRUpdate", [vr, 0])
            tb_GetCPixel(100, 50, c)
            #expect(rd16(c) >> 8 == 255 && rd16(c + 2) >> 8 == 0 && rd16(c + 4) >> 8 == 0)
            #expect(vrCall("QTVRSetBackBufferImagingProc", [vr, 0, 0, 0, 0]).r == 0)
            #expect(vrCall("QTVRRefreshBackBuffer", [vr, 0]).r == 0)
            tb_GetCPixel(100, 50, c)
            #expect(rd16(c + 4) >> 8 == 77)
            tb_DisposeMovie(movie)
        }

        /// A cubic panorama: the face the view looks at, the way up.
        @Test func cube() {
            qtMachine()
            let movie = makeCube()
            let out = tb_NewPtr(16, false, true)
            #expect(vrCall("QTVRGetQTVRInstance", [out, vrCall("QTVRGetQTVRTrack", [movie, 1]).r, 0]).r == 0)
            let vr = rd32(out)
            vrCall("QTVRSetAngularUnits", [vr, 0])
            #expect(abs(vrCall("QTVRGetFieldOfView", [vr]).f - 90) < 0.01)
            let c = tb_NewPtr(6, false, true)
            func face(_ pan: Float, _ tilt: Float) -> Int {
                vrCall("QTVRSetPanAngle", [vr], floats: [pan])
                vrCall("QTVRSetTiltAngle", [vr], floats: [tilt])
                vrCall("QTVRUpdate", [vr, 0])
                tb_GetCPixel(32, 32, c)
                return Int(rd16(c + 4) >> 8) / 40
            }
            #expect(face(0, 0) == 0)
            #expect(face(-90, 0) == 1) // turned right
            #expect(face(180, 0) == 2)
            #expect(face(90, 0) == 3)
            #expect(face(0, 90) == 4)  // straight up
            #expect(face(0, -90) == 5)
            // the front face upright: its red half on the left
            _ = face(0, 0)
            tb_GetCPixel(16, 32, c)
            #expect(rd16(c) >> 8 == 255)
            tb_GetCPixel(48, 32, c)
            #expect(rd16(c) >> 8 == 0)
            let g = tb_NewPtr(12, false, true)
            #expect(vrCall("QTVRGetBackBufferSettings", [vr, g, g + 4, g + 6, g + 10]).r == 0)
            #expect(rd32(g) == fourCC("cube"))
            tb_DisposeMovie(movie)
        }

        /// The world and node information come back as atom containers;
        /// settings round-trip.
        @Test func containersAndSettings() {
            qtMachine()
            let movie = makePanorama()
            let out = tb_NewPtr(16, false, true)
            #expect(vrCall("QTVRGetQTVRInstance", [out, vrCall("QTVRGetQTVRTrack", [movie, 1]).r, 0]).r == 0)
            let vr = rd32(out)
            #expect(vrCall("QTVRGetVRWorld", [vr, out]).r == 0)
            let world = rd32(out)
            #expect(qtCall("QTCountChildrenOfType", [world, 0, fourCC("vrnp")]) == 1)
            #expect(vrCall("QTVRGetNodeInfo", [vr, 1, out]).r == 0)
            #expect(qtCall("QTCountChildrenOfType", [rd32(out), 0, fourCC("hspa")]) == 1)
            #expect(vrCall("QTVRGetNodeInfo", [vr, 9, out]).r == UInt32(bitPattern: -30544))
            // interaction and imaging properties
            #expect(vrCall("QTVRSetInteractionProperty", [vr, 3, 8]).r == 0)
            #expect(vrCall("QTVRGetInteractionProperty", [vr, 3, out]).r == 0 && rd32(out) == 8)
            #expect(vrCall("QTVRSetImagingProperty", [vr, 1, 2, 0x100]).r == 0)
            #expect(vrCall("QTVRGetImagingProperty", [vr, 1, 2, out]).r == 0 && rd32(out) == 0x100)
            vrCall("QTVRSetMouseOverTracking", [vr, 0])
            #expect(vrCall("QTVRGetMouseOverTracking", [vr]).r == 0)
            vrCall("QTVRSetVisible", [vr, 0])
            #expect(vrCall("QTVRGetVisible", [vr]).r == 0)
            // objects' calls on a panorama say so
            #expect(vrCall("QTVRSetViewCurrentTime", [vr, 0]).r == UInt32(bitPattern: -30542))
            let fp = tb_NewPtr(8, false, true)
            #expect(vrCall("QTVRGetViewCenter", [vr, fp]).r == UInt32(bitPattern: -30542))
            // the view parameter calls (QTVR 5)
            vrCall("QTVRSetAngularUnits", [vr, 0])
            let f = tb_NewPtr(4, false, true)
            wr32(f, Float(10).bitPattern)
            vrCall("QTVRSetPanAngle", [vr], floats: [100])
            #expect(vrCall("QTVRSetViewParameter", [vr, 0x100, f, 1]).r == 0) // relative
            #expect(vrCall("QTVRGetViewParameter", [vr, 0x100, f, 0, 0]).r == 0 && abs(getFloat(f) - 110) < 0.01)
            let sizes = tb_NewPtr(12, false, true)
            #expect(vrCall("QTVRGetBackBufferMemInfo", [vr, 0, 1, 32, sizes, sizes + 4, sizes + 8]).r == 0)
            #expect(rd32(sizes + 8) == UInt32(W * H * 4))
            tb_DisposeMovie(movie)
        }
    }
}
