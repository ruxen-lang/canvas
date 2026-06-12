#!/usr/bin/env bash
#
# linux_verify.sh — build the ruxen toolchain from source, stage an install, fetch
# the Linux Skia/HarfBuzz blobs, compile the C shim, and run the FULL canvas pin
# suite HEADLESS on Linux. This is the Linux half of the Phase-4 platform matrix.
#
# It runs in two places, sharing this one script (no duplicated logic):
#   * Locally, inside Dockerfile.linux-verify (a debian/ubuntu arm64 container on a
#     macOS host with Docker) — `docker build -f Dockerfile.linux-verify .` invokes
#     it. The container suite result is the local Linux verification.
#   * In CI on a NATIVE ubuntu-latest runner (.github/workflows/ci.yml) — no Docker
#     needed there; the runner IS Linux.
#
# Inputs (env, all have container/CI-friendly defaults):
#   RUXEN_SRC      ruxen checkout to build from   (default: /ruxen)
#   CANVAS_DIR     canvas checkout to test         (default: this script's repo root)
#   RUXEN_PREFIX   where to stage the toolchain    (default: /opt/ruxen)
#   CC             C compiler for the shim          (default: cc)
#
# Exit status: 0 iff the toolchain built, the blobs verified, the shim compiled
# warnings-clean, AND `ruxen test` reported zero failures. Nonzero otherwise — the
# caller (Docker build / CI job) fails loudly. NOTHING here forks a real window: the
# suite is the deterministic headless framebuffer path; SDL2 (if present) runs under
# the dummy video driver. The suite is HONEST per-platform: macOS-only capabilities
# (Metal pixel pins, NSAccessibility) gate themselves off via capability probes and
# assert their clean-unavailable contract here, never a skip.
set -uo pipefail

RUXEN_SRC="${RUXEN_SRC:-/ruxen}"
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CANVAS_DIR="${CANVAS_DIR:-$(cd "$SELF_DIR/.." && pwd)}"
RUXEN_PREFIX="${RUXEN_PREFIX:-/opt/ruxen}"
CC="${CC:-cc}"

say()  { printf '\n=== %s ===\n' "$1"; }
die()  { printf 'linux_verify: FAIL: %s\n' "$1" >&2; exit 1; }

# Headless + fork-safe: never try to touch a real display/WindowServer. The dummy
# SDL video driver lets the clipboard/cursor probes exercise their SDL paths without
# an X server; RUXEN_TEST_FORMAT is set by `ruxen test` itself per case.
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"

# ---- 0. sanity ----
say "0/5 environment"
command -v cargo >/dev/null 2>&1 || die "cargo not on PATH (need the Rust toolchain)"
command -v "$CC" >/dev/null 2>&1 || die "C compiler '$CC' not found"
[ -f "$RUXEN_SRC/Cargo.toml" ]   || die "ruxen source not at RUXEN_SRC=$RUXEN_SRC"
[ -f "$CANVAS_DIR/Ruxen.toml" ]  || die "canvas not at CANVAS_DIR=$CANVAS_DIR"
echo "host:    $(uname -srm)"
echo "ruxen:   $RUXEN_SRC"
echo "canvas:  $CANVAS_DIR"
echo "prefix:  $RUXEN_PREFIX"
echo "cc:      $($CC --version 2>/dev/null | head -1)"

# ---- 1. build + stage the toolchain from source ----
# install.sh --from-source builds the unified `ruxen` binary (stdlib embedded via
# include_str!, so the install is self-contained) and stages libruxenrt.a into
# $RUXEN_PREFIX/lib. We build into a writable copy of the source tree because the
# mounted /ruxen is read-only (the container mounts it ro for hermeticity).
say "1/5 build ruxen from source (cargo build --release; this is the long step)"
BUILD_SRC="$RUXEN_SRC"
if ! [ -w "$RUXEN_SRC" ]; then
  echo "ruxen source is read-only; copying to a writable tree for the build"
  BUILD_SRC="/tmp/ruxen-build"
  rm -rf "$BUILD_SRC"
  # Copy the source but NOT a possibly-huge target/ dir (we rebuild clean).
  mkdir -p "$BUILD_SRC"
  ( cd "$RUXEN_SRC" && tar --exclude=./target --exclude=./tmp -cf - . ) | ( cd "$BUILD_SRC" && tar -xf - )
fi
RUXEN_NO_MODIFY_PATH=1 bash "$BUILD_SRC/install.sh" \
    --from-source "$BUILD_SRC" --prefix "$RUXEN_PREFIX" --no-modify-path \
  || die "ruxen install.sh --from-source failed"
export PATH="$RUXEN_PREFIX/bin:$PATH"
command -v ruxen >/dev/null 2>&1 || die "ruxen not on PATH after install"
echo "installed: $(ruxen --version 2>&1 | head -1)"

# ---- 2. fetch the Linux Skia + HarfBuzz blobs (SHA-pinned) ----
# fetch_skia.sh is host-aware: under uname=Linux it selects the linux-<arch> RID
# and SHA-verifies the extracted .so against the pins. A pin mismatch is fatal here
# (we WANT the verification to fail loudly if a blob drifted).
say "2/5 fetch Skia + HarfBuzz (linux RID, SHA-pinned)"
bash "$CANVAS_DIR/runtime/fetch_skia.sh" || die "fetch_skia.sh failed (blob/SHA?)"
CACHE_DIR="${RUXEN_CANVAS_CACHE:-$HOME/.cache/ruxen-canvas}"
[ -f "$CACHE_DIR/libSkiaSharp.so" ] || die "libSkiaSharp.so not fetched into $CACHE_DIR"
echo "skia:     $CACHE_DIR/libSkiaSharp.so"
[ -f "$CACHE_DIR/libHarfBuzzSharp.so" ] \
  && echo "harfbuzz: $CACHE_DIR/libHarfBuzzSharp.so" \
  || echo "harfbuzz: (absent — shaping pins will assert the unavailable contract)"

# ---- 3. shim compiles warnings-clean on Linux ----
# The same gate scripts/check.sh stage 1 enforces, run here on the Linux compiler.
# The objc_msgSend cast idiom is excepted (it is inert dlopen'd code on Linux, but
# the cast still compiles); everything else must be clean. -Wno-cast-function-type
# is understood by both gcc and clang; the clang-only spelling
# -Wno-cast-function-type-mismatch is added ONLY when $CC accepts it (gcc emits a
# polluting "unrecognized option" note otherwise, which would trip the empty-log
# check below).
say "3/5 C shim compiles warnings-clean on Linux (-Wall -Wextra)"
warn_flags=(-Wall -Wextra -Wno-cast-function-type)
if "$CC" -Wno-cast-function-type-mismatch -xc -fsyntax-only /dev/null >/dev/null 2>&1; then
  warn_flags+=(-Wno-cast-function-type-mismatch)   # clang
fi
warn_log="$(mktemp)"
if "$CC" -O2 "${warn_flags[@]}" \
        -fsyntax-only "$CANVAS_DIR/runtime/skia_shim.c" "$CANVAS_DIR/runtime/sdl_window.c" \
        2>"$warn_log"; then
  if [ -s "$warn_log" ]; then
    echo "FAIL: shim emitted warnings on Linux:"; cat "$warn_log"; rm -f "$warn_log"
    die "shim not warnings-clean on Linux"
  fi
  echo "ok: shim warnings-clean on Linux"
else
  echo "shim does NOT compile on Linux:"; cat "$warn_log"; rm -f "$warn_log"
  die "shim compile failure on Linux"
fi
rm -f "$warn_log"

# ---- 4. the full canvas pin suite, headless on Linux ----
# `ruxen test` writes build artifacts under <project>/target, so it needs a
# WRITABLE project tree. The container mounts canvas read-only (hermeticity), so
# copy it to a writable tree there; under native CI the checkout is already
# writable and we use it in place. Either way the SOURCE is never mutated.
TEST_DIR="$CANVAS_DIR"
if ! ( touch "$CANVAS_DIR/.rx_write_probe" 2>/dev/null && rm -f "$CANVAS_DIR/.rx_write_probe" ); then
  echo "canvas is read-only; copying to a writable tree for the test build"
  TEST_DIR="/tmp/canvas-test"
  rm -rf "$TEST_DIR"
  mkdir -p "$TEST_DIR"
  ( cd "$CANVAS_DIR" && tar --exclude=./target --exclude=./tmp -cf - . ) | ( cd "$TEST_DIR" && tar -xf - )
fi
say "4/5 ruxen test (full pin suite, headless Linux)"
suite_rc=0
( cd "$TEST_DIR" && ruxen test ) || suite_rc=$?
if [ "$suite_rc" -ne 0 ]; then
  die "ruxen test reported failures on Linux (rc=$suite_rc)"
fi

# ---- 5. leak soak short-mode (the /proc/self/statm RSS arm on Linux) ----
say "5/5 leak soak (short mode; Linux RSS via /proc/self/statm)"
soak_bin="$(mktemp -d)/soak"
if "$CC" -O2 -o "$soak_bin" "$CANVAS_DIR/examples/soak_verify.c" \
        "$CANVAS_DIR/runtime/skia_shim.c" "$CANVAS_DIR/runtime/sdl_window.c" -ldl 2>/dev/null; then
  if SOAK_ITERS="${SOAK_ITERS:-2000}" "$soak_bin"; then
    echo "ok: no leak detected on Linux"
  else
    rm -rf "$(dirname "$soak_bin")"
    die "soak detected sustained RSS growth on Linux"
  fi
else
  rm -rf "$(dirname "$soak_bin")"
  die "could not build the soak harness on Linux"
fi
rm -rf "$(dirname "$soak_bin")"

say "VERDICT"
echo "PASS: Linux verification green (toolchain + blobs + shim + suite + soak)."
