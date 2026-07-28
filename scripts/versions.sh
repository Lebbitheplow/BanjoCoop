#!/usr/bin/env bash
# The one place the pinned upstream version lives.
#
# bootstrap.sh fetches this release to build and test against; the packaging script bundles this
# same release; install.sh offers to download it. Changing it here changes all three, which is the
# point — a bundle built against one runtime and tested against another is a bug waiting to be
# blamed on the mod.

# Upstream release tag, and the exact asset names published under it.
BANJORECOMP_VERSION="v1.0.1"
BANJORECOMP_REPO="BanjoRecomp/BanjoRecomp"

BANJORECOMP_ASSET_LINUX="BanjoRecompiled-${BANJORECOMP_VERSION}-Linux-X64.zip"
BANJORECOMP_ASSET_WINDOWS="BanjoRecompiled-${BANJORECOMP_VERSION}-Windows.zip"
BANJORECOMP_ASSET_LINUX_ARM64="BanjoRecompiled-${BANJORECOMP_VERSION}-Linux-ARM64.zip"
BANJORECOMP_ASSET_MACOS="BanjoRecompiled-${BANJORECOMP_VERSION}-macOS.zip"

banjorecomp_asset_url() {
    echo "https://github.com/${BANJORECOMP_REPO}/releases/download/${BANJORECOMP_VERSION}/$1"
}

# Unpack an upstream release archive into a directory, whatever shape it happens to be.
#
#   banjorecomp_unpack <archive.zip> <destdir>
#
# The two platforms are not packaged the same way, which is worth knowing before assuming either:
#
#   Linux    the zip contains a single BanjoRecompiled.tar.gz, and the executable and assets/ are
#            inside that
#   Windows  the zip contains BanjoRecompiled.exe, its DLLs (SDL2, dxcompiler, dxil) and assets/
#            directly, with no nesting
#
# Rather than encode either layout, this finds the executable wherever it ended up and treats its
# directory as the runtime root — which also picks up the Windows DLLs for free, since they sit
# beside the exe. Lives here because bootstrap.sh and package.sh both need it, and a layout change
# upstream should only have to be fixed once.
banjorecomp_unpack() {
    _bru_zip="$1"
    _bru_dest="$2"
    _bru_tmp="$(mktemp -d)"

    unzip -q "$_bru_zip" -d "$_bru_tmp/x" || { rm -rf "$_bru_tmp"; return 1; }

    # Linux ships a tarball inside the zip.
    for _bru_tar in "$_bru_tmp/x"/*.tar.gz "$_bru_tmp/x"/*.tgz; do
        [ -f "$_bru_tar" ] || continue
        mkdir -p "$_bru_tmp/x/untarred"
        tar xzf "$_bru_tar" -C "$_bru_tmp/x/untarred" || { rm -rf "$_bru_tmp"; return 1; }
    done

    _bru_exe="$(find "$_bru_tmp/x" \( -name BanjoRecompiled -o -name BanjoRecompiled.exe \) -type f 2>/dev/null | head -1)"
    if [ -z "$_bru_exe" ]; then
        echo "no BanjoRecompiled executable inside $(basename "$_bru_zip") - upstream layout changed?" >&2
        rm -rf "$_bru_tmp"
        return 1
    fi

    mkdir -p "$_bru_dest"
    cp -a "$(dirname "$_bru_exe")"/. "$_bru_dest"/
    [ -f "$_bru_dest/BanjoRecompiled" ] && chmod +x "$_bru_dest/BanjoRecompiled"

    rm -rf "$_bru_tmp"
    return 0
}
