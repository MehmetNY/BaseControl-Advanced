# Capabilities and verification

This page answers two questions as honestly as the author can: *what can Control Center Advanced do compared with the OEM Control Center*, and *how much of it has actually been checked*.

## How to read the status

| Status | Meaning |
|---|---|
| **Measured** | The behaviour was observed on a real Tulpar T5 GM5TG7Y by its owner (the GPU power engine of the MONSTER GPU POWER project, which is built in). |
| **Simulated** | The logic is covered by unit tests and scripted interface runs against a stand-in for the laptop and for the vendor's service. The code that talks to the real service, the real hardware or Windows is written from documentation and from reading the vendor's program, compiles, and has **not** been run on the laptop. |
| **Not available** | Not implemented, or deliberately left out. The reason is given. |

Nothing in this project has been through a signed build, a store review or a test lab. The release is an **unsigned pre-release**; Windows SmartScreen will warn about it.

## What this program is, in one paragraph

The vendor's Control Center is a Windows Store window that talks to two background services. **Control Center Advanced replaces only the window.** It is one native program that asks the vendor's own service to do what the vendor's window asks it to do (performance modes, the values of the Custom mode, fan curves, keyboard light, charge profile, a few switches), reads the embedded controller (EC) to check that the change arrived, and uses its own verified engine for one thing the vendor's program does not offer: the GPU power target above the 140 W maximum. There is no service, driver or helper of its own. See [ARCHITECTURE](ARCHITECTURE.md), the message list in [PROTOCOL](PROTOCOL.md) and the safety rules in [SAFETY](SAFETY.md).

## The original program's screens

The OEM Control Center (4.9.47.11) has 19 screens. Each one has a counterpart here; where an original screen was split or merged the table says so. Every value is the one the original shows, from the same source (the vendor's service, the EC or Windows).

| Original screen | Here | What it covers | Status |
|---|---|---|---|
| Genel Bakış (Overview) | **Overview** | Power status, system activity, active settings, quick mode switch, health of the link. | Simulated |
| Tercihler (Preferences) | **General settings → Preferences** | Windows-key lock, Num Lock lock, touchpad corner shortcut, Fn lock, USB power while off, power on with the adapter, language (automatic, English, Türkçe), accent colour, start with Windows, tray behaviour, shortcut notifications. | Simulated. Fn lock, USB power and power-on go through the service and are checked in the EC; the first three are done by hooks in the program and need it to be running |
| Aygıt Yöneticisi (Device manager) | **General settings → Device manager** | Wi-Fi, Bluetooth, camera and touchpad switches; built-in display brightness. | Simulated; the Windows calls are untested |
| GPU Ayarları (GPU settings) | **General settings → GPU settings** | Display path (read-only), refresh rate and the two refresh behaviours, live GPU status, the clock offsets that run now (read-only here; they are edited in Custom mode). | Simulated |
| Oyun İçi Gösterge (In-game OSD) | **General settings → In-game OSD** | Overlay with CPU, GPU, memory, fan and power lines, position, size, colour, opacity, list of games, hotkey. | Simulated. **The FPS line is not available** (see below) |
| Tuş Eşleme (Key mapping) | **General settings → Key remapping** | Assign 19 actions (microphone, volume, media, overlay, performance mode, fan boost, keyboard light, touchpad, Wi-Fi, camera, screenshot, calculator, lock, disable this key ...) to keys; saved mappings. The Fn key itself is handled by the firmware and cannot be remapped. | Simulated; the keyboard hook is untested on Windows |
| Performans Profilleri (Performance profiles) | **Performance → Performance modes** | The four modes of the original (Office, Gaming, Turbo, Custom; the vendor's own profile names are Balanced, Enthusiast, Overboost, Customized) and the Turbo fan switch; what the service reports for the active one. | Simulated |
| Fan Ayarları (Fan settings) | **Performance → Fan settings** | Cooling behaviour (automatic, Turbo fan, your own curve), curve editor for CPU and GPU, the vendor's presets, **the vendor's own curves** back, fan speed history. | Simulated; the table is checked in the EC |
| Özel Profiller (Custom profiles) | **Performance → Custom mode** | CPU PL1/PL2/PL4, CPU temperature offset, GPU clock offsets and temperature target, GPU power (cTGP), restore the vendor's defaults. Every slider has a *Confirm* button. | Simulated; GPU power **Measured** |
| Uygulama Profilleri (Application profiles) | **Performance → Application profiles** | Switch the performance mode when a program starts; ordered list of programs. | Simulated; the process scan is untested on Windows |
| Klavye Aydınlatması (Keyboard lighting) | **Lighting** | On/off, effect, direction, brightness, speed, colour of each light block (picture of the keyboard, picker, presets, hex), *Save to the keyboard*, restore defaults. | Simulated |
| Boşta Kalma Süresi (Idle timer) | **Lighting → Idle timer** | The keyboard light goes out after *Never*, 10 or 30 seconds, or 1, 2, 5, 10, 30 minutes without a key press. | Simulated; the service must confirm the value (see [PROTOCOL](PROTOCOL.md), open point 8) |
| Şarj Profili (Charge profile) | **Battery → Charge profile** | Full capacity, balanced, eco. | Simulated; the level is decided by the firmware |
| Pil Durumu (Battery status) | **Battery → Battery, Battery health, Charge history** | Level and state, time left, charging or discharge power, temperature, health, the charge history of the last 8 hours (kept only while the program runs). | Simulated |
| Renkler (Colours) | **Display settings → Image profile, Colour balance, Tone** | Colour presets, colour temperature, red/green/blue gain, saturation, gamma, brightness, contrast, with a picture of the result before anything is sent. | Simulated; the Windows colour-ramp calls are untested |
| Renk Kalibrasyonu (Colour calibration) | **Display settings → Colour calibration** | Explains factory colour profiles and opens Windows colour management, display settings and night light. **Control Center Advanced does not download the factory profile**: the original fetches it from the manufacturer's server using the serial number stored in the BIOS, which this program never sends anywhere. | By design, no download |
| Bileşenler (Components) | **Device info → Components and status** | Processor, graphics, memory modules, fans, storage drives, system. | Simulated; the SMBIOS and drive reads are untested on a laptop |
| Durum (Status) | **Device info → Components and status** | Live usage and temperatures. Merged with the screen above, as both show hardware with live readings. | Simulated |
| Hakkında (About) | **Device info → About** | Version, language, credits and licences, the repository and issue links. | Simulated |

One page exists only here: **Service** (the link to the vendor's service, the vendor's own program and its services, the write-protection switch, confirmations, diagnostics, what the labels mean, what the program never does).

## Beyond the original

Two things were asked for and are the only features the OEM program does not have; the rest of this table is what was added around them.

| Feature | Where | Notes |
|---|---|---|
| **Live values and a *Confirm* button on every slider** | Custom mode, Fan settings, Display settings | A slider only changes a draft; nothing reaches the hardware until *Confirm*. Otherwise the slider follows what the hardware reports, so a change made by another program shows up at once. |
| **GPU power target above the OEM maximum** | Custom mode → GPU power | The MONSTER GPU POWER engine, with its own identity check (manufacturer, model and BIOS string of the Tulpar T5 V21.7 must match; it is off on every other laptop) and per-step consent. 125, 140, 145 (idle) and 150 W (under load) were observed live by the owner; 130 and 135 W follow the register layout but were never tested and ask for confirmation, as does everything above the OEM's 140 W maximum. These are the only register writes this program makes itself. |
| A write-protection switch, confirmations and a read-back of every change | Service | *Read-only mode* stops everything that changes the machine; each risky step asks first; a change counts as done only when the service and, where it can be read, the EC show it. See [SAFETY](SAFETY.md). |
| An offline simulator | `ControlCenterAdvanced.exe --sim` | The whole interface on a pretend laptop and a pretend service, labelled SIMULATION. |

The interface is English and Turkish (1,008 strings, checked by `tools/check_i18n.py` on every test run); the original also ships German and French.

## Different from the original, on purpose

- **The ready-made modes are never edited.** The values of Office, Gaming and Turbo stay the vendor's: their sliders show the live values and are locked, and *Switch to Custom mode* is offered. Values are sent only while the Custom mode is active, and not at all when the service has not said which mode that is.
- **Colours and some switches are done by Windows, not by the service**: the colour controls use the display's gamma ramp, the Windows-key lock, the Num Lock lock and the touchpad corner shortcut are done by hooks inside the program. They work while Control Center Advanced runs (the tray is enough).
- **Application profiles** look at the list of running programs every few seconds instead of asking the service.
- **"Start with Windows"** creates a scheduled task with the highest rights of your account (the program needs them to read the EC), and only for the copy installed in Program Files.

## Left out of the earlier beta, on purpose

The first beta (v1.0.0-beta) carried a program of its own that took the vendor's place. This version does not, and the following are gone with it:

- a Windows service and a named pipe of its own, and the administrator prompts that came with them;
- the PawnIO driver, the processor's model-specific registers and the *Advanced CPU* panel (package limits, undervolt, prefetchers and so on);
- direct writes to the CPU power limits, the fans and the charge profile in the EC (they go through the vendor's service now, and the EC is only read);
- saved profiles of our own, automatic switching by power source, the keyboard light's idle and battery policies, the CPU policy sliders, the fan watchdog and fixed fan speed;
- *Take over the vendor's service*.

## Not available, and why

| Missing | Why |
|---|---|
| **FPS counter in the overlay** | Counting frames needs an event-tracing session, which this version does not have. The overlay shows a dash for FPS. |
| **Factory colour profile download** | Not implemented on purpose: it would send the BIOS serial number to the manufacturer's server. The Display page explains how to add a profile file in Windows. |
| **Graphics mode switch (MUX / Optimus)** | No verified switch was found for this model. The GPU page shows the mode Windows reports and says so. |
| **Dynamic Boost** | The register meaning is not verified on this model, so it is never sent. |
| **Lifting the shared CPU + GPU power budget** | A combined budget is enforced somewhere in the firmware (the owner measured one). Control Center Advanced has not found how to change it and does not claim to. The GPU page's *Limited by* line says what the hardware reports. |
| **CPU package power and other model-specific registers** | They need a driver that reads them; this version installs none. |
| **Light bar, logo light, hinge light** | This model has none. |
| **Custom charge thresholds** | The firmware decides the levels behind its three profiles. |
| **Flashing or editing the BIOS / EC firmware** | Never. See [SAFETY](SAFETY.md). |
| **Updating itself** | There is no updater and no network access beyond the loopback link to the vendor's service. New versions are on the [releases page](https://github.com/MehmetNY/Control-Center-Advanced/releases). |

## What was checked

| Layer | How | Result |
|---|---|---|
| Controller logic: gates, ranges, consent, thermal gate, rollback, the ready-made-mode rule, the fail-closed rule, application profiles, re-apply after events, lighting, idle timer, battery, display | 154 unit tests with about 125,000 checks against the stand-ins (`make test`) | all pass |
| The link to the service: JSON, MQTT framing, the allow-lists, refusals, reconnects, a service that is silent, slow or wrong | Unit tests of the codec and the session against an in-memory broker | pass |
| Interface: every page, Confirm/draft behaviour, dialogs (Enter never answers one), toasts, the Service page flows | 11 scripted flows with about 400 checks against the simulator, run in English at 1360 px wide under Wine (`tools/run_ui_flows.sh`) | all pass |
| Turkish interface and other window sizes | Looked at in screenshots; **not** covered by scripted checks | reviewed by eye only |
| Translations | `tools/check_i18n.py`: every interface string present and translated, none stale | 1,008 / 1,008 |
| Release binary | `tools/verify_pe.py`: 64-bit GUI executable, ASLR/DEP, administrator manifest, version resource, only Windows system DLLs, no driver loader, no private keys, loopback-only transport | all checks pass |
| Installer | Compiles with NSIS | compiles; **never run on Windows** |
| Documentation tables | `tools/gen_docs.py --check` regenerates [SETTINGS](SETTINGS.md) and [REGISTERS](REGISTERS.md) from the source | up to date |

## What has never been run

Everything below was written from documentation, from reading the vendor's program files and from reference projects, cross-compiled, and (where it has a screen) exercised in the simulator. **None of it has been executed on Windows or on a laptop by the author**:

- the link to the vendor's service (`src/oem/`) against the real broker of the Monster build: which client class it accepts, the real value types and units, and everything in the *Open points* of [PROTOCOL](PROTOCOL.md);
- the EC reader for the `ACPIDriver` device (`src/hw/acpi_ec.cpp`; the vendored one-shot wrapper from MONSTER GPU POWER, which the owner does use on the real unit, is the reference for its call);
- the elevated start-up task (`schtasks`), the installer and uninstaller, and the removal of the first beta's service;
- the keyboard hook, the touchpad corner tap, the overlay window and the tray icon;
- NVML (GPU readings, clock offsets), the display colour ramp, refresh-rate and brightness calls, the Wi-Fi/Bluetooth/camera/touchpad switches, the process scan;
- SMBIOS memory modules, CPU topology and drive health reads.

If you run it, you are the first. Please [open an issue](https://github.com/MehmetNY/Control-Center-Advanced/issues/new/choose) with *Copy diagnostics* from the Service page and say what worked.
