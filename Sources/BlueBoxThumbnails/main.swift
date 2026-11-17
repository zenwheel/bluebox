// Blue Box Thumbnails.appex: classic files' real icons as their Quick
// Look thumbnails in the macOS Finder (ThumbnailProvider).  An app
// extension's program starts in NSExtensionMain, which runs the provider
// its Info.plist names (scripts/make-app.sh writes it).

#if canImport(QuickLookThumbnailing)
import Foundation

@_silgen_name("NSExtensionMain")
func NSExtensionMain(_ argc: Int32, _ argv: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>) -> Int32

exit(NSExtensionMain(CommandLine.argc, CommandLine.unsafeArgv))
#endif
