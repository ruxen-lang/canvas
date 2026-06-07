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
# Source: SkiaSharp.NativeAssets.Linux (NuGet) — the same prebuilt Skia that
# ships behind Avalonia / Uno / .NET MAUI. It exports Skia's flat `sk_*` C API
# (812 functions), which is what skia_capi.h binds.
#
# Install location (canonical, CWD-independent — the shim looks here):
#   ${RUXEN_CANVAS_CACHE:-$HOME/.cache/ruxen-canvas}/libSkiaSharp.so
# Override at runtime with $RUXEN_CANVAS_SKIA (absolute path to the .so).
#
# Idempotent: re-running with the expected SHA already in place is a no-op.
set -euo pipefail

# ---- pinned version + checksums (update together) ----
SKIA_PKG="skiasharp.nativeassets.linux"
SKIA_VER="3.119.4"
NUPKG_SHA256="fae0554059b1107ef7888e46c20bdfb548401ef7a7a6f7391ad4fadc7432d50a"

# Per-arch: nuget runtime-id (RID) -> expected sha256 of the extracted .so.
so_sha256_for_rid() {
  case "$1" in
    linux-x64)   echo "66c856eaf1a47a00b23204c30c6ee407987bf5086ecc0a1a6b4fd67526b0cd02" ;;
    *)           echo "" ;;   # other arches: fetched but not sha-pinned yet
  esac
}

# ---- resolve host arch -> nuget RID ----
arch="$(uname -m)"
case "$arch" in
  x86_64|amd64)   RID="linux-x64"   ;;
  aarch64|arm64)  RID="linux-arm64" ;;
  armv7l|armhf)   RID="linux-arm"   ;;
  *) echo "fetch_skia: unsupported arch '$arch'" >&2; exit 1 ;;
esac

CACHE_DIR="${RUXEN_CANVAS_CACHE:-$HOME/.cache/ruxen-canvas}"
DEST="$CACHE_DIR/libSkiaSharp.so"
EXPECT_SO_SHA="$(so_sha256_for_rid "$RID")"

sha256_of() { sha256sum "$1" | awk '{print $1}'; }

# ---- short-circuit if already installed and matching ----
if [ -f "$DEST" ] && [ -n "$EXPECT_SO_SHA" ] && [ "$(sha256_of "$DEST")" = "$EXPECT_SO_SHA" ]; then
  echo "fetch_skia: up to date ($RID, $SKIA_VER) -> $DEST"
  exit 0
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

member="runtimes/${RID}/native/libSkiaSharp.so"
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
    echo "fetch_skia: extracted .so sha256 mismatch ($RID)" >&2
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
