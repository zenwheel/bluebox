import CCore
import CPU68K
import Testing
import Toolbox

/// Our own AppleScript (M14): compiled scripts in AppleScript 1.1's FasLoad
/// format, built here from the format (never Apple's compiled scripts),
/// loaded and run by our interpreter, with display dialog answered as a
/// headless run answers it and tell refused for programs that aren't Blue
/// Box runs.
extension BlueBox {
    @Suite struct AppleScript {
        // ---- A writer for the FasLoad format ----

        /// An object in a compiled script: its kind and what it holds.
        indirect enum Fas {
            case null
            case int(Int)                    // small integers, in the header
            case long(Int32)
            case real(Double)
            case text(String)
            case ident(String)
            case const4(String)              // a class, property or type
            case const8(String, UInt32)      // an enumerator: its type and value
            case event(String, String)       // a command: its class and ID
            case code([UInt8])
            case block(Int, [Fas])           // a value block of a type
            case pointers([Fas])
            case list([Fas])
            case record([(Fas, Fas)])
            case app(name: String, signature: String)
        }

        /// Objects follow their parent's references in the order they're
        /// resolved, each with its own (negative) reference number.
        final class FasWriter {
            var out: [UInt8] = Array("FasdUAS 1.101.10".utf8)
            var next = -1

            func header(_ index: Int, _ ref: Int, _ size: Int) {
                out += [UInt8(index)] + be16(ref & 0xFFFF) + be16(size & 0xFFFF)
            }

            func newRef() -> Int {
                defer { next -= 1 }
                return next
            }

            func emit(_ v: Fas, ref: Int) {
                switch v {
                case .null: header(1, ref, 0)
                case .int(let n): header(3, ref, n)
                case .long(let n):
                    header(7, ref, 4)
                    out += be32(UInt32(bitPattern: n))
                case .real(let d):
                    header(8, ref, 8)
                    let bits = d.bitPattern
                    out += be32(UInt32(bits >> 32)) + be32(UInt32(bits & 0xFFFF_FFFF))
                case .text(let s):
                    let b = Array(s.utf8)
                    header(12, ref, 0)
                    out += be16(b.count) + b + be16(0)
                case .ident(let s):
                    let b = Array(s.utf8)
                    header(11, ref, 0)
                    out += [48] + be16(b.count) + Array(s.lowercased().utf8) + be16(b.count) + b
                case .const4(let s):
                    header(10, ref, 4)
                    out += [10] + be32(fourCC(s))
                case .const8(let t, let v):
                    header(10, ref, 8)
                    out += [11] + be32(fourCC(t)) + be32(v)
                case .event(let c, let i):
                    header(10, ref, 24)
                    out += [46] + be32(fourCC(c)) + be32(fourCC(i)) + be32(0) + be32(0) + be32(0) + be32(0)
                case .code(let b):
                    header(17, ref, b.count)
                    out += b
                case .block(let t, let items):
                    header(14, ref, items.count)
                    out += [UInt8(t)]
                    children(items)
                case .pointers(let items):
                    header(16, ref, items.count)
                    children(items)
                case .list(let items):
                    if items.isEmpty {
                        header(2, ref, 0)
                        return
                    }
                    var r = ref
                    for item in items {
                        header(2, r, 2)
                        let a = newRef(), b = newRef()
                        out += be16(a & 0xFFFF) + be16(b & 0xFFFF)
                        emit(item, ref: a)
                        r = b
                    }
                    header(2, r, 0)
                case .record(let pairs):
                    if pairs.isEmpty {
                        header(6, ref, 1)
                        return
                    }
                    var r = ref
                    for (k, v) in pairs {
                        header(6, r, 3)
                        let a = newRef(), b = newRef(), c = newRef()
                        out += be16(a & 0xFFFF) + be16(b & 0xFFFF) + be16(c & 0xFFFF)
                        emit(k, ref: a)
                        emit(v, ref: b)
                        r = c
                    }
                    header(6, r, 1)
                case .app(let name, let sig):
                    // AppleScript's header, then an alias record naming the
                    // application's file: its name and its creator.
                    var alias = [UInt8](repeating: 0, count: 150)
                    alias[5] = 150
                    alias[7] = 2
                    alias[50] = UInt8(name.utf8.count)
                    alias.replaceSubrange(51..<(51 + name.utf8.count), with: Array(name.utf8))
                    alias.replaceSubrange(122..<126, with: Array("APPL".utf8))
                    alias.replaceSubrange(126..<130, with: Array(sig.utf8))
                    let body = [UInt8](repeating: 0, count: 90) + Array("alis".utf8) + alias
                    header(15, ref, body.count)
                    out += [8] + body
                }
            }

            func children(_ items: [Fas]) {
                let refs = items.map { _ in newRef() }
                for r in refs { out += be16(r & 0xFFFF) }
                for (item, r) in zip(items, refs) { emit(item, ref: r) }
            }
        }

        /// A handler: its name, parameters, local variables, literals and code.
        static func handler(_ name: Fas, params: [String]? = nil, locals: [String], literals: [Fas],
                            code: [UInt8]) -> Fas {
            let args: Fas = params.map { .block(4, [.int($0.count), .block(0, $0.map { .ident($0) })]) } ?? .null
            return .block(16, [name, .null, args, .list([]), .pointers(locals.map { .ident($0) }),
                               .pointers(literals), .code(code)])
        }

        /// A script: its properties and handlers (names and values).
        static func script(_ props: [(Fas, Fas)]) -> [UInt8] {
            let w = FasWriter()
            let root: Fas = .block(15, [.null, .null, .block(15, []),
                                        .pointers([.null, .pointers(props.map { $0.0 })] + props.map { $0.1 })])
            w.emit(root, ref: 0)
            return w.out
        }

        static let run: Fas = .event("aevt", "oapp")

        /// Bytecode with labels: jumps are relative to their operand.
        final class Asm {
            var code: [UInt8] = []
            var labels: [String: Int] = [:]
            var fixups: [(at: Int, label: String)] = []
            func op(_ b: UInt8...) { code += b }
            func word(_ w: Int) { code += be16(w & 0xFFFF) }
            func jump(_ b: UInt8, _ label: String) {
                code.append(b)
                fixups.append((code.count, label))
                code += [0, 0]
            }
            func label(_ l: String) { labels[l] = code.count }
            func done() -> [UInt8] {
                for f in fixups {
                    let off = labels[f.label]! - f.at
                    code[f.at] = UInt8((off >> 8) & 0xFF)
                    code[f.at + 1] = UInt8(off & 0xFF)
                }
                return code
            }
        }

        // Opcodes, as the corpus's scripts use them.
        static let pushLit: UInt8 = 0xE0, pushVar: UInt8 = 0xA0, popVar: UInt8 = 0xB0, pushGlobal: UInt8 = 0xC0
        static let getData: UInt8 = 0x45, storeResult: UInt8 = 0x4F, ret: UInt8 = 0x0F, concat: UInt8 = 0x25
        static let push0: UInt8 = 0x6A, push1: UInt8 = 0x6B, push2: UInt8 = 0x6C, push3: UInt8 = 0x6D
        static let pushUndefined: UInt8 = 0x68, dup: UInt8 = 0x5B, makeVector: UInt8 = 0x76, coerce: UInt8 = 0x26
        static let messageSend: UInt8 = 0x0C, pushIt: UInt8 = 0x2A, pushMe: UInt8 = 0x29, tell: UInt8 = 0x12

        /// Load and run a script: its result as AppleScript shows it, or the error.
        func runScript(_ data: [UInt8]) -> (text: String, error: Int32) {
            var id: UInt32 = 0
            let err = data.withUnsafeBufferPointer { as_load_script($0.baseAddress, $0.count, &id) }
            if err != 0 { return (String(cString: as_error_message()), err) }
            var result: UInt32 = 0
            let e = as_run_script(id, &result)
            defer { as_dispose(id); as_dispose(result) }
            if e != 0 { return (String(cString: as_error_message()), e) }
            var buf = [CChar](repeating: 0, count: 1024)
            _ = as_display(result, &buf, buf.count)
            return (String(cString: buf), 0)
        }

        func prepare() {
            launchTestApp()
            let q = QTTune()
            q.install()
            machine_set_script_dialog_handler(nil, nil)
        }

        /// A script's properties load, and its run handler computes with
        /// them: a loop, arithmetic, text, a list and count.
        @Test func loadsAndRunsLoopsListsAndText() {
            prepare()
            let a = Asm()
            // set x to start (a property)
            a.op(0x62); a.word(1); a.word(0)            // PushParentVariable 1, 0
            a.op(Self.getData, Self.popVar | 0, Self.storeResult)
            // repeat with i from 1 to 5: set x to x + i
            a.jump(0x17, "exit")                           // LinkRepeat
            a.op(Self.push1, Self.pushLit | 0, Self.push1, Self.pushUndefined)
            a.label("top")
            a.op(0x1C); a.word(1)                          // RepeatInRange, i is local 1
            a.op(Self.pushVar | 0, Self.pushVar | 1, 0x1E, Self.getData, Self.popVar | 0, Self.dup, Self.storeResult)
            a.jump(0x59, "top")                            // Jump
            a.label("exit")
            a.op(Self.storeResult)
            // set l to {1, 2, "three"} & x
            a.op(Self.push1, Self.push2, Self.pushLit | 1, Self.push3, Self.makeVector, Self.pushVar | 0, Self.concat)
            a.op(Self.getData, Self.popVar | 2, Self.storeResult)
            // return (x as text) & ":" & item 4 of l & ":" & (count l) & ":" & (characters 2 thru 3 of "abcd" as text)
            a.op(Self.pushVar | 0, Self.pushLit | 2, Self.coerce, Self.pushLit | 3, Self.concat)
            a.op(Self.pushVar | 2, Self.pushLit | 4, Self.pushLit | 5, 0x2F, Self.concat)   // item 4 of l
            a.op(Self.pushLit | 3, Self.concat)
            a.op(Self.pushVar | 2, Self.push0, Self.messageSend); a.word(6)                 // count l
            a.op(Self.concat, Self.pushLit | 3, Self.concat)
            a.op(Self.pushLit | 7, Self.pushLit | 8, Self.push2, Self.pushLit | 9, 0x32)     // characters 2 thru 9 of
            a.op(Self.pushLit | 2, Self.coerce, Self.concat, Self.ret)
            let lits: [Fas] = [.int(5), .text("three"), .const4("TEXT"), .text(":"), .const4("cobj"), .int(4),
                               .event("core", "cnte"), .text("abcd"), .const4("cha "), .int(3)]
            let data = Self.script([(.ident("start"), .int(100)),
                                    (Self.run, Self.handler(Self.run, locals: ["x", "i", "l"], literals: lits,
                                                            code: a.done()))])
            let r = runScript(data)
            #expect(r.error == 0)
            #expect(r.text == "\"115:115:4:bc\"")
        }

        /// Handlers with parameters, if, try with on error's variables and
        /// an error of the script's own.
        @Test func handlersConditionsAndErrors() {
            prepare()
            // on twice(n): return n * 2
            let h = Asm()
            h.op(Self.pushVar | 0, Self.push2, 0x20, Self.ret)
            let twice = Self.handler(.ident("twice"), params: ["n"], locals: ["n"], literals: [], code: h.done())
            let a = Asm()
            // if twice(21) = 42 then set r to "yes" else set r to "no"
            a.op(Self.pushIt, Self.pushLit | 0, Self.push1, 0x2B); a.word(1)   // PositionalMessageSend twice
            a.op(Self.pushLit | 2, 0x00)                                         // = 42
            a.jump(0x1D, "else")
            a.op(Self.pushLit | 3, Self.getData, Self.popVar | 2)
            a.jump(0x59, "endif")
            a.label("else")
            a.op(Self.pushLit | 4, Self.getData, Self.popVar | 2)
            a.label("endif")
            a.op(Self.storeResult)
            // try: error "boom" number 7; on error m number n: return r & m & n
            a.jump(0x14, "handler")                                              // ErrorHandler
            a.op(Self.pushMe, Self.pushLit | 5, Self.pushLit | 6, Self.push2, Self.pushLit | 7, 0x15)
            a.jump(0x57, "after")                                                // EndErrorHandler
            a.label("handler")
            a.op(0x58); a.word(8); a.word(9)                                     // HandleError m, {errn: n}
            a.op(Self.pushVar | 2, Self.pushVar | 0, Self.concat, Self.pushVar | 1, Self.concat, Self.ret)
            a.label("after")
            a.op(Self.pushLit | 4, Self.ret)
            let lits: [Fas] = [.int(21), .ident("twice"), .int(42), .text("yes"), .text("no"), .const4("errn"),
                               .int(7), .text("boom"), .ident("m"), .record([(.const4("errn"), .ident("n"))])]
            let data = Self.script([(.ident("twice"), twice),
                                    (Self.run, Self.handler(Self.run, locals: ["m", "n", "r"], literals: lits,
                                                            code: a.done()))])
            let r = runScript(data)
            #expect(r.error == 0)
            #expect(r.text == "\"yesboom7\"")

            // An error nothing catches ends the script with it.
            let b = Asm()
            b.op(Self.pushMe, Self.pushLit | 0, Self.pushLit | 1, Self.push2, Self.pushLit | 2, 0x15)
            let failing = Self.script([(Self.run, Self.handler(Self.run, locals: [],
                                                               literals: [.const4("errn"), .int(-128), .text("stop")],
                                                               code: b.done()))])
            let f = runScript(failing)
            #expect(f.error == -128)
            #expect(f.text == "stop")
        }

        /// display dialog "Name?" default answer "Ann" buttons {"No", "Yes"}
        /// default button 2, then text returned & "/" & button returned.
        func dialogScript() -> [UInt8] {
            let a = Asm()
            a.op(Self.pushLit | 0, Self.pushLit | 1, Self.pushLit | 2, Self.pushLit | 3, Self.push2, Self.makeVector)
            a.op(Self.pushLit | 4, Self.push2, Self.pushLit | 5, Self.pushLit | 6, Self.pushLit | 7, Self.messageSend)
            a.word(8)
            a.op(Self.storeResult)
            a.op(Self.pushGlobal | 9, Self.pushLit | 10, 0x2C, Self.pushLit | 11, Self.concat)
            a.op(Self.pushGlobal | 9, Self.pushLit | 12, 0x2C, Self.concat, Self.ret)
            let lits: [Fas] = [.text("Name?"), .const4("btns"), .text("No"), .text("Yes"), .const4("dflt"),
                               .const4("dtxt"), .text("Ann"), .int(6), .event("syso", "dlog"), .const4("rslt"),
                               .const4("ttxt"), .text("/"), .const4("bhit")]
            return Self.script([(Self.run, Self.handler(Self.run, locals: [], literals: lits, code: a.done()))])
        }

        /// Headless, a dialog answers itself with its default button and
        /// default answer; with a host, the host's answer comes back.
        @Test func displayDialogHeadlessAndHost() {
            prepare()
            let headless = runScript(dialogScript())
            #expect(headless.error == 0)
            #expect(headless.text == "\"Ann/Yes\"")

            machine_set_script_dialog_handler({ d, _ in
                guard let d else { return 0 }
                // The host saw the message, the buttons and the default answer.
                let ok = String(cString: withUnsafeBytes(of: d.pointee.message) { Array($0.bindMemory(to: CChar.self)) }) == "Name?"
                    && d.pointee.nbuttons == 2 && d.pointee.default_button == 2 && d.pointee.has_answer
                let typed = Array((ok ? "Bob" : "bad").utf8) + [0]
                withUnsafeMutableBytes(of: &d.pointee.answer) { raw in
                    for (i, b) in typed.enumerated() { raw[i] = b }
                }
                return 1                                                   // "No"
            }, nil)
            let host = runScript(dialogScript())
            machine_set_script_dialog_handler(nil, nil)
            #expect(host.error == 0)
            #expect(host.text == "\"Bob/No\"")
        }

        /// tell application "SimpleText" to get name: there is no such
        /// Blue Box run, and nothing goes to the host: Application isn't
        /// running.  do shell script is refused outright.
        @Test func tellToOtherProgramsIsRefused() {
            prepare()
            let a = Asm()
            a.op(Self.pushLit | 0, Self.tell); a.word(0)
            a.op(Self.pushIt, Self.pushLit | 1, 0x2C, Self.getData, 0x55, Self.ret)   // name of it; EndTell
            let tell = Self.script([(Self.run, Self.handler(Self.run, locals: [],
                                                            literals: [.app(name: "SimpleText", signature: "ttxt"),
                                                                       .const4("pnam")],
                                                            code: a.done()))])
            let r = runScript(tell)
            #expect(r.error == -600)
            #expect(r.text.contains("SimpleText"))

            let b = Asm()
            b.op(Self.pushLit | 0, Self.push0, Self.messageSend); b.word(1)
            b.op(Self.ret)
            let shell = Self.script([(Self.run, Self.handler(Self.run, locals: [],
                                                             literals: [.text("ls"), .event("syso", "exec")],
                                                             code: b.done()))])
            let s = runScript(shell)
            #expect(s.error == -1708)
        }

        /// Damaged data is an error, never a crash.
        @Test func damagedScriptsAreRefused() {
            prepare()
            var id: UInt32 = 0
            let good = dialogScript()
            for n in [0, 8, 20, good.count / 2, good.count - 3] {
                let part = Array(good.prefix(n))
                let err = part.withUnsafeBufferPointer { as_load_script($0.baseAddress, $0.count, &id) }
                #expect(err == -1702)
            }
            // Bytecode that ends in the middle of an instruction stops cleanly.
            let bad = Self.script([(Self.run, Self.handler(Self.run, locals: [], literals: [], code: [0x0C, 0x00]))])
            _ = runScript(bad)
            #expect(!g_cpu!.pointee.halted)
        }

        /// The AppleScript component, as a 68k program uses it: OSALoad,
        /// OSAExecute, OSADisplay through _ComponentDispatch.
        @Test func osaComponentLoadsExecutesAndDisplays() {
            prepare()
            let q = QTTune()
            let ci = q.call([0x7021, 0xA82A], [fourCC("osa "), fourCC("ascr")])   // OpenDefaultComponent
            #expect(ci != 0)
            let a = Asm()
            a.op(Self.pushLit | 0, Self.pushLit | 1, Self.concat, Self.ret)
            let data = Self.script([(Self.run, Self.handler(Self.run, locals: [], literals: [.text("Blue "), .text("Box")],
                                                            code: a.done()))])
            let desc = tb_NewPtr(8, false, true), idp = tb_NewPtr(4, false, true), text = tb_NewPtr(8, false, true)
            let buf = q.guestBytes(data)
            #expect(tb_AECreateDesc(Int32(bitPattern: fourCC("scpt")), buf, Int32(data.count), desc) == 0)
            #expect(q.component(ci, 1, [desc, 0, idp]) == 0)                          // OSALoad
            let script = rd32(idp)
            #expect(q.component(ci, 3, [script, 0, 0, idp]) == 0)                    // OSAExecute
            #expect(q.component(ci, 4, [rd32(idp), fourCC("TEXT"), 0, text]) == 0)   // OSADisplay
            let h = rd32(text + 4)
            let n = Int(tb_GetHandleSize(h))
            let shown = (0..<n).map { rd8(rd32(h) + UInt32($0)) }
            #expect(String(decoding: shown, as: UTF8.self) == "\"Blue Box\"")
            // Compiling is not there yet: a script error saying so.
            #expect(q.component(ci, 0x0203, [desc, 0, idp]) == -1753)
            #expect(q.component(ci, 5, [fourCC("errn"), fourCC("long"), text]) == 0)  // OSAScriptError
            #expect(Int32(bitPattern: rd32(rd32(rd32(text + 4)))) == -2741)
        }
    }
}
