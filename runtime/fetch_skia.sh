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

# ---- HarfBuzz (text shaping, docs/SHAPING.md) — fetched alongside Skia ----
# Latin kerning/ligatures + RTL/complex shaping needs a shaper. The fetched
# libSkiaSharp ships NO SkShaper, so we fetch HarfBuzzSharp (HarfBuzz's flat
# `hb_*` C API) — the shim shapes a run with HarfBuzz and renders the positioned
# glyphs with Skia's textblob API. Same fetch+dlopen+SHA-pin discipline. macOS
# only for now (this is where Skia is active); Linux shaping is a later wire-up.
HARFBUZZ_VER="8.3.1.5"
nupkg_sha256_for_pkg_hb() {
  case "$1" in
    harfbuzzsharp.nativeassets.macos) echo "9f733df17a45794db221a592ee23b574302bbe354254656fa4dd495c4c9a104d" ;;
    *)                                echo "" ;;
  esac
}
bin_sha256_for_member_hb() {
  case "$1" in
    runtimes/osx/native/libHarfBuzzSharp.dylib) echo "f8e9ab02b74e68d151abc3781098c9201e57c392b827b40451515d42386d6b0d" ;;
    *)                                          echo "" ;;
  esac
}

NUPKG_SHA256="$(nupkg_sha256_for_pkg "$SKIA_PKG")"
CACHE_DIR="${RUXEN_CANVAS_CACHE:-$HOME/.cache/ruxen-canvas}"
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

mkdir -p "$CACHE_DIR"

# Fetch one SHA-pinned native library from a NuGet package into the cache.
# Args: pkg ver member dest_basename nupkg_sha bin_sha. A miss on the optional
# bin_sha is allowed (extracted unverified, warned); a missing nupkg_sha is a
# hard refusal (no unpinned downloads). Short-circuits when already installed.
fetch_one() {
  local pkg="$1" ver="$2" mem="$3" base="$4" nupkg_sha="$5" bin_sha="$6"
  local dest="$CACHE_DIR/$base"

  if [ -f "$dest" ] && [ -n "$bin_sha" ] && [ "$(sha256_of "$dest")" = "$bin_sha" ]; then
    echo "fetch_skia: up to date ($pkg $ver) -> $dest"
    return 0
  fi
  if [ -z "$nupkg_sha" ]; then
    echo "fetch_skia: no pinned nupkg sha for '$pkg' — refusing unpinned download" >&2
    return 1
  fi

  local tmp; tmp="$(mktemp -d)"
  local url="https://api.nuget.org/v3-flatcontainer/${pkg}/${ver}/${pkg}.${ver}.nupkg"
  echo "fetch_skia: downloading $pkg $ver ..."
  if ! curl -fSL --retry 3 --max-time 300 -o "$tmp/pkg.nupkg" "$url"; then
    echo "fetch_skia: download failed for $pkg $ver" >&2; rm -rf "$tmp"; return 1
  fi

  local got; got="$(sha256_of "$tmp/pkg.nupkg")"
  if [ "$got" != "$nupkg_sha" ]; then
    echo "fetch_skia: nupkg sha256 mismatch for $pkg" >&2
    echo "  expected $nupkg_sha" >&2
    echo "  got      $got" >&2
    rm -rf "$tmp"; return 1
  fi

  echo "fetch_skia: extracting $mem ..."
  if command -v unzip >/dev/null 2>&1; then
    unzip -o -q "$tmp/pkg.nupkg" "$mem" -d "$tmp/ext"
  elif command -v python3 >/dev/null 2>&1; then
    python3 - "$tmp/pkg.nupkg" "$mem" "$tmp/ext" <<'PY'
import sys, zipfile
pkg, member, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
with zipfile.ZipFile(pkg) as z:
    z.extract(member, outdir)
PY
  else
    echo "fetch_skia: need 'unzip' or 'python3' to extract the nupkg" >&2
    rm -rf "$tmp"; return 1
  fi

  local src="$tmp/ext/$mem"
  if [ ! -f "$src" ]; then
    echo "fetch_skia: $mem not found in $pkg" >&2; rm -rf "$tmp"; return 1
  fi
  if [ -n "$bin_sha" ]; then
    local gsha; gsha="$(sha256_of "$src")"
    if [ "$gsha" != "$bin_sha" ]; then
      echo "fetch_skia: extracted binary sha256 mismatch ($pkg)" >&2
      echo "  expected $bin_sha" >&2
      echo "  got      $gsha" >&2
      rm -rf "$tmp"; return 1
    fi
  else
    echo "fetch_skia: note — $pkg binary not sha-pinned yet (extracted unverified)" >&2
  fi

  install -m 0644 "$src" "$dest"
  echo "fetch_skia: installed -> $dest ($(du -h "$dest" | awk '{print $1}'), sha256 $(sha256_of "$dest" | cut -c1-16)...)"
  rm -rf "$tmp"
  return 0
}

# Skia (required).
fetch_one "$SKIA_PKG" "$SKIA_VER" "$member" "$DEST_BASENAME" \
          "$NUPKG_SHA256" "$EXPECT_SO_SHA"

# HarfBuzz (macOS only this round; a miss is non-fatal — shaping just stays
# unavailable and the non-shaped text path remains the fallback).
if [ "$os" = "Darwin" ]; then
  hb_member="runtimes/osx/native/libHarfBuzzSharp.dylib"
  fetch_one "harfbuzzsharp.nativeassets.macos" "$HARFBUZZ_VER" "$hb_member" \
            "libHarfBuzzSharp.dylib" \
            "$(nupkg_sha256_for_pkg_hb harfbuzzsharp.nativeassets.macos)" \
            "$(bin_sha256_for_member_hb "$hb_member")" \
    || echo "fetch_skia: HarfBuzz fetch failed — shaping will be unavailable (non-fatal)" >&2
fi
