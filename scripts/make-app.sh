#!/bin/sh
# Build Blue Box.app: the bluebox program, opened as an application, shows
# the shell.  The bundle carries everything it needs: the program (whose
# fonts are built in), The Unarchiver's unar for archives when it is
# installed here (Homebrew), Sparkle for software updates, the icon and
# the licences.  QuickTime movies still use VLC.app when it is installed.
#
# usage: sh scripts/make-app.sh [destination folder]   (default: build)
#
# Environment (scripts/release.sh sets all three):
#   BLUEBOX_VERSION  CFBundleShortVersionString   (default: git describe)
#   BLUEBOX_BUILD    CFBundleVersion, which Sparkle compares  (default: 0)
#   SIGN_IDENTITY    codesign identity           (default: -, ad hoc)
# A build without BLUEBOX_BUILD is a development build: numbered 0, so any
# release is newer, and it checks for updates only when asked to.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
dest=${1:-"$root/build"}
app="$dest/Blue Box.app"
version=${BLUEBOX_VERSION:-$(git -C "$root" describe --always --dirty 2>/dev/null || echo dev)}
build=${BLUEBOX_BUILD:-0}
identity=${SIGN_IDENTITY:--}
if [ -n "${BLUEBOX_BUILD:-}" ]; then autocheck=true; else autocheck=false; fi

echo "Building bluebox..."
(cd "$root" && swift build -c release)
bin=$(cd "$root" && swift build -c release --show-bin-path)/bluebox

echo "Making $app..."
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources" "$app/Contents/Frameworks"
cp "$bin" "$app/Contents/MacOS/bluebox"

# Sparkle (MIT), which SwiftPM copies beside the program from its
# xcframework; the program finds it through @executable_path/../Frameworks.
ditto "$(dirname "$bin")/Sparkle.framework" "$app/Contents/Frameworks/Sparkle.framework"

# unar (LGPL 2.1), beside the program, where bluebox looks first.
unar=$(command -v unar || true)
if [ -n "$unar" ]; then
	unar=$(cd "$(dirname "$unar")" && pwd -P)/$(basename "$unar")
	real=$(readlink "$unar" || true)
	if [ -n "$real" ]; then
		unar=$(cd "$(dirname "$unar")" && cd "$(dirname "$real")" && pwd -P)/$(basename "$real")
	fi
	cp "$unar" "$app/Contents/MacOS/unar"
	cellar=$(dirname "$(dirname "$unar")")
	if [ -f "$cellar/LICENSE" ]; then
		cp "$cellar/LICENSE" "$app/Contents/Resources/unar LICENSE.txt"
	fi
else
	echo "note: unar not found; the app will need it installed to import StuffIt archives" >&2
fi

cp "$root/LICENSE" "$app/Contents/Resources/LICENSE.txt"
cp "$root/Sources/MACE/COPYING.LGPLv2.1" "$app/Contents/Resources/MACE COPYING.LGPLv2.1.txt"

# The icon.
iconset=$(mktemp -d)/BlueBox.iconset
"$bin" make-iconset "$iconset"
iconutil -c icns -o "$app/Contents/Resources/BlueBox.icns" "$iconset"
rm -rf "$(dirname "$iconset")"

# Info.plist: the shell, and the files it imports when they are dropped on it.
cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>en</string>
	<key>CFBundleExecutable</key>
	<string>bluebox</string>
	<key>CFBundleIconFile</key>
	<string>BlueBox</string>
	<key>CFBundleIdentifier</key>
	<string>local.bluebox-shell</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>Blue Box</string>
	<key>CFBundleDisplayName</key>
	<string>Blue Box</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleShortVersionString</key>
	<string>$version</string>
	<key>CFBundleVersion</key>
	<string>$build</string>
	<key>LSMinimumSystemVersion</key>
	<string>13.0</string>
	<key>LSApplicationCategoryType</key>
	<string>public.app-category.utilities</string>
	<key>NSMicrophoneUsageDescription</key>
	<string>Classic programs record sound through the microphone.</string>
	<key>NSHighResolutionCapable</key>
	<true/>
	<key>NSPrincipalClass</key>
	<string>NSApplication</string>
	<key>SUFeedURL</key>
	<string>https://www.zenwheel.com/software/bluebox/appcast.xml</string>
	<key>SUPublicEDKey</key>
	<string>Tmp/L8rvhxtHGolvbOSmCRjy16GP5kawG+mA3F+9dcg=</string>
	<key>SUEnableAutomaticChecks</key>
	<$autocheck/>
	<key>CFBundleDocumentTypes</key>
	<array>
		<dict>
			<key>CFBundleTypeName</key>
			<string>Classic Mac archive or disk image</string>
			<key>CFBundleTypeRole</key>
			<string>Viewer</string>
			<key>LSHandlerRank</key>
			<string>Alternate</string>
			<key>CFBundleTypeExtensions</key>
			<array>
				<string>sit</string>
				<string>sea</string>
				<string>cpt</string>
				<string>hqx</string>
				<string>bin</string>
				<string>macbin</string>
				<string>img</string>
				<string>image</string>
				<string>dsk</string>
				<string>dc42</string>
				<string>toast</string>
				<string>iso</string>
				<string>hfs</string>
				<string>cdr</string>
			</array>
		</dict>
	</array>
</dict>
</plist>
EOF

# Sign from the inside out (codesign --deep would sign Sparkle's helpers
# without their own options), each with the hardened runtime: Sparkle's
# installer, downloader (keeping its entitlements), Autoupdate and
# Updater.app, then the framework, unar, and the application with
# BlueBox.entitlements.  Ad hoc unless SIGN_IDENTITY says otherwise.
echo "Signing with ${identity}..."
sign() {
	if [ "$identity" = - ]; then
		codesign --force --sign - --options runtime "$@"
	else
		codesign --force --sign "$identity" --options runtime --timestamp "$@"
	fi
}
fw="$app/Contents/Frameworks/Sparkle.framework"
sign "$fw/Versions/B/XPCServices/Installer.xpc"
sign --preserve-metadata=entitlements "$fw/Versions/B/XPCServices/Downloader.xpc"
sign "$fw/Versions/B/Autoupdate"
sign "$fw/Versions/B/Updater.app"
sign "$fw"
if [ -f "$app/Contents/MacOS/unar" ]; then
	sign "$app/Contents/MacOS/unar"
fi
sign --entitlements "$root/scripts/BlueBox.entitlements" "$app"

echo "Done: $app"
