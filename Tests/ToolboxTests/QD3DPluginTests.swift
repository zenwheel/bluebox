import CCore
import CPUPPC
import Testing
import Toolbox

/// A renderer plug-in written as a program would: its metahandler and
/// methods are native routines standing in for PowerPC code, and what
/// QuickDraw 3D calls them with is logged.
nonisolated(unsafe) var xrLog: [String] = []
nonisolated(unsafe) var xrTriangles: [[Float]] = []
nonisolated(unsafe) var xrMethods: [UInt32: UInt32] = [:]
nonisolated(unsafe) var xrPasses = 0

func xrType(_ s: String) -> UInt32 { s.utf8.reduce(0) { $0 << 8 | UInt32($1) } }

let xrMeta: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c else { return }
    ceReturn(c, xrMethods[ceArg(c, 0)] ?? 0)
}

/// Geometry: the triangle only (everything else broken into triangles).
let xrGeomMeta: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c else { return }
    ceReturn(c, ceArg(c, 0) == xrType("trng") ? xrMethods[xrType("trng")]! : 0)
}

let xrAttrMeta: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c else { return }
    ceReturn(c, ceArg(c, 0) == 5 ? xrMethods[5]! : 0) // the diffuse colour
}

let xrMatrixMeta: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c else { return }
    ceReturn(c, ceArg(c, 0) == xrType("ulwx") ? xrMethods[xrType("ulwx")]! : 0)
}

let xrStartFrame: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c else { return }
    xrLog.append("startFrame")
    ceReturn(c, 1)
}

let xrStartPass: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c else { return }
    xrLog.append(ceArg(c, 2) != 0 && ceArg(c, 3) != 0 ? "startPass" : "startPass without camera or lights")
    ceReturn(c, 1)
}

/// The end of the pass: the image buffer filled with what the triangles'
/// count says (a second pass asked for once).
let xrEndPass: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c else { return }
    xrLog.append("endPass")
    xrPasses += 1
    if xrPasses == 1 {
        ceReturn(c, 1) // kQ3ViewStatusRetraverse
        return
    }
    let view = ceArg(c, 0)
    let out = tb_NewPtr(16, false, true)
    _ = ceCallFrom(c, "Q3View_GetDrawContext", [view, out])
    let dc = rd32(out)
    _ = ceCallFrom(c, "Q3XDrawContext_GetDrawRegion", [dc, out])
    let region = rd32(out)
    if ceCallFrom(c, "Q3XDrawRegion_StartAccessToImageBuffer", [region, 1, out, out + 4]) == 1 {
        let desc = rd32(out), image = rd32(out + 4)
        let w = rd32(desc), h = rd32(desc + 4), rb = rd32(desc + 8)
        xrLog.append("image \(w)x\(h) rowBytes \(rb) pixelSize \(rd32(desc + 12))")
        // Each triangle: a green pixel in the top row.
        for i in 0..<min(xrTriangles.count, Int(w)) { wr32(image + UInt32(4 * i), 0x00FF00) }
        _ = ceCallFrom(c, "Q3XDrawRegion_End", [region])
    }
    _ = ceCallFrom(c, "Q3Object_Dispose", [dc])
    tb_DisposePtr(out)
    ceReturn(c, 0) // kQ3ViewStatusDone
}

let xrTriangle: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c else { return }
    let data = ceArg(c, 3)
    var t: [Float] = []
    for k in 0..<3 { for j in 0..<3 { t.append(Float(bitPattern: rd32(data + UInt32(16 * k + 4 * j)))) } }
    xrTriangles.append(t)
    ceReturn(c, 1)
}

let xrDiffuse: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c else { return }
    let p = ceArg(c, 2)
    xrLog.append(p == 0 ? "diffuse none" : "diffuse \(Float(bitPattern: rd32(p))) \(Float(bitPattern: rd32(p + 4)))")
    ceReturn(c, 1)
}

let xrLocalToWorld: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c else { return }
    xrLog.append("localToWorld x \(Float(bitPattern: rd32(ceArg(c, 2) + 48)))")
    ceReturn(c, 1)
}

let xrNickName: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c else { return }
    let buf = ceArg(c, 0)
    for (i, b) in "Tester".utf8.enumerated() { wr8(buf + UInt32(i), b) }
    wr8(buf + 6, 0)
    if ceArg(c, 2) != 0 { wr32(ceArg(c, 2), 7) }
    ceReturn(c, 1)
}

extension BlueBox.QuickDraw3D {
    func registerTestRenderer() -> UInt32 {
        let tv = { (fn: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void, n: String) in cfm_native_tvector(fn, n) }
        xrMethods = [
            xrType("rdcl"): tv(xrStartFrame, "xrStartFrame"), xrType("rdst"): tv(xrStartPass, "xrStartPass"),
            xrType("rded"): tv(xrEndPass, "xrEndPass"), xrType("rdgm"): tv(xrGeomMeta, "xrGeomMeta"),
            xrType("trng"): tv(xrTriangle, "xrTriangle"), xrType("rdau"): tv(xrAttrMeta, "xrAttrMeta"),
            5: tv(xrDiffuse, "xrDiffuse"), xrType("rdxu"): tv(xrMatrixMeta, "xrMatrixMeta"),
            xrType("ulwx"): tv(xrLocalToWorld, "xrLocalToWorld"), xrType("rdns"): tv(xrNickName, "xrNickName"),
        ]
        let typeOut = Q.words([0])
        let name = Q.words([0, 0, 0, 0])
        for (i, b) in "Test:Renderer".utf8.enumerated() { wr8(name + UInt32(i), b) }
        let cls = Q.call("Q3XObjectHierarchy_RegisterClass",
                         [Self.type("rddr"), typeOut, name, cfm_native_tvector(xrMeta, "xrMeta"), 0, 0, 16])
        #expect(cls != 0)
        return rd32(typeOut)
    }

    /// A renderer a program registers draws the view's frames: started,
    /// told the state, given each triangle (a trimesh broken into them,
    /// and one submitted in immediate mode), asked for a second pass,
    /// and its image buffer goes into the pixmap at the end.
    @Test func rendererPlugIn() {
        launchTestApp()
        _ = cfm_ppc_cpu()
        #expect(Q.call("Q3Initialize") == 1)
        xrLog = []
        xrTriangles = []
        xrPasses = 0
        let type = registerTestRenderer()
        #expect(type != 0)
        let (dc, image) = pixmapContext(16, 16)
        let v = view(dc)
        #expect(Q.call("Q3View_SetRendererByType", [v, type]) == 1)
        let r = Q.words([0])
        #expect(Q.call("Q3View_GetRenderer", [v, r]) == 1)
        #expect(Q.call("Q3Renderer_GetType", [rd32(r)]) == type)
        #expect(Q.call("Q3Renderer_IsInteractive", [rd32(r)]) == 0)
        Q.call("Q3Object_Dispose", [rd32(r)])
        let nick = tb_NewPtr(256, false, true)
        #expect(Q.call("Q3RendererClass_GetNickNameString", [type, nick]) == 1)
        #expect(rd8(nick) == UInt8(ascii: "T") && rd8(nick + 5) == UInt8(ascii: "r"))

        let tm = trimesh()
        let tri = Self.data([Float(0), Float(0), Float(0), 0, Float(1), Float(0), Float(0), 0,
                             Float(0), Float(1), Float(0), 0, 0])
        var status: UInt32 = 1
        #expect(Q.call("Q3View_StartRendering", [v]) == 1)
        repeat {
            #expect(Q.call("Q3Object_Submit", [tm, v]) == 1)
            #expect(Q.call("Q3TranslateTransform_Submit", [Self.data([Float(2), Float(0), Float(0)]), v]) == 1)
            #expect(Q.call("Q3Attribute_Submit", [5, Self.data([Float(0), Float(1), Float(0)]), v]) == 1)
            #expect(Q.call("Q3Triangle_Submit", [tri, v]) == 1)
            status = Q.call("Q3View_EndRendering", [v])
        } while status == 1
        #expect(status == 0)
        #expect(xrLog.first == "startFrame")
        #expect(xrLog.contains("startPass"))
        #expect(xrLog.filter { $0 == "endPass" }.count == 2)
        #expect(xrLog.contains("diffuse 1.0 1.0"))  // the default
        #expect(xrLog.contains("diffuse 0.0 1.0"))  // the green submitted
        #expect(xrLog.contains("localToWorld x 2.0"))
        #expect(xrLog.contains("image 16x16 rowBytes 64 pixelSize 32"))
        // Two passes of a trimesh triangle and the immediate one.
        #expect(xrTriangles.count == 4)
        #expect(xrTriangles.last == [0, 0, 0, 1, 0, 0, 0, 1, 0])
        #expect(px(image, 16, 0, 0) == 0x00FF00 && px(image, 16, 3, 0) == 0x00FF00)
        #expect(px(image, 16, 4, 0) == 0x0000FF) // cleared to the context's colour
        #expect(px(image, 16, 8, 8) == 0x0000FF)
        for obj in [tm, v, dc] { Q.call("Q3Object_Dispose", [obj]) }
        #expect(Q.call("Q3Exit") == 1)
    }

    /// Draw regions and validation: what a renderer asks of the draw
    /// context, and an attribute's data where its set keeps it.
    @Test func drawRegionsAndAttributePointers() {
        launchTestApp()
        _ = cfm_ppc_cpu()
        #expect(Q.call("Q3Initialize") == 1)
        let (dc, _) = pixmapContext(32, 24)
        let out = Q.words([0, 0])
        #expect(Q.call("Q3XDrawContext_GetValidationFlags", [dc, out]) == 1)
        #expect(rd32(out) == 0xFFFF_FFFF) // new: everything to look at
        #expect(Q.call("Q3XDrawContext_ClearValidationFlags", [dc]) == 1)
        #expect(Q.call("Q3XDrawContext_GetValidationFlags", [dc, out]) == 1)
        #expect(rd32(out) == 0)
        #expect(Q.call("Q3DrawContext_SetClearImageMethod", [dc, 0]) == 1)
        #expect(Q.call("Q3XDrawContext_GetValidationFlags", [dc, out]) == 1)
        #expect(rd32(out) != 0) // edited
        #expect(Q.call("Q3XDrawContext_GetDrawRegion", [dc, out]) == 1)
        let region = rd32(out)
        #expect(region != 0)
        #expect(Q.call("Q3XDrawRegion_GetWindowScaleX", [region, out]) == 1 && Q.float(out) == 16)
        #expect(Q.call("Q3XDrawRegion_GetWindowScaleY", [region, out]) == 1 && Q.float(out) == -12)
        #expect(Q.call("Q3XDrawRegion_IsActive", [region, out]) == 1 && rd32(out) == 1)
        #expect(Q.call("Q3XDrawRegion_GetNextRegion", [region, out]) == 1 && rd32(out) == 0)
        #expect(Q.call("Q3XDrawRegion_SetRendererPrivate", [region, 0x1234, 0]) == 1)
        #expect(Q.call("Q3XDrawRegion_GetRendererPrivate", [region, out]) == 1 && rd32(out) == 0x1234)
        #expect(Q.call("Q3XDrawRegion_GetDeviceTransform", [region, out]) == 1)
        #expect(Q.float(rd32(out)) == 16)
        #expect(Q.call("Q3XDrawRegion_GetClipFlags", [region, out]) == 1 && rd32(out) == 0)

        let set = Q.call("Q3AttributeSet_New")
        #expect(Q.call("Q3AttributeSet_Add", [set, 5, Self.data([Float(0.25), Float(0.5), Float(1)])]) == 1)
        let p = Q.call("Q3XAttributeSet_GetPointer", [set, 5])
        #expect(p != 0 && Q.float(p + 4) == 0.5)
        #expect(Q.call("Q3XAttributeSet_GetPointer", [set, 6]) == 0)
        #expect(Q.call("Q3XAttributeSet_GetMask", [set]) == 1 << 4)
        // A class by a type QuickDraw 3D has (Apple's interactive renderer
        // as a plug-in) gives way to ours.
        let name = Q.words([0, 0, 0, 0])
        for (i, b) in "Apple:IR".utf8.enumerated() { wr8(name + UInt32(i), b) }
        #expect(Q.call("EiObjectHierarchy_RegisterClassByType", [Self.type("rddr"), Self.type("ctwn"), name, 0, 0, 0, 0]) == 0)
        for obj in [set, dc] { Q.call("Q3Object_Dispose", [obj]) }
        #expect(Q.call("Q3Exit") == 1)
    }

    /// A NURB patch with a trim loop: a clockwise square in the middle
    /// of its parameters cuts a hole there; the rest is drawn.
    @Test func trimmedNURBPatch() {
        launchTestApp()
        _ = cfm_ppc_cpu()
        #expect(Q.call("Q3Initialize") == 1)
        let (dc, image) = pixmapContext(64, 64)
        let v = view(dc)
        // Bilinear: four corners at z 0, knots 0 0 1 1 both ways.
        let points = Self.data([Float(-1.5), Float(-1.5), Float(0), Float(1), Float(1.5), Float(-1.5), Float(0), Float(1),
                                Float(-1.5), Float(1.5), Float(0), Float(1), Float(1.5), Float(1.5), Float(0), Float(1)])
        let knots = Self.data([Float(0), Float(0), Float(1), Float(1)])
        // A clockwise square from (0.3, 0.3): four order-2 curves.
        func curve(_ a: (Float, Float), _ b: (Float, Float)) -> [UInt32] {
            [2, 2, Self.data([a.0, a.1, Float(1), b.0, b.1, Float(1)]), knots]
        }
        let lo: Float = 0.3, hi: Float = 0.7
        let curves = Q.words(curve((lo, lo), (lo, hi)) + curve((lo, hi), (hi, hi)) + curve((hi, hi), (hi, lo)) +
                             curve((hi, lo), (lo, lo)))
        let loop = Q.words([4, curves])
        let red = Q.call("Q3AttributeSet_New")
        #expect(Q.call("Q3AttributeSet_Add", [red, 5, Self.data([Float(1), Float(0), Float(0)])]) == 1)
        let patch = Q.call("Q3NURBPatch_New", [Q.words([2, 2, 2, 2, points, knots, knots, 1, loop, red])])
        #expect(patch != 0)
        #expect(Q.call("Q3View_StartRendering", [v]) == 1)
        #expect(Q.call("Q3Object_Submit", [patch, v]) == 1)
        #expect(Q.call("Q3View_EndRendering", [v]) == 0)
        #expect(px(image, 64, 32, 32) == 0x0000FF)          // the hole: the clear colour
        #expect(px(image, 64, 18, 18) & 0xFF0000 != 0)      // the patch around it
        #expect(px(image, 64, 46, 46) & 0xFF0000 != 0)
        for obj in [patch, red, v, dc] { Q.call("Q3Object_Dispose", [obj]) }
    }

    /// An attribute a program registers, with its traverse, write and
    /// read-data methods, goes into 3DMF with the set it is in and comes
    /// back into it, in both formats.
    @Test func customAttributesInFiles() {
        launchTestApp()
        _ = cfm_ppc_cpu()
        #expect(Q.call("Q3Initialize") == 1)
        let tv = { (fn: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void, n: String) in cfm_native_tvector(fn, n) }
        xrMethods = [xrType("trvs"): tv(xaTraverse, "xaTraverse"), xrType("writ"): tv(xaWrite, "xaWrite"),
                     xrType("rddt"): tv(xaReadData, "xaReadData")]
        let typeOut = Q.words([0])
        let name = Q.words([0, 0, 0, 0, 0])
        for (i, b) in "Test:Weight".utf8.enumerated() { wr8(name + UInt32(i), b) }
        #expect(Q.call("Q3XAttributeClass_Register", [typeOut, name, 4, cfm_native_tvector(xrMeta, "xaMeta")]) != 0)
        xaType = rd32(typeOut)
        let set = Q.call("Q3AttributeSet_New")
        #expect(Q.call("Q3AttributeSet_Add", [set, 5, Self.data([Float(0), Float(1), Float(0)])]) == 1)
        #expect(Q.call("Q3AttributeSet_Add", [set, xaType, Q.words([0x1234_5678])]) == 1)
        let tri = Q.call("Q3Triangle_New", [Self.data([Float(0), Float(0), Float(0), 0, Float(1), Float(0), Float(0), 0,
                                                       Float(0), Float(1), Float(0), 0, set])])
        #expect(tri != 0)
        let file = BlueBox.QD3DFile()
        for mode: UInt32 in [0, 4] {
            let st = Q.call("Q3MemoryStorage_New", [0, 0])
            let f = Q.call("Q3File_New")
            #expect(Q.call("Q3File_SetStorage", [f, st]) == 1)
            #expect(Q.call("Q3File_OpenWrite", [f, mode]) == 1)
            let view = Q.call("Q3View_New")
            #expect(Q.call("Q3View_StartWriting", [view, f]) == 1)
            #expect(Q.call("Q3Object_Submit", [tri, view]) == 1)
            #expect(Q.call("Q3View_EndWriting", [view]) == 0)
            #expect(Q.call("Q3File_Close", [f]) == 1)
            let buf = tb_NewPtr(12, false, true)
            #expect(Q.call("Q3MemoryStorage_GetBuffer", [st, buf, buf + 4, buf + 8]) == 1)
            let bytes = (0..<rd32(buf + 4)).map { rd8(rd32(buf) + $0) }
            if mode == 4 { #expect(String(decoding: bytes, as: UTF8.self).contains("Test:Weight")) }
            let again = file.readAll(bytes, mode: mode)
            #expect(again.count == 1)
            if let t = again.first {
                let out = Q.words([0])
                #expect(Q.call("Q3Geometry_GetAttributeSet", [t, out]) == 1)
                let got = rd32(out)
                let w = Q.words([0])
                #expect(got != 0 && Q.call("Q3AttributeSet_Get", [got, xaType, w]) == 1 && rd32(w) == 0x1234_5678)
                #expect(Q.call("Q3AttributeSet_Get", [got, 5, Q.words([0, 0, 0])]) == 1)
                if got != 0 { Q.call("Q3Object_Dispose", [got]) }
            }
            for o in again + [f, st, view] { Q.call("Q3Object_Dispose", [o]) }
        }
        for o in [tri, set] { Q.call("Q3Object_Dispose", [o]) }
    }
}

nonisolated(unsafe) var xaType: UInt32 = 0

/// The test attribute's traverse: its four bytes submitted for writing.
let xaTraverse: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c else { return }
    ceReturn(c, ceCallFrom(c, "Q3XView_SubmitWriteData", [ceArg(c, 2), 4, ceArg(c, 1), 0]))
}

let xaWrite: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c else { return }
    ceReturn(c, ceCallFrom(c, "Q3Uns32_Write", [rd32(ceArg(c, 0)), ceArg(c, 1)]))
}

/// Read back and added to the set it is in.
let xaReadData: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c else { return }
    let parent = ceArg(c, 0), file = ceArg(c, 1)
    let w = tb_NewPtr(4, false, true)
    var status = ceCallFrom(c, "Q3Uns32_Read", [w, file])
    if status == 1 { status = ceCallFrom(c, "Q3Set_Add", [parent, xaType, w]) }
    tb_DisposePtr(w)
    ceReturn(c, status)
}
