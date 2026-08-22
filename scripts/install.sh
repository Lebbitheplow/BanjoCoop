#!/usr/bin/env bash
#
# Install BanjoCoop into BanjoRecompiled's mods folder.
#
# This ships inside the release archives, next to the two files it installs, so it looks for them
# beside itself first. It also works from a source checkout.
#
# It never touches your ROM or your save files.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"

# versions.sh sits beside this script in a release archive, one level up in a source checkout.
if [ -f "$HERE/versions.sh" ]; then
    . "$HERE/versions.sh"
elif [ -f "$HERE/scripts/versions.sh" ]; then
    . "$HERE/scripts/versions.sh"
else
    BANJORECOMP_VERSION="v1.0.1"
    BANJORECOMP_REPO="BanjoRecomp/BanjoRecomp"
    BANJORECOMP_ASSET_LINUX="BanjoRecompiled-${BANJORECOMP_VERSION}-Linux-X64.zip"
    BANJORECOMP_ASSET_LINUX_ARM64="BanjoRecompiled-${BANJORECOMP_VERSION}-Linux-ARM64.zip"
    banjorecomp_asset_url() {
        echo "https://github.com/${BANJORECOMP_REPO}/releases/download/${BANJORECOMP_VERSION}/$1"
    }
fi

bold() { printf '\033[1m%s\033[0m\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

# ---- --check-rom -------------------------------------------------------------------------------
#
# Not the main job, but the most likely reason somebody is stuck. BanjoRecompiled accepts only
# NTSC-U v1.0 and rejects everything else without saying why, so "my ROM doesn't work" is the
# common support question and this answers it precisely.

if [ "${1:-}" = "--check-rom" ]; then
    rom="${2:-}"
    [ -n "$rom" ] || { echo "usage: $0 --check-rom <path-to-rom>" >&2; exit 2; }
    [ -f "$rom" ] || { echo "no such file: $rom" >&2; exit 1; }

    checker=""
    for c in "$HERE/check_rom.py" "$HERE/scripts/check_rom.py"; do
        [ -f "$c" ] && checker="$c" && break
    done
    if [ -n "$checker" ]; then
        exec python3 "$checker" "$rom"
    fi

    # Release archives may not carry the python script; do the essential check inline.
    want="1fe1632098865f639e22c11b9a81ee8f29c75d7a"
    got="$(sha1sum "$rom" | cut -d' ' -f1)"
    echo "sha1: $got"
    if [ "$got" = "$want" ]; then
        bold "this is the correct ROM (NTSC-U v1.0)"
    else
        bold "this is NOT the ROM BanjoRecompiled accepts"
        echo "expected $want (NTSC-U v1.0)."
        echo "rev A, PAL, and the Xbox Live Arcade version will not work."
        echo "if yours is a .v64 or .n64, the byte order may just need converting -"
        echo "scripts/check_rom.py in the source repo does that with -o out.z64."
        exit 1
    fi
    exit 0
fi

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    cat <<EOF
usage: ./install.sh [--check-rom <rom>]

Copies banjocoop.nrm and banjocoop_net.so into BanjoRecompiled's mods folder.
Offers to download BanjoRecompiled if no install is found.

  --check-rom <rom>   check whether a ROM you have is the revision
                      BanjoRecompiled accepts, then exit

No ROM is included with or downloaded by this project.
EOF
    exit 0
fi

# ---- locate the two files to install ------------------------------------------------------------

find_file() {
    for p in "$HERE/$1" "$HERE/build/$1" "$HERE/build-native/$1" "$HERE/../build/$1" "$HERE/../build-native/$1"; do
        [ -f "$p" ] && echo "$p" && return 0
    done
    return 1
}

NRM="$(find_file banjocoop.nrm)"   || { echo "banjocoop.nrm not found next to this script" >&2; exit 1; }
LIB="$(find_file banjocoop_net.so)" || { echo "banjocoop_net.so not found next to this script" >&2; exit 1; }

# ---- locate BanjoRecompiled ---------------------------------------------------------------------
#
# The config directory is what matters, not where the executable lives: the runtime loads mods
# from ~/.config/BanjoRecompiled/mods regardless of where it was unpacked. (Upstream documents a
# portable.txt marker that would change this; it has no effect in the 1.0.1 release — tested.)

CONFIG_DIR="$HOME/.config/BanjoRecompiled"
MODS_DIR="$CONFIG_DIR/mods"

runtime_present() {
    [ -d "$CONFIG_DIR" ] && return 0
    for p in "$HERE/BanjoRecompiled" "$HERE/../BanjoRecompiled" ./BanjoRecompiled; do
        [ -x "$p" ] && return 0
    done
    return 1
}

if ! runtime_present; then
    bold "no BanjoRecompiled install found."
    echo
    echo "BanjoCoop is a mod - it needs BanjoRecompiled to run in."
    echo "download it now? (${BANJORECOMP_VERSION}, about 8 MB)"
    printf "  [y/N] "
    read -r reply
    case "$reply" in
        [yY]*)
            case "$(uname -m)" in
                x86_64)          asset="$BANJORECOMP_ASSET_LINUX" ;;
                aarch64|arm64)   asset="$BANJORECOMP_ASSET_LINUX_ARM64" ;;
                *) echo "no prebuilt runtime for $(uname -m)" >&2; exit 1 ;;
            esac
            url="$(banjorecomp_asset_url "$asset")"
            tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
            echo "  fetching $asset"
            if have curl; then curl -fL --progress-bar -o "$tmp/rt.zip" "$url"
            else wget -q --show-progress -O "$tmp/rt.zip" "$url"; fi
            have unzip || { echo "unzip is required" >&2; exit 1; }
            unzip -q "$tmp/rt.zip" -d "$tmp/rt"
            exe="$(find "$tmp/rt" -name BanjoRecompiled -type f | head -1)"
            [ -n "$exe" ] || { echo "unexpected archive layout" >&2; exit 1; }
            mkdir -p "$HERE/BanjoRecompiled-$BANJORECOMP_VERSION"
            cp -a "$(dirname "$exe")"/. "$HERE/BanjoRecompiled-$BANJORECOMP_VERSION/"
            chmod +x "$HERE/BanjoRecompiled-$BANJORECOMP_VERSION/BanjoRecompiled"
            echo "  unpacked -> BanjoRecompiled-$BANJORECOMP_VERSION/"
            ;;
        *)
            echo
            echo "get it from:"
            echo "  https://github.com/${BANJORECOMP_REPO}/releases"
            echo
            echo "run it once - it will ask where your Banjo-Kazooie ROM is - then re-run this script."
            exit 1
            ;;
    esac
fi

# ---- install ------------------------------------------------------------------------------------
#
# Both files, side by side. The runtime loads the native library from next to the .nrm rather than
# from inside it, so installing one without the other produces a mod that loads and then cannot
# find its networking.

mkdir -p "$MODS_DIR"
cp "$NRM" "$MODS_DIR/"
cp "$LIB" "$MODS_DIR/"

# cloudflared, if it shipped in this archive, goes beside the library so the Cloudflare-tunnel
# connection mode finds it. Optional: Direct (UDP) play does not need it.
CF="$(find_file cloudflared || true)"
if [ -n "$CF" ]; then
    cp "$CF" "$MODS_DIR/"
    chmod +x "$MODS_DIR/cloudflared" 2>/dev/null || true
fi

bold "BanjoCoop installed"
echo "  $MODS_DIR/banjocoop.nrm"
echo "  $MODS_DIR/banjocoop_net.so"
[ -n "$CF" ] && echo "  $MODS_DIR/cloudflared  (for the Cloudflare Tunnel connection mode)"
cat <<EOF

next:
  1. run BanjoRecompiled (it asks for your own Banjo-Kazooie ROM, NTSC-U v1.0,
     the first time, and copies it into its own folder)
  2. open the mod menu and enable BanjoCoop
  3. set Network Mode to Host, or to Join with the host's address
     default port is 34567/UDP

stuck on the ROM? ./install.sh --check-rom <your-rom>
EOF
