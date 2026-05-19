#!/usr/bin/env bash
# Fetch the latest released firmware binaries next to index.html for local
# testing. Run from any shell that has curl. Re-run after every new release.
#
# Required because the manifest now uses relative URLs (the only reliable
# way to avoid CORS errors when fetching from GitHub releases) — so the
# binaries have to live alongside the HTML.
set -euo pipefail

REPO="dimitrihilverda/Gotek-Touchscreen-interface"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TAG="${1:-latest}"

BASE="https://github.com/${REPO}/releases/${TAG}/download"
if [ "${TAG}" != "latest" ]; then
  BASE="https://github.com/${REPO}/releases/download/${TAG}"
fi

FILES=(
  "boot_app0.bin"
  "gotek-touchscreen-jc3248-bootloader.bin"
  "gotek-touchscreen-jc3248-partitions.bin"
  "gotek-touchscreen-jc3248.bin"
)

echo "Fetching firmware binaries from ${TAG} into ${HERE}"
for f in "${FILES[@]}"; do
  url="${BASE}/${f}"
  echo "  ${f}"
  curl -fsSL "${url}" -o "${HERE}/${f}" || { echo "FAILED to fetch ${url}"; exit 1; }
done
echo "Done. Now run:"
echo "  python -m http.server 8765"
echo "and open http://localhost:8765/"
