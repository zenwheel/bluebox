#!/bin/sh
# Build "Blue Box After Dark.saver", the macOS screen saver that runs After
# Dark modules in Blue Box, into a folder (default .build/saver).
#
#   scripts/build-saver.sh [out-folder]
#
# To install it, double-click the .saver (System Settings offers to install
# it for this user) or copy it to ~/Library/Screen Savers; then choose it in
# System Settings > Screen Saver and use Options... to pick a module.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$root/.build/saver}
name="Blue Box After Dark"
version=$(git -C "$root" describe --always --dirty 2>/dev/null || echo dev)

swift build -c release --product BlueBoxSaver --package-path "$root"
bin=$(swift build -c release --show-bin-path --package-path "$root")

saver="$out/$name.saver"
rm -rf "$saver"
mkdir -p "$saver/Contents/MacOS" "$saver/Contents/Resources"
cp "$bin/libBlueBoxSaver.dylib" "$saver/Contents/MacOS/$name"
install_name_tool -id "@loader_path/$name" "$saver/Contents/MacOS/$name"

cat > "$saver/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>en</string>
	<key>CFBundleExecutable</key>
	<string>$name</string>
	<key>CFBundleIdentifier</key>
	<string>org.bluebox.afterdark-saver</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>$name</string>
	<key>CFBundlePackageType</key>
	<string>BNDL</string>
	<key>CFBundleShortVersionString</key>
	<string>1.0</string>
	<key>CFBundleVersion</key>
	<string>$version</string>
	<key>LSMinimumSystemVersion</key>
	<string>13.0</string>
	<key>NSPrincipalClass</key>
	<string>BlueBoxAfterDarkView</string>
	<key>NSHumanReadableCopyright</key>
	<string>Blue Box. After Dark modules are their authors'.</string>
</dict>
</plist>
EOF

# Signed for this Mac only (no identity): the screen saver host loads
# bundles that aren't signed by a developer (it disables library validation).
codesign --force --sign - "$saver"
echo "$saver"
