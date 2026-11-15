# Contributing

Thank you for looking at Control Center Advanced. The project controls power limits, fans and firmware-adjacent settings of a real laptop, so a few rules matter more here than in an ordinary desktop app. They are short.

## The most useful thing you can do: run it and report

Almost everything here has only been exercised against a simulator ([what has and has not been run](docs/CAPABILITIES.md#what-has-never-been-run)). A report from a real laptop is worth more than a patch.

- **It worked / it did not work on my laptop:** open a [hardware report](https://github.com/MehmetNY/Control-Center-Advanced/issues/new?template=hardware_report.yml). Include the model, the BIOS and EC versions the *Device info* page shows, and the text of **Copy diagnostics** from the *Service* page.
- **A bug:** open a [bug report](https://github.com/MehmetNY/Control-Center-Advanced/issues/new?template=bug_report.yml). Steps, what you expected, what happened, the diagnostics text.
- **Something dangerous:** read [SECURITY](SECURITY.md) first.

Other laptops of the same Tongfang/Uniwill family may work, but the program deliberately locks writes on anything it does not recognise. Please do not ask for that lock to be removed; send a hardware report instead, so the register map can be checked against your model.

## Before you send a patch

1. Read [DEVELOPMENT](docs/DEVELOPMENT.md) (build, tests, recipes) and [ARCHITECTURE](docs/ARCHITECTURE.md).
2. Make the change small and focused. One idea per pull request.
3. Run the whole check before you push:

   ```sh
   make test -j8                    # i18n check + unit tests with ASan/UBSan
   make -f Makefile.win -j8         # the Windows build must stay warning-free
   python3 -I tools/verify_pe.py build/win/ControlCenterAdvanced.min.exe --version 2.0.0
   python3 -I tools/gen_docs.py --check
   ```

   If you touched the interface, also run the scripted UI flows (`tools/run_ui_flows.sh`, needs Wine or Windows) and look at your page with `--shot` in **English and Turkish**.

## Rules that are not negotiable

- **Hardware writes go through the guard.** Everything that reaches the EC passes the allow-list in `GuardedEc` (two registers, nothing else); everything sent to the vendor's service is built by `src/oem/proto.cpp` and passes its allow-lists, and is checked by the service's echo and an EC read-back. No back doors "for debugging".
- **A new register or setting needs a test and an honest tier.** Do not mark something *Verified* because it should work; *Verified* means measured on a GM5TG7Y by its owner. Say in the pull request what was measured, on which model, with what tool. A change to the write guard, the never-write list or the evidence tiers needs a unit test that fails without it.
- **No blind writes.** Do not add a write to an address because another project does it. Say where the evidence comes from.
- **Nothing is applied without the user's say-so.** Sliders edit drafts; *Confirm* applies. Dialogs are answered by clicking, never by a key press.
- **No network code beyond the loopback link, no driver, no new runtime dependency.** The release check fails if the executable imports an Internet client library or lacks the guard that refuses every address but `127.0.0.1`.
- **No copied code from projects whose licence does not allow it.** Ideas and facts are fine and are credited in [NOTICE](NOTICE.txt); source text is not, unless the licence is GPL-3.0-compatible and the origin is noted in the pull request. Nothing from the OEM's Control Center (code, driver, images, strings, server addresses, keys) belongs in this repository.
- **Interface text is English first and has a Turkish translation.** Wrap new strings in `T()` / `Tf()` (or `N_()` for tables) and add the translation to `src/ui/i18n_tr.inc`; `tools/check_i18n.py --strict` fails on a missing or stale one.
- **Generated documentation stays generated.** [SETTINGS](docs/SETTINGS.md) and [REGISTERS](docs/REGISTERS.md) come from the source; change the source and run `tools/gen_docs.py`.

## Style

C++17, in the style of the file you are in. Comments say *why*, not what. Keep the Windows build warning-free (`-Wall -Wextra -Wpedantic -Wshadow`).

## Commit messages and pull requests

A short imperative summary line, then a paragraph on what and why if it is not obvious. In the pull request, say what you ran (the checks above), what you looked at in the interface, and - for anything that touches hardware - what you measured. By contributing you agree that your work is licensed under the project's licence, GPL-3.0-or-later.

## Conduct

Be kind, be specific, assume good faith. Reports about safety problems are welcome and are never an inconvenience.
