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

# The remaining checks run from the repo root with relative paths — this is a
# Windows Python under Git Bash, so it cannot open the /c/... form that $PWD
# would give it.
cd "$HERE/../.."

# The web UI ships as a gzipped blob inside the firmware, so a syntax error in
# it is invisible until the page is opened on the device. Catch it here.
if command -v node >/dev/null 2>&1 && command -v python >/dev/null 2>&1; then
  echo "=== webui.html JavaScript ==="
  python -c "
import re, io, sys
s = io.open('webui.html', encoding='utf-8').read()
m = re.search(r'<script>(.*)</script>', s, re.S)
if not m:
    sys.exit('no <script> block found in webui.html')
io.open('webui_syntax_check.tmp.js', 'w', encoding='utf-8').write(m.group(1))
"
  if node --check webui_syntax_check.tmp.js; then echo "syntax OK"; else fail=1; fi
  rm -f webui_syntax_check.tmp.js
  echo
fi

# webui.h is generated; a stale one means web fixes never reach the device.
if command -v python >/dev/null 2>&1; then
  echo "=== webui.h freshness ==="
  python tools/make_webui_header.py --check || fail=1
  echo
fi

exit $fail
