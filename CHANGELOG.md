# Changelog

All notable changes are listed here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project uses [semantic versioning](https://semver.org/).

## [2.0.0] - 2026-11-15 - the redesign (pre-release)

A new design, written from the original Control Center's own program files, after the first beta's trial on the target laptop (keyboard backlight working only in its leftmost block, colours too pale). **It has not been tried on a real laptop yet** and is published as a pre-release, unsigned; what that means is in [Capabilities](docs/CAPABILITIES.md#what-has-never-been-run). The first beta stays available as [1.0.0-beta](https://github.com/MehmetNY/Control-Center-Advanced/releases/tag/v1.0.0-beta).

### Changed

- **One native program that replaces only the window of the OEM Control Center.** The privileged work - the embedded controller, the BIOS settings, the keyboard light, the profiles - is done by the vendor's own service, which the program asks the way the vendor's window does (an MQTT session on `127.0.0.1`). The embedded controller is only *read*, to show live values and to check that a change arrived. There is no service, named pipe, driver or helper of our own any more, no PawnIO, and no administrator prompt beyond the one at start. ([Architecture](docs/ARCHITECTURE.md), [Protocol](docs/PROTOCOL.md))
- **Closer to the original.** Pages, grouping and values follow the OEM Control Center 4.9.47.11: the four modes of the original (Office, Gaming, Turbo, Custom), the Turbo fan switch, the vendor's fan presets and curves, the Custom-mode values (PL1/PL2/PL4, CPU temperature offset, GPU clock offsets, GPU temperature target), application profiles, keyboard light, charge profile, colours. Everything else is as the original has it.
- **Performance**: the *Confirm* button is on every slider that changes the laptop; a slider follows the live value until you move it, so a change made by another program shows up at once.
- **The vendor's ready-made modes are never edited.** Their sliders show live values and stay locked; the page offers *Switch to Custom mode*. Profile values are sent only while the Custom mode is active, and not at all while the service has not said which mode that is.
- **Keyboard lighting** goes through the vendor's service: a colour for each light block (the whole keyboard is always sent, so one block never changes another), effect, direction, brightness, speed, the power switch and *Save to the keyboard*.
- A livelier interface: a bright ember-orange accent (five other accents to choose from), more saturated status colours, a new icon.
- *Start with Windows* is now a scheduled task with the highest rights of your account, offered only for the copy in `Program Files` (a no-prompt elevated start must not point at a folder a normal user can write to). The installer refuses folders outside `Program Files` for the same reason.
- The installer removes the first beta's service and keeps the settings folder.

### Added

- **Idle timer** for the keyboard light (never, 10 or 30 seconds, 1, 2, 5, 10, 30 minutes), as in the original.
- **Restore buttons**: *Vendor defaults* for the Custom mode, *Vendor's curves* for the fans, *Restore defaults* for the keyboard colours - each asks first.
- A **Service** page: the state of the link and *Reconnect*, the vendor's program and *Start the vendor's services*, *Read-only mode*, *Keep my settings applied*, *Apply on start*, confirmations (*Reset*), diagnostics (*Record all link messages*, *Detect hardware again*, *Copy diagnostics*), and what the evidence labels mean.
- `--autostart on|off` for scripts (exit status 0, 1 or 2).
- A **repository button** on the About page.
- 154 unit tests (about 125,000 checks) and 11 scripted interface runs, all against an in-memory copy of the vendor's service; 1,008 interface strings in English and Turkish.

### Kept from the beta

The GPU power target up to 150 W through the vendored [MONSTER GPU POWER](https://github.com/MehmetNY/Monster-XMG-Gpu-Power-Control) engine (now in *Performance → Custom mode*), with its identity check, per-step consent, thermal gate, driver confirmation and rollback; it is still the only thing this program writes to the embedded controller itself. Writing the OEM profile file next to it, which the beta always did, is now an option that is off by default (*Also write the OEM profile file*; a backup is made first). The overlay, key remapping, device manager, colour controls and the offline simulator (`--sim`) are kept too.

### Removed

- The Windows service and the named pipe, and with them the *Take over the vendor's service* switch.
- The PawnIO driver, access to the processor's model-specific registers and the *Advanced CPU* panel (package limits, undervolt, prefetchers ...).
- Direct writes of CPU power limits, fans and the charge profile to the embedded controller (they go through the vendor's service now), the fan watchdog and the fixed fan speed.
- Saved profiles of our own, automatic switching by power source, the keyboard light's idle and battery policies, and the Windows CPU policy sliders.
- The *Quiet* profile (the original has four modes, not five).

### Not included

FPS counter, graphics-mode (MUX/Optimus) switch, Dynamic Boost, custom charge thresholds, the factory colour-profile download, CPU package power, and any way to flash the BIOS or EC. The reasons are in [Capabilities](docs/CAPABILITIES.md#not-available-and-why).

### Known open points

All hardware paths are untested on the real laptop. The most important ones: which client name the Monster build's broker accepts, the exact types and units of some values (the idle timer is assumed to be in seconds), the keyboard effect numbers and zone order, and how the service behaves when a request is refused. They are listed in [Protocol](docs/PROTOCOL.md#open-points); the echo and read-back checks turn a wrong guess into a *failed* change instead of a wrong value on the hardware.

## [1.0.0-beta] - 2026-11-14 - first public beta

The project was developed under the working name *BaseControl Advanced* and renamed **Control Center Advanced** before this first release (program, installer, documentation and pictures).

**Known problems, reported after a first trial on the target laptop:** the keyboard backlight does not work properly (only the leftmost part of the keyboard reacted: the lighting protocol of this build was written without the real device and is wrong for it) and the interface colours are too pale. Nothing else has been reported from real hardware. Everything was otherwise checked with a cross-compiler, the Wine compatibility layer, the built-in simulator and unit tests; the code that talks to the laptop and to Windows (EC access, service, keyboard hook, display and radio switches, installer) has not been verified on Windows. See [Capabilities and verification](docs/CAPABILITIES.md) before you run it. The binaries are unsigned; Windows SmartScreen will warn.

### Added

- A native Windows control panel for the Monster Tulpar T5 V21.7 (GM5TG7Y) / XMG Neo 15 M21 (GM5TG7W) as a lighter alternative to the OEM Control Center 4.9.47.11: all 19 screens of the original have a counterpart ([table](docs/CAPABILITIES.md)).
- **Performance**: four profiles (Quiet, Balanced, Enthusiast, Overboost), custom profiles, application profiles, fan modes (auto, boost, custom curves with up to 10 points, fixed speed) and a fan watchdog.
- **Live values and a Confirm button on every slider** that changes a hardware or Windows setting. A slider edits a draft; nothing is sent until you press *Confirm*.
- **Advanced CPU panel** (ThrottleStop-style): package PL1/PL2 with time windows, platform limits, current limit, undervolt offsets, prefetchers and the values the CPU reports. Opens to the right of the page (below it on narrow windows). Needs the optional [PawnIO](https://pawnio.eu) driver.
- **GPU power target** up to 150 W through the vendored [MONSTER GPU POWER](https://github.com/MehmetNY/Monster-XMG-Gpu-Power-Control) engine, offered only on the exactly identified Tulpar T5 V21.7 and with per-step consent above the OEM's 140 W maximum.
- Windows CPU policy (boost mode, energy preference, minimum/maximum state, core parking) and GPU clock offsets through NVML.
- Keyboard lighting (colour, brightness, effects), battery charge profile and health, display colour and refresh rate, device manager switches, in-game overlay, key remapping, drive health.
- A **SYSTEM service** with a restricted named pipe so the window needs no administrator rights; an installer; a portable executable; `--local` mode for elevated use without the service.
- An **offline simulator** (`--sim`) that runs the whole interface on a pretend laptop, labelled SIMULATION.
- **Safety layers**: typed settings only, range checks, read-only switch, evidence tiers (*Verified*, *Family*, *Experimental*, *Os*), model gate, capability probes, consent prompts, thermal gate, an EC write allow-list with a never-write list, read-back with rollback. See [Safety](docs/SAFETY.md).
- English and Turkish interface (1,180 strings, checked on every test run).
- Documentation: [architecture](docs/ARCHITECTURE.md), [safety](docs/SAFETY.md), [capabilities](docs/CAPABILITIES.md), [setup](docs/SETUP.md), [design](docs/DESIGN.md), [development](docs/DEVELOPMENT.md), and generated tables of all [settings](docs/SETTINGS.md) and [registers](docs/REGISTERS.md).

### Not included

FPS counter, graphics-mode (MUX/Optimus) switch, Dynamic Boost, per-zone keyboard colours, custom charge thresholds, SATA SMART attributes, the factory colour-profile download, and any way to flash the BIOS or EC. The reasons are in [Capabilities](docs/CAPABILITIES.md#not-available-and-why).

### Fixed during development (found by review, each with a test where it can be tested)

- A confirmation dialog could be answered with the Enter key, so a stray key press could confirm a risky write. Dialogs are now answered by clicking; Enter does nothing, Esc cancels.
- The fan watchdog read temperatures that were up to 5 s old while the window was hidden. It now keeps its two temperature readings fresh once a second while a custom or fixed fan setting is active.
- Messages from the controller were not translated; they now go through a template table so they appear in the interface language.
- A message about the overlay's FPS line promised something this version does not have; it now says plainly that the FPS counter is not available.
- Launching from a command prompt with output redirected (`> file`) lost the output.

[2.0.0]: https://github.com/MehmetNY/Control-Center-Advanced/releases/tag/v2.0.0
[1.0.0-beta]: https://github.com/MehmetNY/Control-Center-Advanced/releases/tag/v1.0.0-beta
