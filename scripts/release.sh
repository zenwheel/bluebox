#!/bin/sh
#
# Build, notarize and package a Blue Box release for Sparkle.
#
#   scripts/release.sh <version>                          build only, unless
#                                                         NOTARY_PROFILE is set
#   scripts/release.sh <version> --notarize <profile>     full release
#   make release VERSION=<version>                        the same, by make
#
# The notarytool keychain profile comes from NOTARY_PROFILE in the
# environment (e.g. NOTARY_PROFILE=zenwheel-notary), or from --notarize,
# which overrides it.
#
# Without a profile, build/Blue Box.app is built and Developer ID signed
# with the hardened runtime, and zipped to build/BlueBox-<version>.zip for
# local testing. Nothing is written to releases/ and the appcast is left alone.
#
# With a profile:
#   1. Builds build/Blue Box.app (scripts/make-app.sh) with <version> as
#      CFBundleShortVersionString and a UTC timestamp build number as
#      CFBundleVersion (what Sparkle compares), signed from the inside out:
#      Sparkle's helpers, Sparkle.framework, unar, then the application with
#      scripts/BlueBox.entitlements.
#   2. Submits the app to Apple's notary service with that notarytool keychain
#      profile, waits, and staples the ticket.
#   3. Zips it to releases/BlueBox-<version>.zip.
#   4. Runs Sparkle's generate_appcast over releases/, which signs the zip with
#      the EdDSA key in the login keychain, creates .delta files from the
#      previous zips still in releases/, and rewrites releases/appcast.xml.
#
# Publish by copying everything in releases/ (zip, deltas, appcast.xml) to
#   zenwheel-site/static/software/bluebox/
# and running that site's deploy.sh. Keep old zips in releases/ so future
# releases can produce deltas from them. releases/ is not in git: back it up.
#
# Environment overrides:
#   NOTARY_PROFILE   notarytool keychain profile       (default: none, build only)
#   TEAM_ID          Apple Developer team              (default: W774XGX423)
#   SIGN_IDENTITY    codesign identity                 (default: that team's
#                    Developer ID Application; - signs ad hoc, build only)
#   RELEASES_DIR     where the artifacts go            (default: releases/)
#   DOWNLOAD_URL     where releases/ gets published    (default: https://www.zenwheel.com/software/bluebox/)

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

APP_NAME="Blue Box"
ZIP_BASENAME="BlueBox"

TEAM_ID=${TEAM_ID:-W774XGX423}
RELEASES_DIR=${RELEASES_DIR:-$ROOT/releases}
DOWNLOAD_URL=${DOWNLOAD_URL:-https://www.zenwheel.com/software/bluebox/}

BUILD_DIR="$ROOT/build"
APP_PATH="$BUILD_DIR/$APP_NAME.app"
# Sparkle's SwiftPM artifact ships the same tools as the framework version.
SPARKLE_BIN="$ROOT/.build/artifacts/sparkle/Sparkle/bin"

# --- Arguments ---------------------------------------------------------------
VERSION=""
NOTARY_PROFILE=${NOTARY_PROFILE:-}
while [ $# -gt 0 ]; do
	case "$1" in
		--notarize)
			case "${2:-}" in
				"" | -*) echo "--notarize needs a notarytool keychain profile name" >&2; exit 2 ;;
			esac
			NOTARY_PROFILE="$2"; shift ;;
		-h | --help) awk 'NR == 1 { next } /^#/ { print; next } { exit }' "$0"; exit 0 ;;
		-*) echo "unknown option: $1" >&2; exit 2 ;;
		*)
			if [ -n "$VERSION" ]; then echo "only one version may be given" >&2; exit 2; fi
			VERSION="$1" ;;
	esac
	shift
done
if [ -z "$VERSION" ]; then
	echo "usage: $0 <version> [--notarize <keychain-profile>]   (or NOTARY_PROFILE=<keychain-profile>)" >&2
	exit 2
fi
NOTARIZE=0
[ -n "$NOTARY_PROFILE" ] && NOTARIZE=1
# 1, 1.2, 1.2.3...: digits separated by single dots.
case "$VERSION" in
	*[!0-9.]* | .* | *. | *..*)
		echo "version must look like 1.2 or 1.2.3, got: $VERSION" >&2
		exit 2 ;;
esac

if [ "$NOTARIZE" = 1 ]; then
	ZIP_PATH="$RELEASES_DIR/$ZIP_BASENAME-$VERSION.zip"
	if [ -e "$ZIP_PATH" ]; then
		echo "$ZIP_PATH already exists; remove it or pick a new version" >&2
		exit 1
	fi
else
	# Unnotarized builds stay out of RELEASES_DIR so generate_appcast never
	# picks one up as a release.
	ZIP_PATH="$BUILD_DIR/$ZIP_BASENAME-$VERSION.zip"
fi

step() { printf '\n==> %s\n' "$*"; }

# --- Signing identity --------------------------------------------------------
# The team's Developer ID Application certificate, by its SHA-1, so another
# team's certificate in the keychain is never picked by name.
if [ -z "${SIGN_IDENTITY:-}" ]; then
	SIGN_IDENTITY=$(security find-identity -v -p codesigning |
		awk -v team="($TEAM_ID)\"" '/"Developer ID Application: / && index($0, team) { print $2; exit }')
	if [ -z "$SIGN_IDENTITY" ]; then
		echo "no Developer ID Application identity for team $TEAM_ID in the keychain" >&2
		echo "(SIGN_IDENTITY=- builds ad hoc, for local testing only)" >&2
		exit 1
	fi
fi
if [ "$SIGN_IDENTITY" = - ] && [ "$NOTARIZE" = 1 ]; then
	echo "an ad hoc signature cannot be notarized; unset SIGN_IDENTITY" >&2
	exit 2
fi

# --- 1. Build and sign -------------------------------------------------------
# Sparkle orders releases by CFBundleVersion, so use a monotonically increasing
# UTC timestamp for it and keep the human-readable version separate.
BUILD_NUMBER=$(date -u +%Y%m%d%H%M)
step "Building $APP_NAME $VERSION (build $BUILD_NUMBER), signed by $SIGN_IDENTITY"
BLUEBOX_VERSION="$VERSION" BLUEBOX_BUILD="$BUILD_NUMBER" SIGN_IDENTITY="$SIGN_IDENTITY" \
	sh "$ROOT/scripts/make-app.sh" "$BUILD_DIR"

if [ ! -x "$SPARKLE_BIN/generate_appcast" ]; then
	echo "Sparkle tools not found at $SPARKLE_BIN (did package resolution run?)" >&2
	exit 1
fi

built_version=$(plutil -extract CFBundleShortVersionString raw "$APP_PATH/Contents/Info.plist")
built_build=$(plutil -extract CFBundleVersion raw "$APP_PATH/Contents/Info.plist")
if [ "$built_version" != "$VERSION" ] || [ "$built_build" != "$BUILD_NUMBER" ]; then
	echo "built app reports $built_version ($built_build), expected $VERSION ($BUILD_NUMBER)" >&2
	exit 1
fi

# --- 2. Notarize + staple ----------------------------------------------------
if [ "$NOTARIZE" = 1 ]; then
	step "Submitting for notarization with profile '$NOTARY_PROFILE' (this takes a few minutes)"
	notarize_zip="$BUILD_DIR/notarize.zip"
	rm -f "$notarize_zip"
	/usr/bin/ditto -c -k --keepParent "$APP_PATH" "$notarize_zip"
	submit_log="$BUILD_DIR/notarize.log"
	submitted=0
	xcrun notarytool submit "$notarize_zip" --keychain-profile "$NOTARY_PROFILE" --wait >"$submit_log" 2>&1 \
		|| submitted=$?
	cat "$submit_log"
	if [ "$submitted" != 0 ] || ! grep -q 'status: Accepted' "$submit_log"; then
		submission_id=$(awk '/^  id: /{print $2; exit}' "$submit_log" || true)
		if [ -n "$submission_id" ]; then
			echo "Notarization failed; log for $submission_id:" >&2
			xcrun notarytool log "$submission_id" --keychain-profile "$NOTARY_PROFILE" >&2 || true
		fi
		exit 1
	fi

	step "Stapling"
	xcrun stapler staple "$APP_PATH"
	spctl --assess --type execute --verbose=2 "$APP_PATH"
fi
codesign --verify --deep --strict "$APP_PATH"

# --- 3. Zip ------------------------------------------------------------------
step "Zipping to $ZIP_PATH"
mkdir -p "$(dirname "$ZIP_PATH")"
rm -f "$ZIP_PATH"
/usr/bin/ditto -c -k --keepParent "$APP_PATH" "$ZIP_PATH"

if [ "$NOTARIZE" = 0 ]; then
	step "Done (build only)"
	cat <<MSG

$APP_NAME $VERSION (build $BUILD_NUMBER) is at $ZIP_PATH.
It is signed but not notarized, and the appcast was not updated. To release:

    NOTARY_PROFILE=zenwheel-notary $0 $VERSION
MSG
	exit 0
fi

# --- 4. Appcast + deltas -----------------------------------------------------
# Signs with the EdDSA private key in the login keychain (the shared zenwheel
# key; import with `generate_keys -f <exported-key-file>` on a new machine).
# Deltas are generated against every older zip present in RELEASES_DIR.
step "Generating appcast and deltas in $RELEASES_DIR"
"$SPARKLE_BIN/generate_appcast" \
	--download-url-prefix "$DOWNLOAD_URL" \
	--link "$DOWNLOAD_URL" \
	-o "$RELEASES_DIR/appcast.xml" \
	"$RELEASES_DIR"

step "Done"
ls -la "$RELEASES_DIR"
cat <<MSG

$APP_NAME $VERSION (build $BUILD_NUMBER) is in $RELEASES_DIR.
Tag the release (git tag v$VERSION), then publish with e.g.:

    rsync -av "$RELEASES_DIR/" ~/Developer/zenwheel-site/static/software/bluebox/
    (cd ~/Developer/zenwheel-site && ./deploy.sh)
MSG
