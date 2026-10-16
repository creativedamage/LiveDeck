#!/bin/bash
# Builds LiveDeck.app (Apple Silicon + Intel), runs the tests, signs it, and writes to ./dist:
#   LiveDeck-<version>.dmg              drag-to-Applications installer
#   LiveDeck-<version>-mac.zip          update package used by the in-app updater
#   LiveDeck-<version>-mac.zip.sha256   its checksum
#
#   ./scripts/build-mac-app.sh                 # build, test, sign, package
#   ./scripts/build-mac-app.sh --install       # ...and copy LiveDeck.app into /Applications
#   ./scripts/build-mac-app.sh --native        # this Mac's architecture only (faster)
#
# Signing for other Macs (also used by the GitHub release workflow):
#   DEVELOPER_ID="Developer ID Application: Your Name (TEAMID)" \
#   NOTARY_PROFILE="livedeck-notary" ./scripts/build-mac-app.sh
# (create the profile once with: xcrun notarytool store-credentials livedeck-notary)
# Without DEVELOPER_ID the app is ad-hoc signed: fine on this Mac and for in-app updates.
set -euo pipefail
cd "$(dirname "$0")/.."

INSTALL=0; ARCHS="arm64;x86_64"
for a in "$@"; do
  case "$a" in
    --install) INSTALL=1 ;;
    --native)  ARCHS="$(uname -m)" ;;
    *) echo "Unknown option $a"; exit 1 ;;
  esac
done

for tool in cmake ninja xcrun; do
  command -v "$tool" >/dev/null || { echo "Missing $tool. Run: brew install cmake ninja  (and xcode-select --install)"; exit 1; }
done

VERSION=$(sed -n 's/^project(LiveDeck VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
BUILD=build-release
echo "==> Configuring LiveDeck $VERSION for $ARCHS"
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$ARCHS" \
      -DLIVEDECK_GITHUB_REPO="${LIVEDECK_GITHUB_REPO:-}"

echo "==> Building"
cmake --build "$BUILD" -j "$(sysctl -n hw.ncpu)"

echo "==> Testing"
"$BUILD/LiveDeckTests_artefacts/Release/LiveDeckTests"

APP="$BUILD/LiveDeck_artefacts/Release/LiveDeck.app"
[ -d "$APP" ] || { echo "App bundle not found at $APP"; exit 1; }

ENT="$BUILD/livedeck.entitlements"
cat > "$ENT" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>com.apple.security.device.audio-input</key><true/>
</dict></plist>
PLIST

SIGN_ID="${DEVELOPER_ID:--}"
echo "==> Signing (${SIGN_ID})"
TS="--timestamp=none"; [ "$SIGN_ID" != "-" ] && TS="--timestamp"   # notarization needs a secure timestamp
codesign --force --deep --options runtime $TS --entitlements "$ENT" --sign "$SIGN_ID" "$APP"
codesign --verify --deep --strict "$APP"

if [ "$SIGN_ID" != "-" ] && [ -n "${NOTARY_PROFILE:-}" ]; then
  echo "==> Notarizing the app (a few minutes)"
  NZIP="$BUILD/notarize.zip"
  ditto -c -k --keepParent "$APP" "$NZIP"
  xcrun notarytool submit "$NZIP" --keychain-profile "$NOTARY_PROFILE" --wait
  xcrun stapler staple "$APP"
  rm -f "$NZIP"
fi

mkdir -p dist
ZIP="dist/LiveDeck-$VERSION-mac.zip"
echo "==> Packaging $ZIP"
rm -f "$ZIP" "$ZIP.sha256"
ditto -c -k --sequesterRsrc --keepParent "$APP" "$ZIP"
(cd dist && shasum -a 256 "$(basename "$ZIP")" > "$(basename "$ZIP").sha256")

DMG="dist/LiveDeck-$VERSION.dmg"
echo "==> Packaging $DMG"
STAGE=$(mktemp -d)
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
rm -f "$DMG"
hdiutil create -volname "LiveDeck $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
rm -rf "$STAGE"

if [ "$SIGN_ID" != "-" ]; then
  codesign --force --sign "$SIGN_ID" "$DMG"
  if [ -n "${NOTARY_PROFILE:-}" ]; then
    echo "==> Notarizing the disk image"
    xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$DMG"
  fi
fi

if [ "$INSTALL" = 1 ]; then
  echo "==> Installing to /Applications"
  rm -rf "/Applications/LiveDeck.app"
  ditto "$APP" "/Applications/LiveDeck.app"
fi

echo
echo "Done:"
echo "  App: $APP"
echo "  DMG: $DMG"
echo "  Update package: $ZIP (+ .sha256)"
if [ "$INSTALL" = 1 ]; then echo "  Installed: /Applications/LiveDeck.app"; fi
exit 0
