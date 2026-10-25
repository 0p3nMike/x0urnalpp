#!/usr/bin/env bash
# build-linux.sh -- build X0urnal++ from this checkout and install it into its own prefix, beside
# any Xournal++ the distribution ships. Settings live in ~/.config/X0urnal++, so the two never mix.
#
#   x0urnal/build-linux.sh              build, then install with sudo
#   INSTALL=0 x0urnal/build-linux.sh    build only; prints the install command
#   x0urnal/build-linux.sh --tools      also put book-sync on PATH (~/.local/bin)
#
# Environment: PREFIX (default /opt/X0urnal++), BUILD (default <checkout>/build), JOBS.
# Dependencies: see the README ("Build on Linux") or readme/LinuxBuild.md.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PREFIX=${PREFIX:-/opt/X0urnal++}
BUILD=${BUILD:-$ROOT/build}
JOBS=${JOBS:-$(nproc)}
TOOLS=0
[ "${1:-}" = --tools ] && TOOLS=1

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX"
ninja -C "$BUILD" -j"$JOBS"

if [ "${INSTALL:-1}" = 1 ]; then
    sudo ninja -C "$BUILD" install
    sudo ln -sfn xournalpp "$PREFIX/bin/X0urnal++"
    echo "installed: $PREFIX/bin/X0urnal++"
else
    echo "built, not installed. Install with:"
    echo "  sudo ninja -C '$BUILD' install && sudo ln -sfn xournalpp '$PREFIX/bin/X0urnal++'"
fi

if [ "$TOOLS" = 1 ]; then
    # Links, not copies: book-sync writes its merge drivers into each book's .git/config by their
    # real path, so the checkout stays the one place they run from.
    mkdir -p "$HOME/.local/bin"
    for f in book-sync xopp-merge notebook-merge xopp-filter; do
        ln -sfn "$ROOT/book-sync/$f" "$HOME/.local/bin/$f"
    done
    echo "book-sync linked into ~/.local/bin"
fi
