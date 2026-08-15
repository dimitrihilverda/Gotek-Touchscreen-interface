#!/usr/bin/env bash
# Build and run the host-side firmware unit tests.
#
# These compile the real firmware headers against small Arduino/WiFi/SD stubs
# in stubs/, so the code under test is the code that ships — not a copy.
#
# Usage: tools/test/run.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

# scoop-installed gcc isn't always on PATH in non-interactive shells.
if ! command -v g++ >/dev/null 2>&1; then
  if [ -d "$HOME/scoop/apps/gcc/current/bin" ]; then
    export PATH="$HOME/scoop/apps/gcc/current/bin:$PATH"
  fi
fi
command -v g++ >/dev/null 2>&1 || { echo "g++ not found (try: scoop install gcc)"; exit 1; }

fail=0
for src in test_*.cpp; do
  bin="${src%.cpp}.exe"
  echo "=== ${src} ==="
  g++ -std=c++17 -O1 -I stubs -o "$bin" "$src"
  "./$bin" || fail=1
  echo
done

exit $fail
