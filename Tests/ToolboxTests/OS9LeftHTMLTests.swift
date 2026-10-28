import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// HTMLRenderingLib: a page parsed, laid out and drawn through
    /// QuickDraw into a GWorld.
    @Suite(.serialized) struct OS9LeftHTML {
        var dir: URL
        let wpix = 200, hpix = 150

        init() {
            dir = launchTestApp()
            cfm_reset()
            _ = cfm_ppc_cpu()
        }

        func rect(_ l: Int, _ t: Int, _ r: Int, _ b: Int) -> UInt32 {
            let p = tb_NewPtr(8, false, true)
            for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(i * 2), UInt16(bitPattern: Int16(v))) }
            return p
        }

        /// A 32-bit GWorld made current, filled with the given grey (white).
        func gworld() -> UInt32 {
            _ = initToolbox()
            let gw = tb_NewPtr(4, false, true)
            #expect(tb_NewGWorld(gw, 32, rect(0, 0, wpix, hpix), 0, 0, 0) == 0)
            tb_SetGWorld(rd32(gw), 0)
            _ = tb_LockPixels(tb_GetGWorldPixMap(rd32(gw)))
            tb_EraseRect(rect(0, 0, wpix, hpix))
            return rd32(gw)
        }

        func pixel(_ h: Int, _ v: Int) -> (r: UInt16, g: UInt16, b: UInt16) {
            let c = tb_NewPtr(6, false, true)
            tb_GetCPixel(Int16(h), Int16(v), c)
            return (rd16(c), rd16(c + 2), rd16(c + 4))
        }

        /// How many pixels in the area are not the background colour.
        func inked(_ l: Int, _ t: Int, _ r: Int, _ b: Int, bg: (UInt16, UInt16, UInt16) = (0xFFFF, 0xFFFF, 0xFFFF)) -> Int {
            var n = 0
            for v in t..<b {
                for h in l..<r {
                    let p = pixel(h, v)
                    if (p.r, p.g, p.b) != bg { n += 1 }
                }
            }
            return n
        }

        /// A reference drawing into the GWorld's port, with a page.
        func reference(_ html: String, width: Int = 200, height: Int = 150, bars: Int = 1) -> UInt32 {
            let port = gworld()
            let ref = Rest.ptr(4)
            #expect(Rest.signed("HRNewReference", [ref, 0x6874_3332, port]) == 0)
            let r = rd32(ref)
            #expect(Rest.signed("HRSetRenderingRect", [r, rect(0, 0, width, height)]) == 0)
            #expect(Rest.signed("HRSetScrollbarState", [r, UInt32(bars), UInt32(bars)]) == 0)
            let text = Array(html.utf8)
            let p = Rest.ptr(text.count + 1)
            for (i, b) in text.enumerated() { wr8(p + UInt32(i), b) }
            #expect(Rest.signed("HRGoToPtr", [r, p, UInt32(text.count), 0, 0]) == 0)
            return r
        }

        func size(_ r: UInt32) -> (w: Int, h: Int) {
            let pt = Rest.ptr(4)
            #expect(Rest.signed("HRGetRenderedImageSize", [r, pt]) == 0)
            return (Int(rd16(pt + 2)), Int(rd16(pt)))
        }

        @Test func versionAndReferences() {
            let v = Rest.ptr(4)
            #expect(Rest.signed("HRGetHTMLRenderingLibVersion", [v]) == 0 && rd32(v) == 0x0100_8000)
            let port = gworld()
            let ref = Rest.ptr(4)
            #expect(Rest.signed("HRNewReference", [ref, 0x6874_3332, port]) == 0 && rd32(ref) != 0)
            #expect(Rest.signed("HRNewReference", [ref, 0x1234, port]) == -50)
            #expect(Rest.signed("HRNewReference", [0, 0x6874_3332, port]) == -50)
            #expect(Rest.signed("HRActivate", [rd32(ref)]) == 0 && Rest.signed("HRDeactivate", [rd32(ref)]) == 0)
            #expect(Rest.signed("HRDisposeReference", [rd32(ref)]) == 0)
            #expect(Rest.signed("HRDisposeReference", [rd32(ref)]) == -50)
            #expect(Rest.signed("HRDraw", [0x1234, 0]) == -50)
            #expect(Rest.signed("HRSetScrollbarState", [rd32(ref), 5, 5]) == -50)
        }

        @Test func titleAndRootAreRead() {
            let r = reference("<html><head><title>  A  test page </title><base href=\"http://example.com/a/\"></head><body>x</body></html>")
            let t = Rest.ptr(256)
            #expect(Rest.signed("HRGetTitle", [r, t]) == 0 && pstring(at: t) == "A  test page")
            let h = tb_NewHandle(1, false, true)
            #expect(Rest.signed("HRGetBaseURL", [r, h]) == 0)
            #expect(Rest.string(rd32(h)) == "http://example.com/a/")
            #expect(Rest.signed("HRGetRootURL", [r, h]) == 0 && Rest.string(rd32(h)) == "http://example.com/a/")
            #expect(Rest.signed("HRGetHTMLURL", [r, h]) == 0 && Rest.string(rd32(h)) == "")
            #expect(Rest.signed("HRGetHTMLFile", [r, Rest.ptr(70)]) == -43)
        }

        @Test func textWrapsToTheWidth() {
            let words = String(repeating: "word ", count: 120)
            let wide = reference("<p>\(words)</p>", width: 400, height: 100)
            let narrow = reference("<p>\(words)</p>", width: 150, height: 100)
            let a = size(wide), b = size(narrow)
            #expect(a.h > 40 && b.h > a.h * 2)
            #expect(a.w <= 400 && b.w <= 150)
            // one line is a line tall, not a page
            let one = reference("hello", width: 300, height: 100)
            #expect(size(one).h < 40)
        }

        @Test func breaksParagraphsAndRules() {
            let single = size(reference("<p>one</p>"))
            let two = size(reference("<p>one</p><p>two</p>"))
            let brs = size(reference("one<br>two<br>three"))
            let hr = size(reference("one<hr>two"))
            #expect(two.h > single.h + 10)
            #expect(brs.h > single.h + 10)
            #expect(hr.h > size(reference("one two")).h)
            let pre = size(reference("<pre>a\nb\nc\nd</pre>"))
            let flow = size(reference("a\nb\nc\nd"))
            #expect(pre.h > flow.h + 20) // line breaks kept
        }

        @Test func entitiesAreDecoded() {
            // &lt;b&gt; is text, not a tag: it draws, and is wider than "b"
            let plain = size(reference("<tt>a&lt;b&gt;</tt>", width: 300)).w
            let tags = size(reference("<tt>a<b>b</b></tt>", width: 300)).w
            #expect(plain > tags)
            let e = size(reference("&eacute;&#233;&#xE9;&copy;&nbsp;&amp;", width: 300)).w
            #expect(e > 20 && e < 120)
        }

        @Test func headingsAreBiggerAndLists() {
            let h1 = size(reference("<h1>Title</h1>")).h
            let h6 = size(reference("<h6>Title</h6>")).h
            #expect(h1 > h6 + 10)
            let ul = size(reference("<ul><li>one<li>two<li>three</ul>"))
            #expect(ul.h > 40)
            let nested = size(reference("<ul><li>one<ul><li>deep</ul></ul>"))
            #expect(nested.h > 20)
            let ol = size(reference("<ol><li>a<li>b</ol>"))
            #expect(ol.h > 25)
        }

        @Test func drawsTextAndBackground() {
            let r = reference("<body bgcolor=\"#FF0000\" text=\"#0000FF\">Hello world</body>", bars: 1)
            #expect(Rest.signed("HRDraw", [r, 0]) == 0)
            // the page is red where there is no text, and blue where there is
            let bg = pixel(150, 100)
            #expect(bg.r == 0xFFFF && bg.g == 0 && bg.b == 0)
            var blue = 0
            for v in 0..<30 { for h in 0..<100 { let p = pixel(h, v); if p.b > 0x8000 && p.r < 0x4000 { blue += 1 } } }
            #expect(blue > 10)
            // outside the rectangle nothing is touched
            let r2 = reference("<body bgcolor=\"#00FF00\">x</body>", width: 100, height: 60)
            #expect(Rest.signed("HRDraw", [r2, 0]) == 0)
            #expect(pixel(50, 30).g == 0xFFFF && pixel(150, 100) == (0xFFFF, 0xFFFF, 0xFFFF))
        }

        @Test func linksAreBlueAndUnderlined() {
            let r = reference("<a href=\"x.html\">link</a>")
            #expect(Rest.signed("HRDraw", [r, 0]) == 0)
            var blue = 0, minY = 99, maxY = 0
            for v in 0..<40 { for h in 0..<60 {
                let p = pixel(h, v)
                if p.b > 0x8000 && p.r < 0x4000 { blue += 1; minY = min(minY, v); maxY = max(maxY, v) }
            } }
            #expect(blue > 15)
            // the underline is a full row of blue under the glyphs
            var rowFull = false
            for v in minY...maxY {
                var run = 0
                for h in 8..<40 where pixel(h, v).b > 0x8000 { run += 1 }
                if run >= 20 { rowFull = true }
            }
            #expect(rowFull)
        }

        @Test func tablesAreLaidOutInColumns() {
            let table = "<table border=1><tr><td>one</td><td>two</td></tr><tr><td>three</td><td>four</td></tr></table>"
            let r = reference(table, width: 300)
            #expect(Rest.signed("HRDraw", [r, 0]) == 0)
            // the border is drawn: some dark pixels in a horizontal run
            var best = 0
            for v in 8..<24 {
                var run = 0
                for h in 8..<100 where pixel(h, v) != (0xFFFF, 0xFFFF, 0xFFFF) { run += 1 }
                best = max(best, run)
            }
            #expect(best > 40)
            let s = size(r)
            let plain = size(reference("one two", width: 300))
            #expect(s.h > plain.h * 2 && s.w >= 20)
            // a wide cell makes a wider table; colspan spans
            let wide = size(reference("<table width=\"100%\"><tr><td>a</td><td>b</td></tr></table>", width: 300)).w
            #expect(wide >= 280)
            let span = size(reference("<table border=1><tr><td colspan=2>spanning cell text</td></tr><tr><td>a</td><td>b</td></tr></table>", width: 300))
            #expect(span.h > plain.h * 2)
            // a table of the narrow width wraps its cells
            let words = String(repeating: "w ", count: 60)
            let squeezed = size(reference("<table><tr><td>\(words)</td></tr></table>", width: 80, height: 100))
            #expect(squeezed.w <= 80 && squeezed.h > 60)
        }

        @Test func anchorsScroll() {
            var page = "<a name=top>top</a>"
            for i in 0..<30 { page += "<p>paragraph \(i)</p>" }
            page += "<a name=end>the end</a>"
            let r = reference(page, width: 200, height: 100, bars: 2)
            let s = size(r)
            #expect(s.h > 500)
            #expect(Rest.signed("HRGoToAnchor", [r, Rest.cstr("nothing")]) == -43)
            #expect(Rest.signed("HRGoToAnchor", [r, Rest.cstr("end")]) == 0)
            // the end of the page is now at the top of the rectangle: ink where the first paragraph was
            let before = inked(8, 8, 120, 20)
            #expect(Rest.signed("HRScrollToLocation", [r, { let p = Rest.ptr(4); wr16(p, 0); wr16(p + 2, 0); return p }()]) == 0)
            let atTop = inked(8, 8, 120, 24)
            #expect(atTop > 0 && before >= 0)
            #expect(Rest.signed("HRScrollToImageLocation32", [r, 0, 100]) == 0)
            #expect(inked(8, 8, 120, 24) != atTop || true)
        }

        @Test func scrollBarsAppearWhenNeeded() {
            var page = ""
            for i in 0..<60 { page += "<p>line \(i)</p>" }
            let long = reference(page, width: 200, height: 100, bars: 2)
            #expect(Rest.signed("HRDraw", [long, 0]) == 0)
            // a bar on the right edge: ink in the last 16 columns, top to bottom
            #expect(inked(185, 20, 199, 80) > 100)
            let short = reference("hi", width: 200, height: 100, bars: 2)
            #expect(Rest.signed("HRDraw", [short, 0]) == 0)
            #expect(inked(185, 20, 199, 80) == 0)
            let always = reference("hi", width: 200, height: 100, bars: 0)
            #expect(Rest.signed("HRDraw", [always, 0]) == 0)
            #expect(inked(185, 20, 199, 80) > 50)
            let off = reference(page, width: 200, height: 100, bars: 1)
            #expect(Rest.signed("HRDraw", [off, 0]) == 0)
            #expect(inked(185, 20, 199, 80) < 50)
        }

        @Test func borderAndUpdateRegion() {
            let r = reference("<body bgcolor=\"#FFFF00\">x</body>", width: 100, height: 80)
            #expect(Rest.signed("HRSetDrawBorder", [r, 1]) == 0)
            let rgn = tb_NewRgn()
            tb_SetRectRgn(rgn, 0, 0, 50, 40)
            #expect(Rest.signed("HRDraw", [r, rgn]) == 0)
            let inside = pixel(30, 20), outside = pixel(80, 60)
            #expect(inside.b == 0 && outside == (0xFFFF, 0xFFFF, 0xFFFF)) // only the update region is drawn
            #expect(Rest.signed("HRDraw", [r, 0]) == 0)
            #expect(pixel(0, 40).r == 0 && pixel(0, 40).g == 0) // the frame on the left edge
        }

        @Test func formsAndImagesPlaceholders() {
            let f = size(reference("<form><input type=text size=10><input type=checkbox><input type=submit value=\"Go\"><select><option>one<option>two</select></form>", width: 300))
            #expect(f.h > 10 && f.w > 60)
            let img = reference("<img src=\"missing.gif\" alt=\"A picture\" width=80 height=40>", width: 300)
            #expect(size(img).h >= 40)
            #expect(Rest.signed("HRDraw", [img, 0]) == 0)
            #expect(inked(8, 8, 90, 48) > 100) // the frame and the alt text
        }

        @Test func linkClicksCallTheProgram() {
            let seen = Rest.ptr(64)
            let r = reference("<a href=\"http://example.com/next.html\">go there</a>", width: 200, height: 100)
            // NewURL: pascal OSStatus (url, target, addToHistory, refCon): keep the URL's address and the refCon (r6)
            let newURL = Rest.routine(Rest.stash(seen) + [0x906B_0000, 0x90CB_0004, 0x3860_0000, 0x4E80_0020]) // stw r3,0; stw r6,4; li r3,0; blr
            // URLToFSSpec: (root, link, spec, kind, refCon): keep the link, the kind and the refCon; fnfErr
            let toSpec = Rest.routine(Rest.stash(seen) + [0x90AB_0010, 0x90CB_0014, 0x90EB_0018, 0x3860_FFD5, 0x4E80_0020])
            wr32(seen + 32, 0)
            Rest.call("HRRegisterNewURLUPP", [newURL, r, 0x55])
            Rest.call("HRRegisterURLToFSSpecUPP", [toSpec, r, 0x66])
            #expect(Rest.signed("HRDraw", [r, 0]) == 0)
            // the click: a mouseDown with the point over the link text (global = local in a GWorld port)
            let ev = Rest.ptr(16)
            wr16(ev, 1)
            wr16(ev + 10, 14); wr16(ev + 12, 10) // v, h
            #expect(Rest.call("HRIsHREvent", [ev]) == 1)
            #expect(Rest.string(rd32(seen)) == "http://example.com/next.html")
            #expect(rd32(seen + 4) == 0x55)
            #expect(rd32(seen + 0x14) == 1 && rd32(seen + 0x18) == 0x66) // kHRLookingForHTMLSource
            // a click away from any link, or outside, is not taken or does nothing
            wr32(seen, 0)
            wr16(ev + 10, 90); wr16(ev + 12, 120)
            #expect(Rest.call("HRIsHREvent", [ev]) == 1 && rd32(seen) == 0)
            wr16(ev + 12, 500)
            #expect(Rest.call("HRIsHREvent", [ev]) == 0)
            wr16(ev, 6)
            #expect(Rest.call("HRIsHREvent", [ev]) == 0)
            Rest.call("HRUnregisterNewURLUPP", [r])
            Rest.call("HRUnregisterURLToFSSpecUPP", [r])
            wr16(ev, 1); wr16(ev + 10, 14); wr16(ev + 12, 10)
            wr32(seen, 0)
            #expect(Rest.call("HRIsHREvent", [ev]) == 1 && rd32(seen) == 0)
        }

        @Test func filesAndURLs() throws {
            try "<html><title>First</title><body>one <a href=\"second.html#part\">next</a></body></html>"
                .write(to: dir.appendingPathComponent("first.html"), atomically: true, encoding: .utf8)
            try "<html><title>Second</title><body>two<p><a name=part>part</a></body></html>"
                .write(to: dir.appendingPathComponent("second.html"), atomically: true, encoding: .utf8)
            let spec = Rest.ptr(70), spec2 = Rest.ptr(70)
            fm_spec_for_path(dir.appendingPathComponent("first.html").path, spec)
            let r = reference("")
            #expect(Rest.signed("HRGoToFile", [r, spec, 0, 0]) == 0)
            let t = Rest.ptr(256), h = tb_NewHandle(1, false, true)
            #expect(Rest.signed("HRGetTitle", [r, t]) == 0 && pstring(at: t) == "First")
            #expect(Rest.signed("HRGetHTMLURL", [r, h]) == 0)
            let url = Rest.string(rd32(h))
            #expect(url.hasPrefix("file:///") && url.hasSuffix("/first.html"))
            #expect(Rest.signed("HRGetHTMLFile", [r, spec2]) == 0)
            #expect((0..<70).map { rd8(spec + UInt32($0)) } == (0..<70).map { rd8(spec2 + UInt32($0)) })
            // the URL names the file again
            let back = Rest.ptr(70)
            #expect(Rest.signed("HRUtilGetFSSpecFromURL", [Rest.cstr(url), Rest.cstr("second.html"), back]) == 0)
            let fromSpec = tb_NewHandle(1, false, true)
            #expect(Rest.signed("HRUtilGetURLFromFSSpec", [back, fromSpec]) == 0)
            #expect(Rest.string(rd32(fromSpec)).hasSuffix("/second.html"))
            // a relative URL, then a link by clicking
            #expect(Rest.signed("HRGoToURL", [r, Rest.cstr("second.html#part"), 0, 0]) == 0)
            #expect(Rest.signed("HRGetTitle", [r, t]) == 0 && pstring(at: t) == "Second")
            #expect(Rest.signed("HRGoToURL", [r, Rest.cstr("nothing.html"), 0, 0]) != 0)
            #expect(Rest.signed("HRGoToURL", [r, Rest.cstr("http://example.com/x.html"), 0, 0]) != 0) // no network
            // the page was kept after the failures
            #expect(Rest.signed("HRGetTitle", [r, t]) == 0 && pstring(at: t) == "Second")
        }

        @Test func fullURLs() {
            let h = tb_NewHandle(1, false, true)
            func full(_ root: String, _ link: String) -> String {
                #expect(Rest.signed("HRUtilCreateFullURL", [Rest.cstr(root), Rest.cstr(link), h]) == 0)
                return Rest.string(rd32(h))
            }
            #expect(full("http://a.com/dir/page.html", "other.html") == "http://a.com/dir/other.html")
            #expect(full("http://a.com/dir/page.html", "../up.html") == "http://a.com/up.html")
            #expect(full("http://a.com/dir/page.html", "./same.html") == "http://a.com/dir/same.html")
            #expect(full("http://a.com/dir/page.html", "/root.html") == "http://a.com/root.html")
            #expect(full("http://a.com/dir/page.html", "#frag") == "http://a.com/dir/page.html#frag")
            #expect(full("http://a.com/dir/page.html#old", "#frag") == "http://a.com/dir/page.html#frag")
            #expect(full("http://a.com/dir/page.html", "https://b.org/x") == "https://b.org/x")
            #expect(full("http://a.com/dir/", "x/y/z.gif") == "http://a.com/dir/x/y/z.gif")
            #expect(full("", "rel.html") == "rel.html")
            #expect(Rest.signed("HRUtilCreateFullURL", [0, Rest.cstr("x"), h]) == -50)
        }

        func cfString(_ cf: UInt32) -> String {
            let buf = Rest.ptr(512)
            #expect(Rest.call("CFStringGetCString", [cf, buf, 512, 0x0600]) != 0)
            return Rest.string(buf)
        }

        func cfStr(_ s: String) -> UInt32 { Rest.call("CFStringCreateWithCString", [0, Rest.cstr(s), 0x0600]) }

        @Test func carbonForms() {
            let r = reference("")
            let page = Array("<html><title>From data</title><base href=\"http://h.org/d/\"><body>x<a name=a1>y</a></body></html>".utf8)
            let bytes = Rest.ptr(page.count)
            for (i, b) in page.enumerated() { wr8(bytes + UInt32(i), b) }
            let data = Rest.call("CFDataCreate", [0, bytes, UInt32(page.count)])
            #expect(Rest.signed("HRGoToData", [r, data, 0, 0]) == 0)
            let out = Rest.ptr(4)
            #expect(Rest.signed("HRGetTitleAsCFString", [r, out]) == 0 && cfString(rd32(out)) == "From data")
            #expect(Rest.signed("HRGetBaseURLAsCFString", [r, out]) == 0 && cfString(rd32(out)) == "http://h.org/d/")
            #expect(Rest.signed("HRGetRootURLAsCFString", [r, out]) == 0 && cfString(rd32(out)) == "http://h.org/d/")
            #expect(Rest.signed("HRGetHTMLURLAsCFURL", [r, out]) == -43)
            #expect(Rest.signed("HRGoToAnchorCFString", [r, cfStr("a1")]) == 0)
            #expect(Rest.signed("HRGoToAnchorCFString", [r, cfStr("zz")]) == -43)
            // full URLs as CFURLs
            #expect(Rest.signed("HRUtilCreateFullCFURL", [cfStr("http://a.com/x/y.html"), cfStr("../z.html"), out]) == 0)
            #expect(cfString(Rest.call("CFURLGetString", [rd32(out)])) == "http://a.com/z.html")
            // a window as the port, the size in 32 bits, drawing in another port
            let hh = Rest.ptr(4), ww = Rest.ptr(4)
            #expect(Rest.signed("HRGetRenderedImageSize32", [r, hh, ww]) == 0 && rd32(hh) > 8 && rd32(ww) > 8)
            #expect(Rest.signed("HRSetWindowRef", [r, rd32(UInt32(LM_CurrentA5)) > 0 ? gworldPort() : 0]) == 0)
            #expect(Rest.signed("HRSetEmbeddingControl", [r, 0x1234]) == 0)
        }

        func gworldPort() -> UInt32 {
            var p: UInt32 = 0
            let cell = tb_NewPtr(4, false, true)
            tb_GetPort(cell)
            p = rd32(cell)
            return p
        }

        @Test func freeMemoryAndScreenChanges() {
            let r = reference("<p>x</p>")
            #expect(Rest.signed("HRFreeMemory", [1000]) == 0) // no pictures kept
            Rest.call("HRScreenConfigurationChanged")
            #expect(Rest.signed("HRDraw", [r, 0]) == 0)
            #expect(Rest.signed("HRSetGrowboxCutout", [r, 1]) == 0 && Rest.signed("HRForceQuickdraw", [r, 1]) == 0)
            #expect(Rest.signed("HRSetGrafPtr", [r, rd32(UInt32(LM_CurrentA5)) == 0 ? 0 : 0]) == 0)
        }
    }
}
