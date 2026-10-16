// Keyboard input through the host's keyboard layout.

#if canImport(AppKit)
import AppKit
import Carbon

/// Key events to text with the current keyboard layout, keeping dead-key
/// state between keystrokes as the Mac's keyboard layouts did.
public final class KeyTranslator {
    public init() {}

    private var deadKeyState: UInt32 = 0

    /// The text for a key press; nil while a dead key waits for the next key.
    public func translate(_ event: NSEvent) -> String? {
        guard let source = TISCopyCurrentKeyboardLayoutInputSource()?.takeRetainedValue(),
              let ptr = TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData) else {
            return event.characters
        }
        let data = Unmanaged<CFData>.fromOpaque(ptr).takeUnretainedValue()
        guard let bytes = CFDataGetBytePtr(data) else { return event.characters }
        let layout = UnsafeRawPointer(bytes).assumingMemoryBound(to: UCKeyboardLayout.self)
        var flags: UInt32 = 0
        if event.modifierFlags.contains(.shift) { flags |= UInt32(shiftKey) }
        if event.modifierFlags.contains(.option) { flags |= UInt32(optionKey) }
        if event.modifierFlags.contains(.capsLock) { flags |= UInt32(alphaLock) }
        if event.modifierFlags.contains(.control) { flags |= UInt32(controlKey) }
        var chars = [UniChar](repeating: 0, count: 8)
        var length = 0
        let status = UCKeyTranslate(layout, event.keyCode, UInt16(kUCKeyActionDown), (flags >> 8) & 0xFF,
                                    UInt32(LMGetKbdType()), 0, &deadKeyState, chars.count, &length, &chars)
        guard status == noErr else { return event.characters }
        if length == 0 && deadKeyState != 0 { return nil }
        return String(utf16CodeUnits: chars, count: length)
    }
}

#endif
