// Blue Box Preview.appex: classic files previewed by Quick Look in the
// macOS Finder (PreviewProvider).  An app extension's program starts in
// NSExtensionMain, which runs the provider its Info.plist names
// (scripts/make-app.sh writes it).

#if canImport(QuickLookUI)
import Foundation

@_silgen_name("NSExtensionMain")
func NSExtensionMain(_ argc: Int32, _ argv: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>) -> Int32

exit(NSExtensionMain(CommandLine.argc, CommandLine.unsafeArgv))
#endif
