# Changelog

All notable changes are listed here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project uses [semantic versioning](https://semver.org/).

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

[1.0.0-beta]: https://github.com/MehmetNY/Control-Center-Advanced/releases/tag/v1.0.0-beta
