#!/usr/bin/env bash
#
# Get a fresh clone to the point where `make` works.
#
# Three things are needed that are deliberately not in the repository, because all three are
# reproducible and two of them are large:
#
#   1. the submodules      - the decomp headers, symbol tables, ENet, and the recomp toolchain
#   2. tools/RecompModTool - built from vendor/N64Recomp; packages the .nrm
#   3. runtime/            - the BanjoRecompiled release, which `make check-imports` validates
#                            the build against and `make run` launches
#
# Idempotent: run it again any time. Anything already in place is left alone unless --force.

set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
. scripts/versions.sh

FORCE=0
SKIP_RUNTIME=0
for arg in "$@"; do
    case "$arg" in
        --force)        FORCE=1 ;;
        --skip-runtime) SKIP_RUNTIME=1 ;;
        -h|--help)
            cat <<EOF
usage: scripts/bootstrap.sh [--force] [--skip-runtime]

  --force          redo steps even if their output already exists
  --skip-runtime   don't download BanjoRecompiled (CI builds and tests without it,
                   but 'make check-imports' and 'make run' need it)
EOF
            exit 0 ;;
        *) echo "unknown option: $arg (try --help)" >&2; exit 2 ;;
    esac
done

say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

# ---- prerequisites ---------------------------------------------------------------------------
#
# Checked up front and all at once. Finding out about a missing linker after a five minute
# submodule fetch is a poor use of anybody's evening.

missing=""
for tool in git cmake clang python3; do
    have "$tool" || missing="$missing $tool"
done
# The mod links with ld.lld directly, not with whatever `ld` happens to be.
have ld.lld || have ld.lld-14 || missing="$missing lld"
# Either generator works; ninja is what the Makefile asks cmake for.
have ninja || have make || missing="$missing ninja"

if [ -n "$missing" ]; then
    echo "missing required tools:$missing" >&2
    echo >&2
    echo "  Debian/Ubuntu/Pop!_OS:  sudo apt install git cmake ninja-build clang lld python3" >&2
    echo "  Fedora:                 sudo dnf install git cmake ninja-build clang lld python3" >&2
    echo "  Arch:                   sudo pacman -S git cmake ninja clang lld python" >&2
    exit 1
fi

# ---- 1. submodules ---------------------------------------------------------------------------

say "submodules"
if [ ! -f .gitmodules ]; then
    echo "no .gitmodules - is this a clone of the repository?" >&2
    exit 1
fi
git submodule update --init --recursive
echo "ok"

# ---- 2. RecompModTool ------------------------------------------------------------------------
#
# Built from the pinned vendor/N64Recomp (MIT) rather than committed as a binary, so it always
# matches the recomp version everything else here is built against.

say "RecompModTool"
if [ -x tools/RecompModTool ] && [ "$FORCE" -eq 0 ]; then
    echo "already built (--force to rebuild)"
else
    mkdir -p tools
    GEN=""
    have ninja && GEN="-G Ninja"
    # shellcheck disable=SC2086
    cmake -S vendor/N64Recomp -B build/n64recomp $GEN -DCMAKE_BUILD_TYPE=Release > /dev/null
    cmake --build build/n64recomp --target RecompModTool -j"$(nproc 2>/dev/null || echo 4)"
    found="$(find build/n64recomp -name RecompModTool -type f -perm -u+x | head -1)"
    if [ -z "$found" ]; then
        echo "built, but RecompModTool was not where expected under build/n64recomp" >&2
        exit 1
    fi
    cp "$found" tools/RecompModTool
    echo "ok -> tools/RecompModTool"
fi

# ---- 3. the BanjoRecompiled runtime ----------------------------------------------------------
#
# The prebuilt release, unmodified. GPL-3; see NOTICE.md. It contains no game assets and no ROM —
# you supply your own copy of Banjo-Kazooie to it on first run.

if [ "$SKIP_RUNTIME" -eq 1 ]; then
    say "runtime (skipped)"
elif [ -x runtime/BanjoRecompiled ] && [ "$FORCE" -eq 0 ]; then
    say "runtime"
    echo "already present (--force to re-download)"
else
    say "runtime — BanjoRecompiled $BANJORECOMP_VERSION"

    case "$(uname -s)-$(uname -m)" in
        Linux-x86_64)          asset="$BANJORECOMP_ASSET_LINUX" ;;
        Linux-aarch64|Linux-arm64) asset="$BANJORECOMP_ASSET_LINUX_ARM64" ;;
        Darwin-*)              asset="$BANJORECOMP_ASSET_MACOS" ;;
        *)
            echo "no prebuilt runtime for $(uname -s)-$(uname -m)." >&2
            echo "build it yourself, or re-run with --skip-runtime." >&2
            exit 1 ;;
    esac

    url="$(banjorecomp_asset_url "$asset")"
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT

    echo "  $url"
    if have curl; then
        curl -fL --progress-bar -o "$tmp/rt.zip" "$url"
    else
        wget -q --show-progress -O "$tmp/rt.zip" "$url"
    fi

    have unzip || { echo "unzip is required to unpack the runtime" >&2; exit 1; }

    rm -rf runtime
    banjorecomp_unpack "$tmp/rt.zip" runtime || exit 1
    [ -x runtime/BanjoRecompiled ] || { echo "unpacked, but runtime/BanjoRecompiled is missing" >&2; exit 1; }
    echo "ok -> runtime/"
fi

# ---- done ------------------------------------------------------------------------------------

cat <<EOF

$(printf '\033[1mready\033[0m')

  make                 build the mod and the native transport
  make check-imports   verify the build against the shipped runtime
  make test-native     headless transport tests (no ROM, no game)
  make test-modlogic   mod logic against fake game accessors
  make run             launch it

BanjoRecompiled will ask for your own copy of Banjo-Kazooie (NTSC-U v1.0) the
first time it runs. No ROM is included with or downloaded by this project;
scripts/check_rom.py will tell you whether one you have is the right revision.
EOF
