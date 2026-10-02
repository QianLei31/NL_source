#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd -- "$(dirname -- "$0")" && pwd)"
REPO="${NL_SOURCE_REPO:-$(cd "$HERE/../../.." && pwd)}"
export NL_SOURCE_REPO="$REPO"
: "${NL_DEPS_PREFIX:?Set NL_DEPS_PREFIX to the prepared Qt6 dependency prefix}"
QTROOT="$NL_DEPS_PREFIX/include/x86_64-linux-gnu/qt6"
g++ -O2 -std=c++17 -fPIC -I "$REPO/src" -I "$QTROOT" -I "$QTROOT/QtCore" \
  "$HERE/detector_harness.cpp" "$REPO/src/signal/spike_filter.cpp" \
  -L "$NL_DEPS_PREFIX/lib/x86_64-linux-gnu" -lQt6Core -o "$HERE/detector_harness"
# Official public API records provenance and supplies the public download URL.
if [[ ! -f "$HERE/figshare_article.json" ]]; then
  curl -L --fail --max-time 30 -sS https://api.figshare.com/v2/articles/11897595 -o "$HERE/figshare_article.json"
fi
if [[ ! -f "$HERE/Simulator.zip" ]]; then
  URL="$(python -c 'import json,sys; print(json.load(open(sys.argv[1]))["files"][0]["download_url"])' "$HERE/figshare_article.json")"
  curl -L --fail --max-time 180 -sS "$URL" -o "$HERE/Simulator.zip"
fi
python "$HERE/run_validation.py" | tee "$HERE/run_validation.log"
python "$HERE/check_metrics.py"
