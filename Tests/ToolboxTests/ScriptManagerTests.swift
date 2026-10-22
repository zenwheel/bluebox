import CCore
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Scripts other than Roman: installed only when the System file has
    /// their bundle, and then 2-byte text is told apart.
    @Suite struct ScriptManagerScripts {
        static let smEnabled: Int16 = 4, smForced: Int16 = 12, smDefault: Int16 = 14, smIntlForce: Int16 = 10
        static let smLastScript: Int16 = 20, smKeyScript: Int16 = 22, smDoubleByte: Int16 = 36
        static let smKeyDisableState: Int16 = 42
        static let smJapanese: Int16 = 1, smTradChinese: Int16 = 2

        /// Text in guest memory.
        func text(_ bytes: [UInt8]) -> UInt32 {
            let p = tb_NewPtr(Int32(bytes.count + 1), false, true)
            for (i, b) in bytes.enumerated() { wr8(p + UInt32(i), b) }
            return p
        }

        /// A port of our own to draw with, in a font.
        func port(font: Int16) {
            let port = tb_NewPtr(Int32(GrafPort_SIZEOF), false, true)
            tb_OpenPort(port)
            tb_TextFont(font)
        }

        /// The Japanese script's bundle, put in the System file.
        func installJapanese() {
            let saved = tb_CurResFile()
            tb_UseResFile(Int16(bitPattern: rd16(UInt32(LM_SysMap))))
            let h = tb_NewHandle(50, false, true)
            let name = tb_NewPtr(2, false, true)
            tb_AddResource(h, Int32(bitPattern: fourCC("itlb")), 1, name)
            tb_UseResFile(saved)
        }

        // あ (82 A0), 亜 (88 9F, level 1), 弌 (98 9F, level 2), Ａ (82 60)
        let japanese: [UInt8] = [0x41, 0x82, 0xA0, 0x42, 0x88, 0x9F, 0x98, 0x9F, 0x82, 0x60]

        /// Blue Box's System is U.S.: one script, and 2-byte text is just
        /// bytes, as Inside Macintosh says of a system with Roman alone.
        @Test func romanAlone() {
            launchTestApp()
            #expect(tb_GetScriptManagerVariable(Self.smEnabled) == 1)
            #expect(tb_GetScriptManagerVariable(Self.smDoubleByte) == 0)
            #expect(tb_GetScriptManagerVariable(Self.smIntlForce) == 0xFF) // from 'itlc'
            #expect(tb_FontToScript(3) == 0 && tb_GetScriptManagerVariable(Self.smDefault) == 0)
            // A Japanese font: the script isn't there, so the system script.
            #expect(tb_FontToScript(16384) == 0 && tb_GetScriptManagerVariable(Self.smDefault) == 0xFF)
            let t = text(japanese)
            #expect(tb_CharacterByteType(t, 1, Self.smJapanese) == 0)
            #expect(tb_CharacterByteType(t, 2, Self.smJapanese) == 0)
            let table = tb_NewPtr(256, false, true)
            #expect(tb_FillParseTable(table, Self.smJapanese) == 1)
            #expect((0..<256).allSatisfy { rd8(table + UInt32($0)) == 0 })
            tb_KeyScript(Self.smJapanese)
            #expect(tb_GetScriptManagerVariable(Self.smKeyScript) == 0)
            tb_KeyScript(-1) // smKeyNextScript: there is no other
            #expect(tb_GetScriptManagerVariable(Self.smKeyScript) == 0)
        }

        /// With the Japanese bundle installed: its fonts, its first bytes,
        /// its character types and the keyboard script.
        @Test func japaneseInstalled() {
            launchTestApp()
            _ = initToolbox()
            installJapanese()
            #expect(tb_GetScriptManagerVariable(Self.smEnabled) == 2)
            #expect(tb_GetScriptManagerVariable(Self.smDoubleByte) == 0xFF)
            #expect(tb_FontToScript(16384) == 1 && tb_FontToScript(16895) == 1)
            #expect(tb_GetScriptManagerVariable(Self.smDefault) == 0)
            #expect(tb_FontToScript(16896) == 0 && tb_GetScriptManagerVariable(Self.smDefault) == 0xFF)
            #expect(tb_FontToScript(16383) == 0)

            let t = text(japanese)
            let types = (0..<Int16(japanese.count)).map { tb_CharacterByteType(t, $0, Self.smJapanese) }
            #expect(types == [0, -1, 1, 0, -1, 1, -1, 1, -1, 1])
            // Big5 has no first byte at $82: Traditional Chinese isn't installed anyway.
            #expect(tb_CharacterByteType(t, 1, Self.smTradChinese) == 0)
            #expect(UInt16(bitPattern: tb_CharacterType(t, 1, Self.smJapanese)) == 0x8003) // hiragana
            #expect(UInt16(bitPattern: tb_CharacterType(t, 2, Self.smJapanese)) == 0x8003)
            #expect(UInt16(bitPattern: tb_CharacterType(t, 4, Self.smJapanese)) == 0x8004) // ideograph, level 1
            #expect(UInt16(bitPattern: tb_CharacterType(t, 7, Self.smJapanese)) == 0x8104) // level 2
            #expect(UInt16(bitPattern: tb_CharacterType(t, 8, Self.smJapanese)) == 0xC001) // full-width A
            #expect(tb_CharacterType(t, 0, Self.smJapanese) == 0x4001) // A, by the 'itl2'

            let table = tb_NewPtr(256, false, true)
            #expect(tb_FillParseTable(table, Self.smJapanese) == 1)
            #expect(rd8(table + 0x81) == 1 && rd8(table + 0x9F) == 1 && rd8(table + 0xE0) == 1 && rd8(table + 0xFC) == 1)
            #expect(rd8(table + 0x41) == 0 && rd8(table + 0xA0) == 0 && rd8(table + 0xDF) == 0 && rd8(table + 0xFD) == 0)

            // The current script is the port's font's.
            port(font: 16384)
            #expect(tb_FontScript() == 1)
            #expect(tb_IntlScript() == 0 && tb_GetScriptManagerVariable(Self.smForced) == 0xFF)
            #expect(tb_CharByte(t, 2) == 1)
            #expect(tb_ParseTable(table) == 1 && rd8(table + 0x82) == 1)

            // Keyboard scripts: to Japanese and back, swapped, the next.
            tb_KeyScript(Self.smJapanese)
            #expect(tb_GetScriptManagerVariable(Self.smKeyScript) == 1)
            #expect(tb_GetScriptManagerVariable(Self.smLastScript) == 0)
            tb_KeyScript(-3) // smKeySwapScript
            #expect(tb_GetScriptManagerVariable(Self.smKeyScript) == 0)
            #expect(tb_GetScriptManagerVariable(Self.smLastScript) == 1)
            tb_KeyScript(-1) // smKeyNextScript
            #expect(tb_GetScriptManagerVariable(Self.smKeyScript) == 1)
            tb_KeyScript(-17) // smKeyRoman
            #expect(tb_GetScriptManagerVariable(Self.smKeyScript) == 0)
            tb_KeyScript(Self.smTradChinese) // not installed: nothing
            #expect(tb_GetScriptManagerVariable(Self.smKeyScript) == 0)
            tb_KeyScript(-6) // smKeyDisableKybds: Roman and the system script only
            #expect(tb_GetScriptManagerVariable(Self.smKeyDisableState) == 0xFF)
            tb_KeyScript(Self.smJapanese)
            #expect(tb_GetScriptManagerVariable(Self.smKeyScript) == 0)
            tb_KeyScript(-7) // smKeyEnableKybds
            tb_KeyScript(-12) // smKeyDisableKybdSwitch
            tb_KeyScript(Self.smJapanese)
            #expect(tb_GetScriptManagerVariable(Self.smKeyScript) == 0)
            tb_KeyScript(-7)
            tb_KeyScript(Self.smJapanese)
            #expect(tb_GetScriptManagerVariable(Self.smKeyScript) == 1)
        }

        /// Truncating and breaking lines keep 2-byte characters whole.
        @Test func twoByteTextStaysWhole() {
            launchTestApp()
            _ = initToolbox()
            installJapanese()
            port(font: 16384)
            // あいう: room for one and a half characters and the ellipsis.
            let s = tb_NewPtr(256, false, true)
            let kana: [UInt8] = [0x82, 0xA0, 0x82, 0xA2, 0x82, 0xA4]
            wr8(s, UInt8(kana.count))
            for (i, b) in kana.enumerated() { wr8(s + 1 + UInt32(i), b) }
            let three = tb_TextWidth(s + 1, 0, 3), ell = tb_CharWidth(0xC9)
            #expect(tb_TruncString(three + ell, s, 0) == 1) // truncEnd
            #expect(rd8(s) == 3 && rd8(s + 1) == 0x82 && rd8(s + 2) == 0xA0 && rd8(s + 3) == 0xC9)

            // A line one and a half characters wide breaks between characters.
            let t = text(kana)
            let widthp = tb_NewPtr(4, false, true), offset = tb_NewPtr(4, false, true)
            wr32(widthp, UInt32(tb_TextWidth(t, 0, 3)) << 16)
            wr32(offset, 1) // the first run on the line
            #expect(tb_StyledLineBreak(t, 6, 0, 6, 0, widthp, offset) == 1) // smBreakChar
            #expect(rd32(offset) == 2)
        }
    }
}
