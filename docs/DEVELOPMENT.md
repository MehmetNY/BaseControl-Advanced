# Development

BaseControl Advanced is plain C++17 with no third-party libraries at build time: the interface is GDI+ and the Win32 API, the controller is portable C++. The Windows executable is cross-compiled on Linux with mingw-w64; the controller logic is tested natively on any host against a simulator.

## What you need

| | For |
|---|---|
| `g++` (C++17), `make`, Python 3 | host tests, the translation check, the document generator |
| `mingw-w64` (`x86_64-w64-mingw32-g++`, `windres`, `strip`) | the Windows executable |
| `pefile` (`pip install pefile`) | `tools/verify_pe.py` |
| `makensis` (NSIS 3, with the Unicode and Turkish language files) | the installer |
| `Pillow`, `rsvg-convert` / ImageMagick (optional) | `tools/make_art.py` (installer pictures), `tools/render_screenshots.sh` and `tools/make_banner.py` (README pictures) |
| Wine and Xvfb (optional) | the scripted UI runs and screenshots |

A Windows machine with MSYS2/MinGW can build the same way; nothing in the makefiles is Linux specific except the host sanitizers.

## Build and test

```sh
make test -j8                       # translation check, then all unit tests (AddressSanitizer + UBSan)
make -f Makefile.win -j8            # build/win/BaseControl.exe (debug symbols) and BaseControl.min.exe (stripped, the release binary)
python3 -I tools/verify_pe.py build/win/BaseControl.min.exe --version 1.0.0   # 28 checks on the file itself
python3 -I tools/gen_docs.py --check                                          # docs/SETTINGS.md and docs/REGISTERS.md are current
```

`make test` runs `tools/check_i18n.py --strict` first: it fails when an interface string has no Turkish translation or a translation has no string.

### Scripted UI runs

The same `Frame()` code that draws the real window can run offscreen against the simulator. `tools/run_ui_flows.sh` runs every script in `tests/ui` under Wine (start `Xvfb :99` first) and prints one line per script:

```sh
Xvfb :99 -screen 0 1920x1200x24 &
tools/run_ui_flows.sh                 # all of them, takes ten minutes or more
tools/run_ui_flows.sh battery display # only these
EXE=build/win/BaseControl.exe WINE= tools/run_ui_flows.sh   # natively on Windows (Git Bash)
```

A script is a text file; the first lines say how to run it (`# Run: ... --size 1360x1500`, optionally `--sim-model family` or `--sim-version 0.9.0`). The commands are `page`, `click`, `clickid`, `hoverid`, `dragby`, `wheel`, `key`, `type`, `wait`, `shot <file.png>`, `say`, and the checks `expect <key> <value>` (a snapshot value), `expectui`, `expectpref`, `expectint`, `expectdraft`. A line that starts with `#` is a comment. `shot` writes the frame so you can look at it.

### Screenshots

```sh
build/win/BaseControl.exe --sim --font Inter --shot custom out.png --size 1360x1700 [--advanced] [--drafts] [--lang tr]
```

`--advanced` opens the *Advanced CPU* panel and `--drafts` shows sliders with an unconfirmed draft. Every screenshot made this way is labelled SIMULATION.

### Release

`tools/make_release.sh` runs the host tests, builds the Windows executable, runs the generated-documentation and translation checks and `tools/verify_pe.py` on the exact file that is shipped, then writes into `dist/`: the installer (`...-Setup.exe`, built with NSIS from `installer/BaseControl.nsi`), the bare program (`...-portable.exe`), a zip with the program, licence, notices, `README.txt` and the documentation, and `SHA256SUMS.txt`. Nothing is signed or uploaded; the script prints the `gh release create` command for a pre-release. `SKIP_TESTS=1` skips `make test`; `VERSION=x.y.z` overrides the version. `installer/README.txt` is the plain-text read-me (English and Turkish) that the installer puts next to the program.

The pictures in the README come from `tools/render_screenshots.sh`: it renders the main pages from the simulator in English and Turkish into `docs/images/` (every picture shows SIMULATION in its top bar, because none of them was taken from a real laptop) and then calls `tools/make_banner.py` for the two banners. Run it after an interface change so the README matches the program.

## Code map

See [ARCHITECTURE](ARCHITECTURE.md) for what runs where. Quick orientation:

- `src/common/controller.cpp` + `ctl_*.cpp`: the controller. `Gate()` and `DoSet()` in `controller.cpp` are the order in which a request is judged.
- `src/common/settings_reg.cpp`: **every** setting, with range, evidence tier, capability and experimental flag.
- `src/common/registers.h` and `writeguard.cpp`: every EC address and the allow-list.
- `src/hw/*`: Windows implementations of the interfaces in `src/common/hw_iface.h`.
- `src/svc/*`: protocol, pipe server and client, the service, the three backends.
- `src/sim/sim.cpp`: the simulated machine; assumptions are written down in its header.
- `src/ui/*`: toolkit (`gfx`, `ui`, `page`), one `pages_*.cpp` per screen, the translation table `i18n_tr.inc`.

## Recipes

### Add a setting

1. Add one line to `kTable` in `src/common/settings_reg.cpp` (`INT`, `BOOL`, `ENUM` or `TEXT`): key, range, step, evidence tier, capability name, whether it needs *Allow experimental writes*. Pick the **lowest** evidence tier you can honestly defend; see the legend in [SETTINGS](SETTINGS.md).
2. Add the handler in the matching `ctl_*.cpp` and dispatch it from `DoSet()`. A handler must verify by reading back and undo what did not stick.
3. Have the probe publish the capability (`SetCap("name", "ok" | "absent" | ..., reason)`) so the control can say why it is disabled.
4. Add the sentence for the table to `DESCRIPTIONS` in `tools/gen_docs.py` and run `python3 -I tools/gen_docs.py`.
5. Write unit tests in `tests/test_controller.cpp` (success, out of range, gated, verify failure) and, if there is a control for it, a step in the matching `tests/ui/*_flow.txt`.

### Add an EC register

1. Name it in `src/common/registers.h`, with a comment saying where the knowledge comes from.
2. Add it to the allow-list in `src/common/writeguard.cpp` **with a bit mask and value rules**. Never add one from the *never written* list; a unit test checks that the list still wins.
3. Say in the pull request what was measured, on which model, and how.

### Add or change interface text

Write the English text as the key: `T("Apply")` for text shown directly, `N_("...")` where the string is only stored (it returns the text unchanged and marks it for the checker). Messages from the controller go through `TMsg()`, which also translates printf-style templates (`N_("CPU is at %d C; ...")`). Then add the pair `{"English", "Türkçe"},` to `src/ui/i18n_tr.inc` and run `python3 tools/check_i18n.py`. Style: buttons are a bare imperative or noun; *Cancel* is *İptal*, *Discard* is *Vazgeç*, *Confirm* is *Onayla*.

### Add a page

Add the page to the `Page` enum (`src/ui/app.h`), to the name table that scripts use (`src/ui/app.cpp`), to `kMeta`, the sidebar groups and the dispatch `switch` (`src/ui/app_chrome.cpp`). Write `pages_<name>.cpp` with a `Page<Name>()` function built from `Card`s in a `Masonry` (see any existing page), add a flow in `tests/ui/`, and look at it with `--shot` in English and Turkish at 1360 px wide.

## Style and rules of the road

- Warnings are errors in spirit: the Windows build uses `-Wall -Wextra -Wpedantic -Wshadow` and is warning-free.
- No new runtime dependency. No driver. No network code (the PE check fails if a networking library is imported).
- Anything that reaches hardware goes through `GuardedEc` or a typed handler; do not add a back door "for debugging".
- Honest labels: do not mark something *Verified* because it should work. Say *simulated* when only the simulator has seen it.
- Documentation tables that are generated stay generated.

## Credits and licence

GPL-3.0-or-later. Ideas and references from other projects are credited in [NOTICE](../NOTICE.txt); nothing is copied from a project whose licence does not allow it.
