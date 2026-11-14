# Capabilities and verification

This page answers two questions as honestly as the author can: *what can Control Center Advanced do compared with the OEM Control Center*, and *how much of it has actually been checked*.

## How to read the status

| Status | Meaning |
|---|---|
| **Measured** | The behaviour was observed on a real Tulpar T5 GM5TG7Y by its owner (for example with the MONSTER GPU POWER project, whose engine is built in). |
| **Simulated** | The logic is covered by unit tests and scripted UI runs against the simulator. The code that talks to the real hardware or to Windows is written from documentation and compiles, but has not been run on a laptop. |
| **Not available** | Not implemented, or deliberately disabled. The reason is given. |

Nothing in this project has been through a signed build, a store review or a test lab. The release is an **unsigned beta**; Windows SmartScreen will warn about it.

## The original program's screens

The OEM Control Center (4.9.47.11) has 19 screens. Each one has a counterpart here; where an original screen was split or merged the table says so.

| Original screen | Here | What it covers | Status |
|---|---|---|---|
| Genel Bakış (Overview) | **Overview** | Power status, system activity, active settings, quick profile, health. | Simulated |
| Tercihler (Preferences) | **General settings → Preferences** | Windows-key lock, Num Lock lock, Fn lock, touchpad corner shortcut, USB power while off, power on with the adapter, language (automatic, English, Türkçe), accent colour, start with Windows, tray behaviour, shortcut notifications. | Simulated (the EC switches are *Family* tier) |
| Aygıt Yöneticisi (Device manager) | **General settings → Device manager** | Wi-Fi, Bluetooth, camera and touchpad switches; built-in display brightness. | Simulated; the Windows calls are untested |
| GPU Ayarları (GPU settings) | **General settings → GPU settings** | Display path (read-only), refresh rate, live GPU status, Dynamic Boost switch, link to the GPU power target and clock offsets. | The GPU power target is **Measured**; the rest is Simulated |
| Oyun İçi Gösterge (in-game OSD) | **General settings → In-game OSD** | Overlay with CPU, GPU, memory, fan and power lines, corner, size, colour, opacity, "games only" list, hotkey. | Simulated. **The FPS line is not available** (see below) |
| Tuş Eşleme (Key mapping) | **General settings → Key remapping** | Assign 19 actions (microphone, volume, media, overlay, profile, fan boost, keyboard light, touchpad, Wi-Fi, camera, screenshot, calculator, lock ...) to keys; saved mappings. | Simulated; the keyboard hook is untested on Windows |
| Performans Profilleri (Performance profiles) | **Performance → Performance profiles** | Quiet, Balanced, Enthusiast and Overboost: power, fans and the Windows CPU policy together; save the current settings as a profile. | Simulated |
| Fan Ayarları (Fan settings) | **Performance → Fan settings** | Auto, boost, custom curves (up to 10 points per fan), fixed speed, ramp speed, temperature and speed history, fan watchdog. | Simulated (*Family* and *Experimental* tiers) |
| Özel Profiller (Custom profiles) | **Performance → Custom profiles** | CPU PL1/PL2/PL4 and temperature limit, GPU power target and clock offsets, Windows CPU policy, and the *Advanced CPU* panel. Every slider has a *Confirm* button. | Simulated; GPU power target **Measured** |
| Uygulama Profilleri (Application profiles) | **Performance → Application profiles** | Switch profiles automatically when a program starts; ordered list. | Simulated; the process scan is untested on Windows |
| Klavye Aydınlatması (Keyboard lighting) | **Lighting → Keyboard light** | On/off, colour, brightness, effects (static, breathing, cycle, temperature, pulse), speed, live preview. | Simulated (*Family* tier). One light zone only |
| Boşta Kalma Süresi (Idle time) | **Lighting → Idle timeout, By power state** | Switch the light off after a delay, dim it on battery, switch it off with the display. | Simulated (Windows-side rule, *Os* tier) |
| Şarj Profili (Charge profile) | **Battery → Charge profile** | Full capacity, balanced, eco. | Simulated (*Family* tier); the levels are decided by the firmware |
| Pil Durumu (Battery status) | **Battery → Battery, Battery health, Charge history** | Charge level and state, time left, voltage, charging or discharge power, temperature, health (cycles, design and full-charge capacity), the charge history of the last 8 hours. | Simulated; health comes from the EC |
| Renkler (Colours) | **Display → Image profile, Colour balance, Tone** | Colour presets, saturation, colour temperature, gain per channel, brightness, contrast, gamma. | Simulated; the Windows colour-ramp calls are untested |
| Renk Kalibrasyonu (Colour calibration) | **Display → Colour calibration** | Explains factory colour profiles and opens Windows colour management, display settings and night light. **Control Center Advanced does not download the factory profile**: the original fetches it from the manufacturer's server using the serial number stored in the BIOS, which this program never sends anywhere. If you have the `.icc`/`.icm` file, add it in Windows colour management and it works together with the colours above. | By design, no download |
| Bileşenler (Components) | **Device info → Components and status** | Processor, graphics, memory modules, storage drives with health. | Simulated; the SMBIOS and drive reads are untested on a laptop |
| Durum (Status) | **Device info → Components and status** | Live usage and temperatures. Merged with the screen above, as both show hardware with live readings. | Simulated |
| Hakkında (About) | **Device info → About** | Version, licences, credits, language. | Simulated |

Two screens exist only here: **Safety and service** (write protection, confirmations, the OEM service, Control Center Advanced's own service, diagnostics) and the **Advanced CPU** panel of *Custom profiles*.

## Beyond the original

These are not in the OEM program's screens as far as the supplied screenshots and package show; they are why this project exists.

| Feature | Where | Notes |
|---|---|---|
| **Live values and a *Confirm* button on every slider** | Custom profiles, Fan, Display, GPU | A slider only changes a draft; nothing reaches the hardware until *Confirm*. The slider otherwise follows what the hardware reports, so another program's change shows up. |
| **ThrottleStop-style CPU registers** | Custom profiles → *Advanced CPU...* (right of or below the page) | Package PL1/PL2 with time windows and enable bits, clamp, platform (PSys) PL1/PL2, bidirectional PROCHOT, current limit, undervolt offsets, prefetchers, and the facts the CPU reports. Needs the optional PawnIO driver and is *Experimental* tier. See [SETUP](SETUP.md). |
| **GPU power target above the OEM maximum** | Custom profiles → GPU power | The MONSTER GPU POWER engine, with its own identity check (manufacturer, model and BIOS string of the Tulpar T5 V21.7 must match; it is off on every other laptop) and per-step consent. 125, 140, 145 (idle) and 150 W (under load) were observed live by the owner; 130 and 135 W follow the register layout but were never tested and ask for confirmation, as does everything above the OEM's 140 W maximum. |
| **Windows CPU policy** | Custom profiles, Performance profiles | Boost mode, energy/performance preference, minimum and maximum state, core parking. Plain Windows interfaces, no firmware. |
| **GPU clock offsets** | Custom profiles → GPU clocks | Through the NVIDIA driver (NVML). *Experimental* tier. |
| **Fan watchdog** | Safety and service | Forces fan boost at 97 °C (CPU) / 92 °C (GPU) while a fixed or custom fan setting is active. See [SAFETY](SAFETY.md). |
| **Evidence tiers** | Everywhere | Each setting is labelled *Verified*, *Family*, *Experimental* or *Os*; risky ones are locked until you allow them. |
| **Drive health** | Device info → Storage | Temperature, power-on hours, wear and written data for NVMe drives. Read-only; serial numbers are never read. |
| **An offline simulator** | `ControlCenterAdvanced.exe --sim` | The whole interface on a pretend laptop, labelled SIMULATION. |
| **Turkish and English interface** | Preferences / top bar | 1,180 interface strings, checked by `tools/check_i18n.py` on every test run. |

## Not available, and why

| Missing | Why |
|---|---|
| **FPS counter in the overlay** | Counting frames needs an event-tracing session in the service, which this version does not have. The overlay shows a dash for FPS. |
| **Factory colour profile download** | Not implemented on purpose: it would send the BIOS serial number to the manufacturer's server. The Display page explains how to add a profile file in Windows. |
| **Graphics mode switch (MUX / Optimus)** | No verified switch was found for this model. The GPU page shows the mode Windows reports and says so. |
| **Dynamic Boost** | The register meaning is not verified on this model. The control stays disabled. |
| **Lifting the shared CPU + GPU power budget** | A combined budget is enforced somewhere in the firmware (the owner measured one). Control Center Advanced has not found how to change it and does not claim to. The GPU page's *Limited by* line says what the hardware reports. |
| **Per-zone keyboard colours** | One light zone only; no per-zone protocol has been verified. |
| **Light bar, logo light** | This model has neither. The controls exist for models that do and appear only when the probe finds them. |
| **Custom charge thresholds** | The firmware decides the levels behind its three profiles. |
| **SATA SMART attributes** | Only NVMe health pages are read. Other drives get their inventory and temperature only. |
| **Flashing or editing the BIOS / EC firmware** | Never. See [SAFETY](SAFETY.md). |
| **Updating itself** | There is no updater and no network access. New versions are on the [releases page](https://github.com/MehmetNY/Control-Center-Advanced/releases). |

## What was checked

| Layer | How | Result |
|---|---|---|
| Controller logic: gates, ranges, consent, thermal gate, rollback, profiles, application profiles, re-apply after events, fan watchdog, lighting, battery, display, drive health | 144 unit tests with 123,866 checks against the simulator (`make test`) | all pass |
| Wire protocol and the pipe's request limits | Unit tests of the codec and of the validation rules | pass |
| Interface: every page, Confirm/draft behaviour, dialogs (including that Enter never answers one), toasts, the safety and service flows | 13 scripted flows with 325 checks against the simulator, run in English at 1360 px wide under Wine (`tools/run_ui_flows.sh`) | all pass |
| Turkish interface and other window sizes | Looked at in screenshots; **not** covered by scripted checks | reviewed by eye only |
| Translations | `tools/check_i18n.py`: every interface string present and translated, none stale | 1,180 / 1,180 |
| Release binary | `tools/verify_pe.py`: 64-bit GUI executable, ASLR/DEP, `asInvoker` manifest, version resource, only Windows system DLLs, no networking library, no driver loader, no overlay, no private keys | 28 checks pass |
| Installer | Compiles with NSIS | compiles; **never run on Windows** |
| Documentation tables | `tools/gen_docs.py --check` regenerates [SETTINGS](SETTINGS.md) and [REGISTERS](REGISTERS.md) from the source | up to date |

## What has never been run

Everything below was written from documentation and reference projects, cross-compiled, and (where it has a screen) exercised in the simulator. **None of it has been executed on Windows or on a laptop by the author**:

- the EC reader/writer for the `ACPIDriver` device (`src/hw/acpi_ec.cpp`; the vendored one-shot wrapper from MONSTER GPU POWER, which the owner does use on the real unit, is the reference for its ABI);
- installing, starting, stopping and removing the Windows service, the named pipe between the window and the service, and the UAC prompts that start them;
- the keyboard hook, the touchpad corner tap, the overlay window and the tray icon;
- PawnIO access to the CPU registers;
- NVML (GPU readings, clock offsets), the ITE keyboard-lighting HID reports, the display colour ramp, refresh-rate and brightness calls, Wi-Fi/Bluetooth/camera/touchpad switches;
- SMBIOS memory modules, CPU topology and drive health reads;
- the installer and uninstaller.

If you run it, you are the first. Please [open an issue](https://github.com/MehmetNY/Control-Center-Advanced/issues/new/choose) with the *Diagnostics* card from the Safety page and say what worked.
