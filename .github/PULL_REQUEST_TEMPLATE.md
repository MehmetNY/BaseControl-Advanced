## What and why

<!-- One idea per pull request. What does it change, and why? -->

## What I ran

- [ ] `make test -j8`
- [ ] `make -f Makefile.win -j8` (no warnings)
- [ ] `python3 -I tools/verify_pe.py build/win/ControlCenterAdvanced.min.exe --version 1.0.0-beta`
- [ ] `python3 -I tools/gen_docs.py --check`
- [ ] Interface change: `tools/run_ui_flows.sh`, and I looked at the page with `--shot` in English **and** Turkish

## Hardware

<!-- If this touches a register, a limit, a gate or an evidence tier: what was measured, on which model, with which tool?
     "Verified" means measured on a GM5TG7Y. Say "simulated" when only the simulator has seen it. -->

## Rules

- [ ] Every hardware write still goes through the guard; no new back door
- [ ] No network code, no driver, no new runtime dependency
- [ ] No code copied from a project whose licence does not allow it; nothing from the OEM Control Center
- [ ] New interface text is wrapped for translation and has a Turkish translation
- [ ] I agree that my contribution is licensed GPL-3.0-or-later
