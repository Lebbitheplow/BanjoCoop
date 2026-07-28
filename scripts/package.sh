#!/usr/bin/env bash
#
# Assemble the release archives into dist/.
#
#   scripts/package.sh <os>              mod-only archive
#   scripts/package.sh <os> --bundle     bundle, including BanjoRecompiled
#
# <os> is linux or windows. The .nrm is byte-identical on every platform — only the native
# transport differs — so a Windows archive can be assembled anywhere as long as the .dll has been
# built somewhere and put in build-native/.
#
# The version comes from config/mod.toml, so it is set in exactly one place.

set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
. scripts/versions.sh

OS="${1:-}"
BUNDLE=0
[ "${2:-}" = "--bundle" ] && BUNDLE=1

case "$OS" in
    linux)   LIB_NAME="banjocoop_net.so";  ASSET="$BANJORECOMP_ASSET_LINUX";   EXE="BanjoRecompiled" ;;
    windows) LIB_NAME="banjocoop_net.dll"; ASSET="$BANJORECOMP_ASSET_WINDOWS"; EXE="BanjoRecompiled.exe" ;;
    *) echo "usage: $0 <linux|windows> [--bundle]" >&2; exit 2 ;;
esac

VERSION="$(grep -m1 '^version = ' config/mod.toml | cut -d'"' -f2)"
[ -n "$VERSION" ] || { echo "could not read version from config/mod.toml" >&2; exit 1; }

KIND="mod-only"; [ "$BUNDLE" -eq 1 ] && KIND="bundle"
NAME="banjocoop-${VERSION}-${OS}-${KIND}"
STAGE="dist/$NAME"

echo "==> $NAME"

# ---- the mod itself ----------------------------------------------------------------------------

[ -f build/banjocoop.nrm ] || { echo "build/banjocoop.nrm missing - run 'make' first" >&2; exit 1; }
LIB="build-native/$LIB_NAME"
[ -f "$LIB" ] || { echo "$LIB missing - build the $OS transport first" >&2; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE"
cp build/banjocoop.nrm "$STAGE/"
cp "$LIB" "$STAGE/"
cp NOTICE.md LICENSE "$STAGE/"
cp scripts/versions.sh "$STAGE/"
if [ "$OS" = "windows" ]; then cp scripts/install.ps1 "$STAGE/"; else cp scripts/install.sh "$STAGE/"; fi
cp scripts/check_rom.py "$STAGE/"
chmod +x "$STAGE/install.sh" 2>/dev/null || true

# ---- the runtime, for a bundle ------------------------------------------------------------------
#
# Upstream's release artifact, unpacked and unmodified. GPL-3: NOTICE.md names the exact version
# and points at the source, which is what §6 asks for when both come from the same place.

if [ "$BUNDLE" -eq 1 ]; then
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    url="$(banjorecomp_asset_url "$ASSET")"
    echo "    fetching $ASSET"
    if command -v curl >/dev/null; then curl -fL --progress-bar -o "$tmp/rt.zip" "$url"
    else wget -q -O "$tmp/rt.zip" "$url"; fi

    banjorecomp_unpack "$tmp/rt.zip" "$STAGE" || exit 1
    [ -f "$STAGE/$EXE" ] || { echo "unpacked, but no $EXE in the staged bundle" >&2; exit 1; }

    mkdir -p "$STAGE/third_party/BanjoRecompiled"
    cp third_party/BanjoRecompiled/COPYING "$STAGE/third_party/BanjoRecompiled/"
    echo "$BANJORECOMP_VERSION" > "$STAGE/BUNDLED_VERSION"
fi

# ---- README.txt ---------------------------------------------------------------------------------

{
    echo "BanjoCoop $VERSION - online multiplayer for Banjo-Kazooie"
    echo "https://github.com/Lebbitheplow/BanjoCoop"
    echo
    if [ "$BUNDLE" -eq 1 ]; then
        echo "This archive contains BanjoCoop and BanjoRecompiled $BANJORECOMP_VERSION."
    else
        echo "This archive contains BanjoCoop only. You need BanjoRecompiled:"
        echo "  https://github.com/${BANJORECOMP_REPO}/releases"
    fi
    echo
    echo "YOU NEED YOUR OWN COPY OF BANJO-KAZOOIE."
    echo "No game data is included here or downloaded by anything in this archive."
    echo "BanjoRecompiled asks where your ROM is the first time it runs and copies"
    echo "it into its own folder. Only the North American v1.0 (NTSC-U rev0) release"
    echo "is accepted - no rev A, no PAL, no Xbox Live Arcade version."
    echo
    echo "INSTALL"
    if [ "$OS" = "windows" ]; then
        echo "  1. Right-click install.ps1 -> Run with PowerShell"
        echo "     (or: powershell -ExecutionPolicy Bypass -File .\\install.ps1)"
    else
        echo "  1. ./install.sh"
    fi
    echo "  2. Run BanjoRecompiled and point it at your ROM."
    echo "  3. Mod menu -> enable BanjoCoop -> set Network Mode to Host or Join."
    echo
    echo "  Default port is 34567/UDP. The host needs it reachable."
    echo
    echo "ROM REJECTED?"
    if [ "$OS" = "windows" ]; then
        echo "  .\\install.ps1 -CheckRom <your-rom>"
    else
        echo "  ./install.sh --check-rom <your-rom>"
    fi
    echo "  It reports the SHA-1 and tells you whether it is the right revision."
    echo
    echo "STATUS"
    echo "  Two players in a world, shared progress and free-roam are played and work."
    echo "  Enemies, bosses, projectiles, revive, carrying and party modes are built"
    echo "  and tested off-game but barely played. Reports are the point of this build."
    echo
    echo "LICENCE"
    echo "  BanjoCoop is GPL-3.0-or-later. See LICENSE and NOTICE.md."
    if [ "$BUNDLE" -eq 1 ]; then
        echo "  BanjoRecompiled is GPL-3.0, bundled unmodified; see third_party/ and NOTICE.md."
    fi
} > "$STAGE/README.txt"

# ---- zip -----------------------------------------------------------------------------------------

( cd dist && rm -f "$NAME.zip" && zip -qr "$NAME.zip" "$NAME" )
rm -rf "$STAGE"

echo "    dist/$NAME.zip  ($(du -h "dist/$NAME.zip" | cut -f1))"
