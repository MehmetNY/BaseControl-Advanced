#!/usr/bin/env bash
# Runs every UI script in tests/ui against the simulator, offscreen, and prints one line per script.
# Each script names its window size in its header line ("# Run: BaseControl.exe --sim --script <file> --size WxH") and, when it needs
# another simulated laptop ("--sim-model family|unknown") or service version ("--sim-version 0.9.0"), it names that there too.
#
#   tools/run_ui_flows.sh                    # all scripts, under Wine (needs a display, e.g. Xvfb on :99)
#   tools/run_ui_flows.sh battery display    # only these
#   EXE=build/win/BaseControl.exe WINE= tools/run_ui_flows.sh    # natively on Windows (Git Bash / MSYS)
#
# Screenshots of the steps land in $SHOTS (default /tmp/shots). Exit status is the number of scripts that failed.
set -u
cd "$(dirname "$0")/.."
EXE=${EXE:-build/win/BaseControl.exe}
WINE=${WINE-/usr/lib/wine/wine64}
SHOTS=${SHOTS:-/tmp/shots}
FONT=${FONT:-Inter}
mkdir -p "$SHOTS"
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-basecontrol} WINEDEBUG=-all DISPLAY=${DISPLAY:-:99}

if [ "$#" -gt 0 ]; then files=(); for n in "$@"; do files+=("tests/ui/${n}_flow.txt"); done; else files=(tests/ui/*_flow.txt); fi

failed=0
for f in "${files[@]}"; do
  name=$(basename "$f" _flow.txt)
  size=$(sed -n 's/^# Run:.*--size \([0-9]*x[0-9]*\).*/\1/p' "$f" | head -1)
  size=${size:-1360x1100}
  model=$(sed -n 's/^# Run:.*--sim-model \([a-z]*\).*/\1/p' "$f" | head -1)
  simver=$(sed -n 's/^# Run:.*--sim-version \([0-9.]*\).*/\1/p' "$f" | head -1)
  out=$(timeout 600 $WINE "$EXE" --sim ${model:+--sim-model "$model"} ${simver:+--sim-version "$simver"} --font "$FONT" --script "$f" --size "$size" 2>&1)
  fails=$(printf '%s\n' "$out" | grep -c '^FAIL')
  last=$(printf '%s\n' "$out" | grep 'script finished' | tail -1)
  checks=$(printf '%s\n' "$out" | grep -c '^ok')
  if [ "$fails" -eq 0 ] && printf '%s' "$last" | grep -q ' 0 failure'; then
    printf '%-10s %4d checks ok   (%s)\n' "$name" "$checks" "$size"
  else
    failed=$((failed + 1))
    printf '%-10s FAILED  %s\n' "$name" "${last:-no result line}"
    printf '%s\n' "$out" | grep '^FAIL' | head -10 | sed 's/^/    /'
  fi
done
exit "$failed"
