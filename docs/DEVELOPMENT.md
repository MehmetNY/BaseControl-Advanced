# Development

Control Center Advanced is plain C++17 with no third-party libraries at build time: the interface is GDI+ and the Win32 API, the controller and the vendor-service link are portable C++. The Windows executable is cross-compiled on Linux with mingw-w64; the controller, the protocol code and the simulator also build natively, so the whole control stack is tested on any machine, with no laptop and no vendor software.

## What you need

| | For |
|---|---|
| `g++` (C++17), `make`, Python 3 | host tests, the translation check, the document generator |
| `mingw-w64` (`x86_64-w64-mingw32-g++`, `windres`, `strip`) | the Windows executable |
| `pefile` (`pip install pefile`) | `tools/verify_pe.py` |
| `makensis` (NSIS 3, with the Unicode and Turkish language files) | the installer |
| `Pillow`, `rsvg-convert` (optional) | `tools/make_icon.py` (the icon from `assets/icon.svg`), `tools/make_art.py` (installer pictures), `tools/render_screenshots.sh` and `tools/make_banner.py` (README pictures) |
| Wine and Xvfb (optional) | the scripted UI runs and the screenshots |

A Windows machine with MSYS2/MinGW can build the same way; nothing in the makefiles is Linux specific except the host sanitizers.

## Build and test

```sh
make test -j8                       # translation check, then all unit tests (AddressSanitizer + UBSan)
make -f Makefile.win -j8            # build/win/ControlCenterAdvanced.exe (with symbols) and ControlCenterAdvanced.min.exe (stripped, the release binary)
python3 -I tools/verify_pe.py build/win/ControlCenterAdvanced.min.exe --version 2.0.0   # 28 checks on the file itself
python3 -I tools/gen_docs.py --check                                                    # docs/SETTINGS.md and docs/REGISTERS.md are current
```

`make test` runs `tools/check_i18n.py --strict` first: it fails when an interface string has no Turkish translation or a translation has no string.

The host tests are `tests/*.cpp` on a tiny framework (`tests/mini_test.h`). `test_oem.cpp` runs the MQTT client, the JSON reader and the link against the in-memory vendor service of `src/sim/fake_oem.cpp`; `test_controller.cpp` drives the controller against the simulated machine (every setting: success, range, gate, service errors, read-back failure); `test_ui_models.cpp`, `test_autostart.cpp`, `test_msgtext.cpp` and `test_basic.cpp` cover the pure parts.

### Scripted UI runs

The same `Frame()` code that draws the real window can run offscreen against the simulator. `tools/run_ui_flows.sh` runs every script in `tests/ui` under Wine (start `Xvfb :99` first) and prints one line per script:

```sh
Xvfb :99 -screen 0 1920x1200x24 &
tools/run_ui_flows.sh                 # all of them, ten minutes or more
tools/run_ui_flows.sh battery display # only these
EXE=build/win/ControlCenterAdvanced.exe WINE= tools/run_ui_flows.sh   # natively on Windows (Git Bash)
```

A script is a text file; its header line says how to run it (`# Run: ... --size 1360x1500`, optionally `--sim-model family|unknown` or `--sim-version 0.9.0`). The commands are `page`, `click`, `clickid`, `hoverid`, `dragid`, `dragby`, `wheel`, `key`, `type`, `shot`, `wait`, `say`, `let`, the checks `expect`, `expectdraft`, `expectui`, `expectpref`, `expectmodal`, `expectint`, `expecthit`, and the ways of acting as "somebody else": `external` (another program changes an EC register, a keyboard value, the battery mode, the service's state ...), `cmd`, `notify`, `capture`, `fire`. The interpreter in `src/ui/app_shot.cpp` is the reference for their arguments, and the scripts in `tests/ui` are the examples.

Two things are worth knowing when writing a script: `clickid` on a widget that is below the window or on an option of a disabled combo box does nothing (use a taller `--size`, or check with `expecthit <id> 0`), and only one Wine run should be active at a time.

### Screenshots

```sh
build/win/ControlCenterAdvanced.exe --sim --font Inter --shot custom out.png --size 1360x1700 [--drafts] [--lang tr]
```

`--drafts` shows sliders with an unconfirmed draft. Every screenshot made this way is labelled SIMULATION in the top bar. The page names are the ones in `kPageNames` (`src/ui/app.cpp`): `overview prefs devices gpu osd keys profiles fan custom apps lighting battery display deviceinfo service about`.

### Release

`tools/make_release.sh` runs the host tests, builds the Windows executable, runs the generated-documentation and translation checks and `tools/verify_pe.py` on the exact file that is shipped, then writes into `dist/`: the installer, the bare program, a zip with the program, licence, notices and documentation, and `SHA256SUMS.txt`. Nothing is signed and nothing is uploaded.

The program is linked without a timestamp and stripped with `SOURCE_DATE_EPOCH=0`, so building the same source with the same toolchain gives the same bytes (checked for the 2.0.0 release by building it from two different directories and comparing SHA-256; it is the toolchain's output, not a promise about every compiler version).

The pictures in the README come from `tools/render_screenshots.sh`: it renders the main pages from the simulator in English and Turkish into `docs/images/` (every picture shows SIMULATION in its top bar, because none of them was taken from a real laptop) and draws the banners with `tools/make_banner.py`.

## Code map

See [ARCHITECTURE](ARCHITECTURE.md) for what runs where. Quick orientation:

| Path | What lives there |
|---|---|
| `src/main.cpp` | command line (`--help --version --probe --autostart --local --sim --tray --lang --shot --script`), one window per user session |
| `src/common/controller.cpp`, `ctl_*.cpp` | the controller: probes, polls (EC read-only, the vendor service, GPU, Windows), validates and executes requests. `Gate()` and `DoSet()` in `controller.cpp` are the order in which a request is judged; `ctl_oem.cpp` holds the link side (`OemSend`, `WaitOem`, `PollOem`) |
| `src/common/settings_reg.cpp` | **every** setting, with range, step, evidence tier and capability |
| `src/common/registers.h`, `writeguard.cpp` | every EC address, the allow-list of the two writable registers and the never-write list |
| `src/common/fan.cpp`, `keymap.cpp`, `osdcfg.cpp`, `colorramp.cpp`, `hostdata.cpp` | pure models the UI and the tests share (fan curves, key remapping, overlay layout, gamma ramp, drive health parsing) |
| `src/common/autostart_task.cpp` | the Task Scheduler XML and the rules for a start with Windows (pure part) |
| `src/oem/` | the link to the vendor's service: `json` (tolerant reader, strict writer), `mqtt` (MQTT 3.1.1 packets), `tcp` (loopback-only socket), `link` (session thread, last status of every topic), `proto` (topics, client identity, message builders and the allow-lists) |
| `src/hw/` | Windows implementations of the interfaces in `src/common/hw_iface.h`: the read-only EC driver handle (`acpi_ec`), the link settings from the registry (`oem_link_win`), the vendor's services (`oem_services`), autostart (`autostart_win`), power, display, devices, storage, NVML, host info; `win_world.cpp` builds them for the controller |
| `src/mgp/core`, `src/mgp/win` | the GPU power engine of [MONSTER GPU POWER](https://github.com/MehmetNY/Monster-XMG-Gpu-Power-Control) (identity guard, register transaction with rollback, NVML confirmation, OEM profile reader) with its Windows layer; the only code that writes the EC |
| `src/sim/` | the simulated machine (`sim.cpp`) and the stand-in for the vendor's service (`fake_oem.cpp`); the assumptions are written in their headers |
| `src/ui/` | toolkit (`gfx`, `ui`, `page`), the window (`app*.cpp`), one `pages_*.cpp` per screen, the overlay (`osd_*.cpp`), keyboard hook and touchpad corner (`session.cpp`), the translation table `i18n_tr.inc` |
| `tests/`, `tests/ui/` | host unit tests and the scripted UI flows |
| `tools/` | translation check, document generator, PE check, icon, installer art, screenshots, banners, release |
| `installer/`, `res/`, `assets/` | the NSIS script and its pictures, manifest/version resource/icon, the icon source |

There is no service and no pipe of our own: the process that draws the window is the process that holds the (read-only) driver handle and the link. Keep it that way; a second process would need its own protocol and its own failure modes for no gain.

## Recipes

### Add a setting

1. Add one line to `kTable` in `src/common/settings_reg.cpp` (`INT`, `BOOL`, `ENUM` or `TEXT`): key, range, step, evidence tier, capability name. Pick the **lowest** evidence tier you can honestly defend.
2. Add the handler in the matching `ctl_*.cpp` and dispatch it from `DoSet()`. A handler for something the vendor's service does must verify: wait for the service's echo (`WaitOem`) and, where the value is mirrored in the EC, read the EC back; it reports `failed.service` or `failed.verify` and never "success" on a guess.
3. Have the probe publish the capability (`SetCap("name", "ok" | "absent" | "pending" | "offline", reason)`) so the control can say why it is disabled.
4. Add the sentence for the table to `DESCRIPTIONS` in `tools/gen_docs.py` and run `python3 -I tools/gen_docs.py`.
5. Write unit tests in `tests/test_controller.cpp` (success, out of range, gated, verify failure) and, if there is a control for it, a step in the matching `tests/ui/*_flow.txt`.

### Teach the program a message of the vendor's service

1. Write the builder in `src/oem/proto.cpp` (declare it in `proto.h`). If the message carries keys or actions chosen by the caller, add the name to the allow-list next to it (`FanDetailAllowed`, `SettingActionAllowed` ...): a name that is not on the list is refused before anything is sent. Say in a comment where the structure was read.
2. Make the in-memory service answer it (`src/sim/fake_oem.cpp`): the same topic, the same status message afterwards, and an `ExternalKeyboard`/`External...` hook if a script should be able to change the state "from outside".
3. Test the builder and the round trip in `tests/test_oem.cpp`, the controller side in `tests/test_controller.cpp`.
4. Describe it in `docs/PROTOCOL.md`, including what is **not** known (units, polarity, what the service does when it fails).

### Add an EC register

Reading is easy, writing almost never happens.

1. Name it in `src/common/registers.h`, with a comment saying where the knowledge comes from. Reading goes through the read-only handle; a register nobody reads does not need to be listed.
2. Writing: do not. The two cTGP registers are the only ones on the allow-list in `src/common/writeguard.cpp` (with a bit mask and value rules) and the never-write list wins over everything; a unit test checks that. Anything else the firmware needs goes through the vendor's service.
3. Say in the pull request what was measured, on which model, and how.

### Add or change interface text

Write the English text as the key: `T("Apply")` for text shown directly, `N_("...")` where the string is only stored (it returns the text unchanged and marks it for the checker). Messages from the controller go through `TMsg()`, which also matches the ones built with `Fmt(N_("... %d ..."))`. Add the Turkish pair to `src/ui/i18n_tr.inc` and run `python3 tools/check_i18n.py` for the list of what is missing.

### Add a page

Add the page to the `Page` enum (`src/ui/app.h`), to the name table that scripts use (`src/ui/app.cpp`), to `kMeta`, the sidebar groups and the dispatch `switch` (`src/ui/app_chrome.cpp`). Write `pages_<name>.cpp` with a `Page<Name>(App&, ...)` function built from the rows in `src/ui/page.h`; for a value that can be changed from outside, use the "live value + draft + Confirm" row, never a slider that sends while it moves. Add a flow script in `tests/ui/` and, if the page is worth showing, an entry in `tools/render_screenshots.sh`.

## Style and rules of the road

- Warnings are errors in spirit: the Windows build uses `-Wall -Wextra -Wpedantic -Wshadow` and is warning-free.
- No new runtime dependency, no driver, no kernel access of our own. The only network code is the loopback socket of the vendor link, which refuses every other address (`tools/verify_pe.py` looks for the guard and fails when an Internet client library is imported).
- Anything that reaches hardware goes through `GuardedEc` or a typed handler that the settings registry knows; do not add a back door "for debugging".
- Anything that goes to the vendor's service goes through the builders in `src/oem/proto.cpp`; never assemble JSON for it elsewhere.
- Honest labels: do not mark something *Verified* because it should work. Say *simulated* when only the simulator has seen it, and keep the list of open points in [PROTOCOL](PROTOCOL.md) and [CAPABILITIES](CAPABILITIES.md) true.
- Documentation tables that are generated stay generated.

## Credits and licence

GPL-3.0-or-later. Ideas and references from other projects are credited in [NOTICE](../NOTICE.txt); nothing is copied from a project whose licence does not allow it, and the vendor's own program files are read for understanding and are neither included nor redistributed.
