#!/usr/bin/env bash
set -euo pipefail

# Reproducible build env for Enigma (macOS + Homebrew, Apple Silicon)
# Run from the Enigma repository root.

# --- Homebrew prefix (Apple Silicon default) ---
HOMEBREW_PREFIX="${HOMEBREW_PREFIX:-/opt/homebrew}"

# If you want to auto-detect instead, uncomment:
# HOMEBREW_PREFIX="$(brew --prefix)"

# --- Tooling / pkg-config ---
export PATH="$HOMEBREW_PREFIX/bin:$PATH"
export PKG_CONFIG_PATH="$HOMEBREW_PREFIX/lib/pkgconfig:$HOMEBREW_PREFIX/share/pkgconfig:${PKG_CONFIG_PATH:-}"

# --- Autotools: make sure aclocal can find gettext macros (AM_GNU_GETTEXT, etc.) ---
# Homebrew gettext keeps macros here:
#   /opt/homebrew/opt/gettext/share/gettext/m4
# and sometimes also:
#   /opt/homebrew/opt/gettext/share/aclocal
GETTEXT_PREFIX="$(brew --prefix gettext 2>/dev/null || true)"
if [[ -n "${GETTEXT_PREFIX}" && -d "${GETTEXT_PREFIX}/share/gettext/m4" ]]; then
  export ACLOCAL_PATH="${GETTEXT_PREFIX}/share/gettext/m4:${GETTEXT_PREFIX}/share/aclocal:${ACLOCAL_PATH:-}"
else
  # fallback: still set something reasonable
  export ACLOCAL_PATH="${HOMEBREW_PREFIX}/share/aclocal:${ACLOCAL_PATH:-}"
fi

# --- Compiler / linker flags so ./configure finds Homebrew headers & libs ---
# This is what fixed the libpng (-lpng) check for you.
export CPPFLAGS="-I${HOMEBREW_PREFIX}/include ${CPPFLAGS:-}"
export LDFLAGS="-L${HOMEBREW_PREFIX}/lib ${LDFLAGS:-}"

# Helpful on macOS for runtime lookup while testing locally (optional)
export DYLD_LIBRARY_PATH="${HOMEBREW_PREFIX}/lib:${DYLD_LIBRARY_PATH:-}"

# --- C/C++ flags (optional, conservative defaults) ---
export CFLAGS="${CFLAGS:- -O2 -g}"
export CXXFLAGS="${CXXFLAGS:- -O2 -g}"

# --- Make parallelism ---
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

# --- Clean stale configure cache (helps when flags change) ---
rm -f config.cache
rm -rf autom4te.cache

# --- Bootstrap + configure + build ---
./autogen.sh
./configure "$@"
make -j"$JOBS"

echo
echo "Build finished. If you want to install:"
echo "  sudo make install"

