#!/bin/sh
# Build Blue Box.app: the bluebox program, opened as an application, shows
# the shell.  The bundle carries everything it needs: the program (whose
# fonts are built in), The Unarchiver's unar for archives when it is
# installed here (Homebrew), the icon and the licences.  QuickTime movies
# still use VLC.app when it is installed.
#
# usage: sh scripts/make-app.sh [destination folder]   (default: build)
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
dest=${1:-"$root/build"}
app="$dest/Blue Box.app"
version=$(git -C "$root" describe --always --dirty 2>/dev/null || echo dev)

echo "Building bluebox..."
(cd "$root" && swift build -c release)
bin=$(cd "$root" && swift build -c release --show-bin-path)/bluebox

echo "Making $app..."
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$bin" "$app/Contents/MacOS/bluebox"

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
swift "$root/scripts/make-icon.swift" "$iconset"
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
	<string>$version</string>
	<key>LSMinimumSystemVersion</key>
	<string>13.0</string>
	<key>LSApplicationCategoryType</key>
	<string>public.app-category.utilities</string>
	<key>NSHighResolutionCapable</key>
	<true/>
	<key>NSPrincipalClass</key>
	<string>NSApplication</string>
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

# An ad hoc signature over the whole bundle, so it runs as one application.
codesign --force --sign - "$app/Contents/MacOS/unar" 2>/dev/null || true
codesign --force --sign - "$app"

echo "Done: $app"
