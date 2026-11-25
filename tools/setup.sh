#!/usr/bin/env sh
# Fetch the fonts and pre-render the bitmap resources. Run once before
# `pebble build`, and again after changing tools/gen_assets.py.
set -eu
cd "$(dirname "$0")/.."
B=https://raw.githubusercontent.com/google/fonts/main/ofl
mkdir -p resources/fonts resources/images
curl -fsSLo resources/fonts/ChivoMono.ttf "$B/chivomono/ChivoMono%5Bwght%5D.ttf"
curl -fsSLo resources/fonts/Silkscreen-Regular.ttf "$B/silkscreen/Silkscreen-Regular.ttf"
curl -fsSLo resources/fonts/Silkscreen-Bold.ttf "$B/silkscreen/Silkscreen-Bold.ttf"
curl -fsSLo resources/fonts/OFL-ChivoMono.txt "$B/chivomono/OFL.txt"
curl -fsSLo resources/fonts/OFL-Silkscreen.txt "$B/silkscreen/OFL.txt"
if python3 -c "import PIL" 2>/dev/null; then
  python3 tools/gen_assets.py
else
  uv run --with pillow python3 tools/gen_assets.py
fi
