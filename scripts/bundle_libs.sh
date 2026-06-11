#!/usr/bin/env bash
#
# bundle_libs.sh — copy canvas's fetched native dylibs into a shipped app bundle,
# SHA-verified, so the app runs on a machine that never ran fetch_skia.sh.
# (Prod-hardening; docs/decisions/packaging.md.)
#
# Usage:
#   scripts/bundle_libs.sh <App.app | dest-dir>
#
#   * <App.app>   → copies into <App.app>/Contents/Frameworks/  (the macOS bundle
#                   convention; the shim's executable-relative loader finds them
#                   there with zero env setup — see packaging.md §2).
#   * <dest-dir>  → a plain directory (for a flat distribution): copies the dylibs
#                   directly into it.
#
# What it does, in order:
#   1. locate the fetched dylibs (the same search the shim uses: $RUXEN_CANVAS_CACHE
#      else $HOME/.cache/ruxen-canvas).
#   2. SHA-256 verify each against the pin in runtime/fetch_skia.sh (the SINGLE
#      source of truth — we read it, we don't keep a second copy). Refuse on
#      mismatch (a corrupt / wrong-version dylib must never reach a bundle).
#   3. copy into the bundle's Frameworks dir.
#   4. write THIRD_PARTY_LICENSES naming the redistributed libraries' licenses.
#
# Idempotent: re-running over an already-bundled, matching dylib is a no-op copy.
# Exits nonzero on any failure (missing dylib / SHA mismatch / bad args).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # repo root
fetch="$here/runtime/fetch_skia.sh"

die() { echo "bundle_libs: $*" >&2; exit 1; }

[ $# -eq 1 ] || die "usage: bundle_libs.sh <App.app | dest-dir>"
dest="$1"
[ -f "$fetch" ] || die "cannot find runtime/fetch_skia.sh (the SHA pins) at $fetch"

# ---- the sha256 tool (matches fetch_skia.sh's logic) ----
if command -v sha256sum >/dev/null 2>&1; then
  sha256_of() { sha256sum "$1" | awk '{print $1}'; }
elif command -v shasum >/dev/null 2>&1; then
  sha256_of() { shasum -a 256 "$1" | awk '{print $1}'; }
else
  die "need 'sha256sum' or 'shasum' for verification"
fi

# ---- read a pinned binary SHA out of fetch_skia.sh (single source of truth) ----
# The pins live as `<member-path>) echo "<sha>" ;;` lines; grep the member.
pin_for() {
  local member="$1"
  grep -F "$member)" "$fetch" | grep -oE '"[0-9a-f]{64}"' | head -1 | tr -d '"'
}

SKIA_MEMBER="runtimes/osx/native/libSkiaSharp.dylib"
HB_MEMBER="runtimes/osx/native/libHarfBuzzSharp.dylib"
SKIA_PIN="$(pin_for "$SKIA_MEMBER")"
HB_PIN="$(pin_for "$HB_MEMBER")"
[ -n "$SKIA_PIN" ] || die "could not read the libSkiaSharp SHA pin from fetch_skia.sh"
[ -n "$HB_PIN" ]   || die "could not read the libHarfBuzzSharp SHA pin from fetch_skia.sh"

# ---- locate the fetched dylibs (same search order as the shim's cache tier) ----
cache="${RUXEN_CANVAS_CACHE:-$HOME/.cache/ruxen-canvas}"
SKIA_SRC="$cache/libSkiaSharp.dylib"
HB_SRC="$cache/libHarfBuzzSharp.dylib"
[ -f "$SKIA_SRC" ] || die "libSkiaSharp.dylib not in $cache — run runtime/fetch_skia.sh first"
[ -f "$HB_SRC" ]   || die "libHarfBuzzSharp.dylib not in $cache — run runtime/fetch_skia.sh first"

# ---- verify ----
verify() {
  local f="$1" want="$2" got
  got="$(sha256_of "$f")"
  [ "$got" = "$want" ] || die "SHA mismatch for $f
  expected $want
  got      $got
  (wrong version or corrupt download — refusing to bundle)"
  echo "  verified $(basename "$f")  sha256=$got"
}
echo "verifying fetched dylibs against the fetch_skia.sh pins:"
verify "$SKIA_SRC" "$SKIA_PIN"
verify "$HB_SRC"   "$HB_PIN"

# ---- choose the destination Frameworks dir ----
case "$dest" in
  *.app) fw="$dest/Contents/Frameworks" ;;
  *)     fw="$dest" ;;
esac
mkdir -p "$fw"

echo "copying into $fw:"
cp -f "$SKIA_SRC" "$fw/libSkiaSharp.dylib"; echo "  libSkiaSharp.dylib"
cp -f "$HB_SRC"   "$fw/libHarfBuzzSharp.dylib"; echo "  libHarfBuzzSharp.dylib"

# ---- licensing note (packaging.md §4) ----
cat > "$fw/THIRD_PARTY_LICENSES" <<'EOF'
This application bundles native libraries that canvas (the L1 GUI engine)
loads at runtime. Their licenses require this notice in distributed binaries:

  libSkiaSharp.dylib
    * Skia      — BSD-3-Clause (Copyright (c) Google LLC). 2D graphics engine.
    * SkiaSharp — MIT (Copyright (c) Microsoft / .NET Foundation). C-API glue.

  libHarfBuzzSharp.dylib
    * HarfBuzz  — "Old MIT" license (Copyright the HarfBuzz authors). Text shaping.

  (If SDL2 is also bundled rather than used as a system library, it is zlib-licensed.)

The full license texts ship with each upstream project; an application vendor
should reproduce them in their About / acknowledgements. canvas itself is dual
MIT / Apache-2.0 (see LICENSE-MIT / LICENSE-APACHE in the canvas repo).
EOF
echo "  THIRD_PARTY_LICENSES"

echo "bundle_libs: done — $fw is ready (the shim's exe-relative loader finds these with no env setup)."
