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
    skiasharp.nativeassets.win32) echo "5a5698b1b4e1fdc9ffe9868df6874db5fa69f21a4de76ba71a01a542e9b43391" ;;
    *)                            echo "" ;;
  esac
}

# Expected sha256 of the extracted native binary, keyed by package member path.
# Windows (win-*) members are pinned but EXPERIMENTAL — fetchable + verifiable from
# any host, but the shim is compiles-untested-until-CI (docs/ROADMAP.md Phase 4).
bin_sha256_for_member() {
  case "$1" in
    runtimes/linux-x64/native/libSkiaSharp.so)    echo "66c856eaf1a47a00b23204c30c6ee407987bf5086ecc0a1a6b4fd67526b0cd02" ;;
    runtimes/linux-arm64/native/libSkiaSharp.so)  echo "87d2d56c49a9b1d1da618dfa20994ee213f752f9ada04e085703115162997aef" ;;
    runtimes/osx/native/libSkiaSharp.dylib)       echo "e09f07ae1df62ded475351f56d8dc8366cd679043f28b65d9dee597a5fd0da6c" ;;
    runtimes/win-x64/native/libSkiaSharp.dll)     echo "7dec3ba900ab353491e6446f0083739924c6f8dd668832e2f09d38ebffdbbe1c" ;;
    runtimes/win-arm64/native/libSkiaSharp.dll)   echo "f7409fcfc3557e272d3e8df2dcd6f737ee5a68b09fdbc6d6ff8c9a3c24b2e36a" ;;
    *)                                            echo "" ;;   # not sha-pinned yet
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
  MINGW*|MSYS*|CYGWIN*|Windows_NT)
    # Windows (EXPERIMENTAL, docs/ROADMAP.md Phase 4). Bash under Git-Bash / MSYS2
    # reports MINGW*/MSYS*; the Skia native package for Windows is the .Win32
    # variant (per-arch RID under runtimes/win-<arch>/native/libSkiaSharp.dll).
    case "$arch" in
      x86_64|amd64)   RID="win-x64"   ;;
      aarch64|arm64)  RID="win-arm64" ;;
      *) echo "fetch_skia: unsupported Windows arch '$arch'" >&2; exit 1 ;;
    esac
    SKIA_PKG="skiasharp.nativeassets.win32"
    member="runtimes/${RID}/native/libSkiaSharp.dll"
    DEST_BASENAME="libSkiaSharp.dll"
    ;;
  *)
    echo "fetch_skia: unsupported OS '$os'" >&2; exit 1 ;;
esac

# ---- HarfBuzz (text shaping, docs/SHAPING.md) — fetched alongside Skia ----
# Latin kerning/ligatures + RTL/complex shaping needs a shaper. The fetched
# libSkiaSharp ships NO SkShaper, so we fetch HarfBuzzSharp (HarfBuzz's flat
# `hb_*` C API) — the shim shapes a run with HarfBuzz and renders the positioned
# glyphs with Skia's textblob API. Same fetch+dlopen+SHA-pin discipline. Host-aware:
# macOS universal dylib + Linux per-RID .so, both SHA-pinned (Phase-4: Linux shaping
# now wired so the container suite's shaping pins run on-platform).
HARFBUZZ_VER="8.3.1.5"
nupkg_sha256_for_pkg_hb() {
  case "$1" in
    harfbuzzsharp.nativeassets.macos) echo "9f733df17a45794db221a592ee23b574302bbe354254656fa4dd495c4c9a104d" ;;
    harfbuzzsharp.nativeassets.linux) echo "6b95b9cddec035d0a85accd18a29c42038ac7714d366a004494de7f0b9b66157" ;;
    harfbuzzsharp.nativeassets.win32) echo "940856fef8c9373754e5443e21e7f00c716c91e44e6d062d3ffce4c63bab2bcd" ;;
    *)                                echo "" ;;
  esac
}
bin_sha256_for_member_hb() {
  case "$1" in
    runtimes/osx/native/libHarfBuzzSharp.dylib)         echo "f8e9ab02b74e68d151abc3781098c9201e57c392b827b40451515d42386d6b0d" ;;
    runtimes/linux-arm64/native/libHarfBuzzSharp.so)    echo "68fc5ef842ac0dbd57e8063a11de5bb2f3c2b5af530a2d4fa07a7cf5d6ea4259" ;;
    runtimes/linux-x64/native/libHarfBuzzSharp.so)      echo "1d5c3afef13545bf34bf8f068b14e25ee619c3b6dee235c44e260fe61cb24018" ;;
    runtimes/win-x64/native/libHarfBuzzSharp.dll)       echo "cf97ae00945e6f5290967ba4ff3051f1cd47a0758b7d9a7159fb352b33655d43" ;;
    runtimes/win-arm64/native/libHarfBuzzSharp.dll)     echo "e8c9923c68c08e83f632e18fec2c9dd812f64e1e553e4b6702b5396b3834f821" ;;
    *)                                                  echo "" ;;
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

# HarfBuzz (host-aware; a miss is non-fatal — shaping just stays unavailable and
# the non-shaped text path remains the fallback). The package + member + install
# basename mirror the Skia host-selection above.
case "$os" in
  Darwin)
    HB_PKG="harfbuzzsharp.nativeassets.macos"
    hb_member="runtimes/osx/native/libHarfBuzzSharp.dylib"
    HB_BASENAME="libHarfBuzzSharp.dylib"
    ;;
  Linux)
    HB_PKG="harfbuzzsharp.nativeassets.linux"
    hb_member="runtimes/${RID}/native/libHarfBuzzSharp.so"
    HB_BASENAME="libHarfBuzzSharp.so"
    ;;
  MINGW*|MSYS*|CYGWIN*|Windows_NT)
    HB_PKG="harfbuzzsharp.nativeassets.win32"
    hb_member="runtimes/${RID}/native/libHarfBuzzSharp.dll"
    HB_BASENAME="libHarfBuzzSharp.dll"
    ;;
  *)
    HB_PKG="" ;;
esac
if [ -n "$HB_PKG" ]; then
  fetch_one "$HB_PKG" "$HARFBUZZ_VER" "$hb_member" "$HB_BASENAME" \
            "$(nupkg_sha256_for_pkg_hb "$HB_PKG")" \
            "$(bin_sha256_for_member_hb "$hb_member")" \
    || echo "fetch_skia: HarfBuzz fetch failed — shaping will be unavailable (non-fatal)" >&2
fi
