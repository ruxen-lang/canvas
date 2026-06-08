#!/usr/bin/env bash
#
# fetch_skia.sh — fetch the prebuilt Skia native library (libSkiaSharp) that
# the canvas C shim (runtime/skia_shim.c) dlopen()s at runtime.
#
# Why a fetch script instead of a committed binary:
#   * libSkiaSharp.so is ~11 MB per arch — a public-repo binary blob we'd
#     rather not carry. Only the tiny C-API header (runtime/skia/skia_capi.h)
#     is committed; the binary is fetched + SHA-pinned here.
#   * The shim dlopen()s the result exactly like SDL2 (see sdl_window.c), so
#     there is zero link-time dependency: no -lSkiaSharp, no dev package.
#
# Sources (host-aware — both SHA-pinned):
#   * Linux  → SkiaSharp.NativeAssets.Linux  → runtimes/<linux-RID>/native/libSkiaSharp.so
#   * macOS  → SkiaSharp.NativeAssets.macOS   → runtimes/osx/native/libSkiaSharp.dylib
#              (a single UNIVERSAL fat Mach-O covering arm64 + x86_64 — one file
#               serves both Mac arches; this build has SK_METAL=1, so Metal is
#               real here, unlike the Linux .so. See docs/GPU.md.)
# Both are the same prebuilt Skia that ships behind Avalonia / Uno / .NET MAUI,
# exporting Skia's flat `sk_*` C API, which is what skia_capi.h binds.
#
# Install location (canonical, CWD-independent — the shim looks here):
#   ${RUXEN_CANVAS_CACHE:-$HOME/.cache/ruxen-canvas}/libSkiaSharp.{so,dylib}
# The basename matches the platform's convention (.so on Linux, .dylib on macOS)
# so the shim's loader can probe the right name. Override at runtime with
# $RUXEN_CANVAS_SKIA (absolute path to the .so / .dylib).
#
# Idempotent: re-running with the expected SHA already in place is a no-op.
set -euo pipefail

# ---- pinned versions + checksums (update the per-OS triple together) ----
SKIA_VER="3.119.4"

# nupkg sha256 per source package (pin every download — no unpinned fetch).
nupkg_sha256_for_pkg() {
  case "$1" in
    skiasharp.nativeassets.linux) echo "fae0554059b1107ef7888e46c20bdfb548401ef7a7a6f7391ad4fadc7432d50a" ;;
    skiasharp.nativeassets.macos) echo "f7f2f539ce5bba337aa4a8d6eac25caf58cbdd12edf3f32ddcc98294e730cf2c" ;;
    *)                            echo "" ;;
  esac
}

# Expected sha256 of the extracted native binary, keyed by package member path.
bin_sha256_for_member() {
  case "$1" in
    runtimes/linux-x64/native/libSkiaSharp.so) echo "66c856eaf1a47a00b23204c30c6ee407987bf5086ecc0a1a6b4fd67526b0cd02" ;;
    runtimes/osx/native/libSkiaSharp.dylib)    echo "e09f07ae1df62ded475351f56d8dc8366cd679043f28b65d9dee597a5fd0da6c" ;;
    *)                                         echo "" ;;   # not sha-pinned yet
  esac
}

# ---- resolve host OS + arch -> nuget package, member path, install basename ----
os="$(uname -s)"
arch="$(uname -m)"
case "$os" in
  Darwin)
    # macOS: ONE universal dylib at runtimes/osx/native (no per-arch RID dir).
    SKIA_PKG="skiasharp.nativeassets.macos"
    member="runtimes/osx/native/libSkiaSharp.dylib"
    DEST_BASENAME="libSkiaSharp.dylib"
    RID="osx"
    ;;
  Linux)
    case "$arch" in
      x86_64|amd64)   RID="linux-x64"   ;;
      aarch64|arm64)  RID="linux-arm64" ;;
      armv7l|armhf)   RID="linux-arm"   ;;
      *) echo "fetch_skia: unsupported Linux arch '$arch'" >&2; exit 1 ;;
    esac
    SKIA_PKG="skiasharp.nativeassets.linux"
    member="runtimes/${RID}/native/libSkiaSharp.so"
    DEST_BASENAME="libSkiaSharp.so"
    ;;
  *)
    echo "fetch_skia: unsupported OS '$os'" >&2; exit 1 ;;
esac

NUPKG_SHA256="$(nupkg_sha256_for_pkg "$SKIA_PKG")"
CACHE_DIR="${RUXEN_CANVAS_CACHE:-$HOME/.cache/ruxen-canvas}"
DEST="$CACHE_DIR/$DEST_BASENAME"
EXPECT_SO_SHA="$(bin_sha256_for_member "$member")"

# sha256 tool differs by platform: sha256sum on Linux, shasum -a 256 on macOS.
if command -v sha256sum >/dev/null 2>&1; then
  sha256_of() { sha256sum "$1" | awk '{print $1}'; }
elif command -v shasum >/dev/null 2>&1; then
  sha256_of() { shasum -a 256 "$1" | awk '{print $1}'; }
else
  echo "fetch_skia: need 'sha256sum' or 'shasum' for checksum verification" >&2
  exit 1
fi

# ---- short-circuit if already installed and matching ----
if [ -f "$DEST" ] && [ -n "$EXPECT_SO_SHA" ] && [ "$(sha256_of "$DEST")" = "$EXPECT_SO_SHA" ]; then
  echo "fetch_skia: up to date ($RID, $SKIA_VER) -> $DEST"
  exit 0
fi

if [ -z "$NUPKG_SHA256" ]; then
  echo "fetch_skia: no pinned nupkg sha for '$SKIA_PKG' — refusing unpinned download" >&2
  exit 1
fi

mkdir -p "$CACHE_DIR"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

url="https://api.nuget.org/v3-flatcontainer/${SKIA_PKG}/${SKIA_VER}/${SKIA_PKG}.${SKIA_VER}.nupkg"
echo "fetch_skia: downloading $SKIA_PKG $SKIA_VER ($RID) ..."
curl -fSL --retry 3 --max-time 300 -o "$tmp/pkg.nupkg" "$url"

got_nupkg_sha="$(sha256_of "$tmp/pkg.nupkg")"
if [ "$got_nupkg_sha" != "$NUPKG_SHA256" ]; then
  echo "fetch_skia: nupkg sha256 mismatch" >&2
  echo "  expected $NUPKG_SHA256" >&2
  echo "  got      $got_nupkg_sha" >&2
  exit 1
fi

echo "fetch_skia: extracting $member ..."
if command -v unzip >/dev/null 2>&1; then
  unzip -o -q "$tmp/pkg.nupkg" "$member" -d "$tmp/ext"
elif command -v python3 >/dev/null 2>&1; then
  python3 - "$tmp/pkg.nupkg" "$member" "$tmp/ext" <<'PY'
import sys, zipfile, os
pkg, member, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
with zipfile.ZipFile(pkg) as z:
    z.extract(member, outdir)
PY
else
  echo "fetch_skia: need 'unzip' or 'python3' to extract the nupkg" >&2
  exit 1
fi

src="$tmp/ext/$member"
if [ ! -f "$src" ]; then
  echo "fetch_skia: $member not found in package" >&2
  exit 1
fi

if [ -n "$EXPECT_SO_SHA" ]; then
  got_so_sha="$(sha256_of "$src")"
  if [ "$got_so_sha" != "$EXPECT_SO_SHA" ]; then
    echo "fetch_skia: extracted binary sha256 mismatch ($RID)" >&2
    echo "  expected $EXPECT_SO_SHA" >&2
    echo "  got      $got_so_sha" >&2
    exit 1
  fi
else
  echo "fetch_skia: note — $RID is not sha-pinned yet (extracted unverified)" >&2
fi

install -m 0644 "$src" "$DEST"
echo "fetch_skia: installed -> $DEST"
echo "fetch_skia: ($(du -h "$DEST" | awk '{print $1}'), sha256 $(sha256_of "$DEST" | cut -c1-16)...)"
