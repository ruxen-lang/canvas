#!/usr/bin/env bash
#
# check.sh — THE one-command local pre-commit gate for canvas (prod-hardening).
# Run this before committing. It exits NONZERO if anything that should pass fails.
#
#   scripts/check.sh
#
# Stages (in order; a failure in a gated stage aborts with nonzero):
#   1. shim compiles warnings-clean (-Wall -Wextra) — the C boundary must be clean.
#   2. ruxen test — the full pin suite (the correctness gate).
#   3. leak soak — short mode (SOAK_ITERS, default 2000 here; the full 10k is the
#      examples/soak_verify.c default). Gated: a detected leak fails the gate.
#   4. perf bench — REPORT-ONLY (never gated; shared-machine perf flakes — see
#      docs/PERF.md). Its numbers print for eyeballing, but a slow run does NOT
#      fail the gate.
#
# Tunables:
#   SOAK_ITERS=N   soak iteration count for stage 3 (default 2000)
#   SKIP_BENCH=1   skip the report-only bench (stage 4)
#   CHECK_CC=...   the C compiler (default cc)
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
CC="${CHECK_CC:-cc}"
SOAK_ITERS="${SOAK_ITERS:-2000}"
shim_srcs=(runtime/skia_shim.c runtime/sdl_window.c)
fail=0
step() { printf '\n=== %s ===\n' "$1"; }

# ---- 1. shim warnings-clean ----
# -Wall -Wextra EXCEPT the objc_msgSend function-pointer cast idiom: reaching the
# objc runtime by dlopen REQUIRES casting the single objc_msgSend symbol to each
# callee's exact ABI signature (the file-dialog / Metal / a11y bridges all do this;
# it is the documented, load-bearing pattern — clang's -Wcast-function-type-mismatch
# flags every such cast). We suppress ONLY that one warning class; everything else
# must be clean.
step "1/4 shim compiles warnings-clean (-Wall -Wextra, objc-msgSend casts excepted)"
warn_log="$(mktemp)"
warn_flags=(-Wall -Wextra -Wno-cast-function-type -Wno-cast-function-type-mismatch)
if "$CC" -O2 "${warn_flags[@]}" -fsyntax-only "${shim_srcs[@]}" 2>"$warn_log"; then
  if [ -s "$warn_log" ]; then
    echo "FAIL: shim emitted warnings:"; cat "$warn_log"; fail=1
  else
    echo "ok: shim is warnings-clean"
  fi
else
  echo "FAIL: shim does not compile:"; cat "$warn_log"; fail=1
fi
rm -f "$warn_log"

# ---- 2. full ruxen test suite ----
step "2/4 ruxen test (full pin suite)"
if ruxen test; then
  echo "ok: suite green"
else
  echo "FAIL: ruxen test reported failures"; fail=1
fi

# ---- 3. leak soak (short mode, gated) ----
step "3/4 leak soak (SOAK_ITERS=$SOAK_ITERS, gated)"
soak_bin="$(mktemp -d)/soak"
if "$CC" -O2 -o "$soak_bin" examples/soak_verify.c "${shim_srcs[@]}" -ldl 2>/dev/null; then
  if SOAK_ITERS="$SOAK_ITERS" "$soak_bin"; then
    echo "ok: no leak detected"
  else
    echo "FAIL: soak detected sustained RSS growth"; fail=1
  fi
else
  echo "FAIL: could not build soak harness"; fail=1
fi
rm -rf "$(dirname "$soak_bin")"

# ---- 4. perf bench (report-only, NEVER gated) ----
if [ "${SKIP_BENCH:-0}" = "1" ]; then
  step "4/4 perf bench — SKIPPED (SKIP_BENCH=1)"
else
  step "4/4 perf bench (report-only — see docs/PERF.md; NOT gated)"
  bench_bin="$(mktemp -d)/bench"
  if "$CC" -O2 -o "$bench_bin" examples/bench_frame.c "${shim_srcs[@]}" -ldl 2>/dev/null; then
    "$bench_bin" || echo "(bench run returned nonzero — ignored, report-only)"
  else
    echo "(could not build bench — report-only, ignored)"
  fi
  rm -rf "$(dirname "$bench_bin")"
fi

# ---- verdict ----
step "verdict"
if [ "$fail" -eq 0 ]; then
  echo "PASS: all gated stages green. Safe to commit."
  exit 0
fi
echo "FAIL: one or more gated stages failed. Do NOT commit until green."
exit 1
