#!/usr/bin/env bash
# Builds the release files into dist/:
#
#   Control-Center-Advanced-<version>-Setup.exe      installer (NSIS)
#   Control-Center-Advanced-<version>-portable.exe   the bare program (no installer)
#   Control-Center-Advanced-<version>-portable.zip   the program, licence, notices and the documentation
#   SHA256SUMS.txt                                checksums of the three files above
#
# Everything is checked on the way: the host tests, a warning-free Windows build, the generated documentation, and tools/verify_pe.py on the
# exact file that is shipped. Nothing is signed and nothing is uploaded; the last line prints the command that publishes a beta (pre-release).
#
#   tools/make_release.sh                 # version from Makefile.win
#   VERSION=1.0.1 tools/make_release.sh
#   SKIP_TESTS=1 tools/make_release.sh    # skip "make test" (the other checks still run)
#
# Needs: x86_64-w64-mingw32-g++ (mingw-w64), makensis (NSIS 3), python3, zip, sha256sum.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD

VERSION=${VERSION:-$(sed -n 's/^VERSION ?= *//p' Makefile.win | head -1)}
[ -n "$VERSION" ] || { echo "cannot read the version from Makefile.win" >&2; exit 1; }
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}
NUMVER=${VERSION%%-*}           # 1.0.0-beta -> 1.0.0 (the version resource of the installer wants a.b.c.d)
NAME="Control-Center-Advanced-$VERSION"

for tool in x86_64-w64-mingw32-g++ makensis python3 zip sha256sum; do
  command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done

step() { printf '\n== %s\n' "$*"; }

if [ "${SKIP_TESTS:-0}" != 1 ]; then
  step "host tests"
  make test -j"$JOBS"
fi

step "Windows build $VERSION"
make -f Makefile.win VERSION="$VERSION" -j"$JOBS"
EXE=$ROOT/build/win/ControlCenterAdvanced.min.exe

step "checking the binary that will be shipped"
python3 -I tools/verify_pe.py "$EXE" --version "$VERSION"

step "documentation tables and translations"
python3 -I tools/gen_docs.py --check
python3 tools/check_i18n.py --strict

step "staging"
rm -rf dist
mkdir -p dist/stage dist/zip/"$NAME"/docs
cp "$EXE" dist/stage/ControlCenterAdvanced.exe
# Windows text files get Windows line endings (the installer shows the licence in a text box). The read-me is UTF-8 with a byte-order mark
# so that every Notepad shows the Turkish half correctly.
for f in LICENSE:LICENSE.txt NOTICE.txt:NOTICE.txt; do
  sed 's/\r$//; s/$/\r/' "${f%%:*}" > dist/stage/"${f##*:}"
done
{ printf '\xEF\xBB\xBF'; sed 's/\r$//; s/$/\r/' installer/README.txt; } > dist/stage/README.txt

step "installer"
makensis -V2 -DVERSION="$VERSION" -DNUMVER="$NUMVER" -DSTAGE="$ROOT/dist/stage" -DOUTFILE="$ROOT/dist/$NAME-Setup.exe" installer/ControlCenterAdvanced.nsi

step "portable program"
cp "$EXE" "dist/$NAME-portable.exe"
cp dist/stage/ControlCenterAdvanced.exe dist/stage/LICENSE.txt dist/stage/NOTICE.txt dist/stage/README.txt dist/zip/"$NAME"/
cp docs/*.md dist/zip/"$NAME"/docs/
(cd dist/zip && zip -q -X -9 -r "../$NAME-portable.zip" "$NAME")

step "checksums"
(cd dist && sha256sum "$NAME-Setup.exe" "$NAME-portable.exe" "$NAME-portable.zip" > SHA256SUMS.txt)
rm -rf dist/stage dist/zip

echo
ls -l dist
echo
cat dist/SHA256SUMS.txt
cat <<EOF

Ready. To publish a beta (pre-release; unsigned, first hardware trial found problems):

  gh release create v$VERSION --prerelease --title "Control Center Advanced $VERSION" --notes-file <notes.md> \\
     dist/$NAME-Setup.exe dist/$NAME-portable.exe dist/$NAME-portable.zip dist/SHA256SUMS.txt
EOF
