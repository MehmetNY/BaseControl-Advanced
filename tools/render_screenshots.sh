#!/usr/bin/env bash
# Renders the README pictures from the built-in simulator, then the banners:
#
#   docs/images/*.png       English screenshots (1360 px wide)
#   docs/images/tr/*.png    the same pages in Turkish
#   docs/images/banner.png, banner.tr.png
#
# Every picture shows SIMULATION in its top bar: nothing here was taken from a real laptop.
#
#   tools/render_screenshots.sh                                  # under Wine (needs a display, e.g. Xvfb on :99)
#   EXE=build/win/ControlCenterAdvanced.exe WINE= tools/render_screenshots.sh    # natively on Windows (Git Bash / MSYS)
#
# Needs Pillow and the Inter font (the same as tools/make_art.py). One run takes a few minutes.
set -u
cd "$(dirname "$0")/.."
EXE=${EXE:-build/win/ControlCenterAdvanced.exe}
WINE=${WINE-/usr/lib/wine/wine64}
FONT=${FONT:-Inter}
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-cca} WINEDEBUG=-all DISPLAY=${DISPLAY:-:99}

# name:page:size[:extra,extra]   (--drafts gives a page a lived-in state, --advanced opens the Advanced CPU panel)
SHOTS=(
  "hero-custom:custom:1360x900:--advanced,--drafts"
  "overview:overview:1360x860"
  "profiles:profiles:1360x860:--drafts"
  "fan:fan:1360x900"
  "display:display:1360x860"
  "battery:battery:1360x860"
  "safety:safety:1360x900"
  "deviceinfo:deviceinfo:1360x860"
)

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0

for lang in en tr; do
  out=docs/images
  [ "$lang" = tr ] && out=docs/images/tr
  mkdir -p "$out"
  for spec in "${SHOTS[@]}"; do
    IFS=: read -r name page size extra <<<"$spec"
    args=()
    [ -n "${extra:-}" ] && IFS=, read -ra args <<<"$extra"
    if timeout 300 $WINE "$EXE" --sim --font "$FONT" --lang "$lang" --shot "$page" "$out/$name.png" --size "$size" ${args[@]+"${args[@]}"} >/dev/null 2>&1 \
       && [ -s "$out/$name.png" ]; then
      echo "ok   $lang $name"
    else
      echo "FAIL $lang $name"; fail=$((fail + 1))
    fi
  done
  # Lighting: a script gives the keyboard a colour (the hex field is below the fold of a short window, so it runs tall and is cropped).
  cat > "$tmp/light.txt" <<EOF
page lighting
clickid in#light.hex
type #e2a387
clickid bt#light.hex
wait 800
shot $tmp/lighting-$lang.png
EOF
  if timeout 300 $WINE "$EXE" --sim --font "$FONT" --lang "$lang" --script "$tmp/light.txt" --size 1360x1500 2>&1 | grep -q 'script finished: 0 failure'; then
    echo "ok   $lang lighting"
  else
    echo "FAIL $lang lighting"; fail=$((fail + 1))
  fi
done

# Crop the tall lighting pictures, optimise every PNG, then draw the banners.
python3 - "$tmp" <<'EOF'
import glob, os, sys
from PIL import Image

tmp = sys.argv[1]
for lang, out in (("en", "docs/images"), ("tr", "docs/images/tr")):
    src = os.path.join(tmp, "lighting-%s.png" % lang)
    if os.path.exists(src):
        Image.open(src).crop((0, 0, 1360, 860)).save(os.path.join(out, "lighting.png"))
for path in sorted(glob.glob("docs/images/*.png") + glob.glob("docs/images/tr/*.png")):
    if os.path.basename(path).startswith("banner"):
        continue
    im = Image.open(path).convert("RGB")
    im.save(path, optimize=True)
EOF
python3 tools/make_banner.py || fail=$((fail + 1))

echo
ls -l docs/images docs/images/tr | grep -v '^total'
[ "$fail" -eq 0 ] || { echo "$fail picture(s) failed" >&2; exit 1; }
