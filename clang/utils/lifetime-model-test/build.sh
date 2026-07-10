#!/usr/bin/env bash
#
# Build the ASCII Asteroids safe-model test application using the freshly
# built clang in this checkout.
#
# The whole application opts into the lifetime-safety "safe programming model"
# via per-file region markers from annotations.h:
#
#     LIFETIME_SAFE_START / LIFETIME_SAFE_END
#
# LIFETIME_SAFE_START expands to a pragma that (a) enables the lifetime-safety
# analysis and (b) upgrades every soundness check to an error -- but only for
# code *inside* the region. System headers are #included *outside* the region,
# so the STL produces no noise and no global -Wlifetime-safety flag is required.
# Unavoidable raw-pointer FFI is carved out with LIFETIME_UNSAFE_BEGIN /
# LIFETIME_UNSAFE_END. See annotations.h and NOTES.md for details.
#
# Usage:
#   ./build.sh            # build ./asteroids (safe model enforced, -O2)
#   ./build.sh debug      # -O0 -g, no optimization
#   VERBOSE=1 ./build.sh  # echo the compiler invocation

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# clang/utils/lifetime-model-test -> repo root is three levels up.
REPO_ROOT="$(cd "$HERE/../../.." && pwd)"

CLANG="${CLANG:-$REPO_ROOT/build/bin/clang}"
if [[ ! -x "$CLANG" ]]; then
  echo "error: clang not found at $CLANG" >&2
  echo "       build it first:  ninja -C build clang" >&2
  exit 1
fi

# macOS: locate the SDK so libc++ / system headers are found.
SYSROOT_FLAGS=()
if command -v xcrun >/dev/null 2>&1; then
  SDK="$(xcrun --show-sdk-path 2>/dev/null || true)"
  [[ -n "$SDK" ]] && SYSROOT_FLAGS=(-isysroot "$SDK")
fi

OPT=(-O2)
if [[ "${1:-}" == "debug" ]]; then
  OPT=(-O0 -g)
fi

SOURCES=(grid.cpp world.cpp render.cpp terminal.cpp main.cpp)

# -fno-exceptions: the safe model bans throw/try/catch; we never throw.
# -fno-rtti: not needed, keeps the binary lean.
# --driver-mode=g++: invoke clang as a C++ driver so the C++ runtime (libc++)
#   is linked -- plain `clang foo.cpp` compiles as C++ but does NOT link libc++.
FLAGS=(
  --driver-mode=g++
  -std=c++20
  "${OPT[@]}"
  -fno-exceptions
  -fno-rtti
  -Wall
  "${SYSROOT_FLAGS[@]}"
)

cd "$HERE"

# Only pass sources that actually exist yet (the app is built incrementally).
EXISTING=()
for s in "${SOURCES[@]}"; do
  [[ -f "$s" ]] && EXISTING+=("$s")
done

if [[ ${#EXISTING[@]} -eq 0 ]]; then
  echo "error: no source files present yet" >&2
  exit 1
fi

if [[ "${VERBOSE:-0}" == "1" ]]; then
  set -x
fi

"$CLANG" "${FLAGS[@]}" "${EXISTING[@]}" -o asteroids
set +x
echo "built: $HERE/asteroids"
