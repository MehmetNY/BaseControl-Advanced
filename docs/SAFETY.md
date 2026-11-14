# Safety

Control Center Advanced can change things your laptop's firmware normally keeps to itself: CPU and GPU power limits, fan behaviour, charging profile. A wrong value can make the machine unstable, hot, or - in the worst case - damage it. This page says what the program does to keep that unlikely, what it does *not* protect you from, and how to get back to a clean state.

> **Read this first.** The program is licensed GPL-3.0 *without any warranty*. Everything in it was verified with a cross-compiler, the Wine compatibility layer, a simulator and unit tests. **No release has been tested on real hardware by anyone but the owner's own measurements that the register map is built on** (see [CAPABILITIES](CAPABILITIES.md)). Treat the first run like the first run of any overclocking tool: start with the defaults, change one thing at a time, watch the temperatures.

## What it will never do

- It never flashes or edits the BIOS, the embedded controller's firmware or any NVRAM variable.
- It never writes the registers on the *never-written* list ([REGISTERS](REGISTERS.md#never-written)), among them the EC power-limit bypass `0x0780`. The list wins over the allow-list, so even a future bug in an allow-list entry cannot reach them.
- It never picks a charger rating for you and never claims that the CPU and GPU share one power budget that it has "unlocked".
- It does not ship, install or modify a kernel driver. The EC is reached through the OEM Control Center's own `ACPIDriver` device; CPU registers go through [PawnIO](https://pawnio.eu) *if you install it yourself* ([SETUP](SETUP.md)).
- It does not connect to the internet (the executable imports no networking library, and the release check fails if one appears). The only web links are buttons on the *About* page and the setup hint of the advanced CPU panel: they hand an `https://` address (this repository, its issue form, the projects it credits) to your default browser when you click, and the program itself makes no request.

## The layers

Every change you make passes all of these, in this order. The first one that says no ends the request, and the window shows *why* in plain words.

| # | Layer | What it does |
|---|---|---|
| 1 | **Typed settings only** | A request names a setting from the [table](SETTINGS.md), never an address. The wire protocol has no field for one. |
| 2 | **Range check** | The value must be inside the setting's range and step. Out-of-range values are refused, not clamped. |
| 3 | **Read-only switch** | *Safety and service → Read-only mode* blocks every hardware write (Windows-only settings such as display brightness are not hardware writes). |
| 4 | **Evidence gate** | Each setting is *Verified*, *Family*, *Experimental* or *Os*. Experimental ones are locked until you turn on *Allow experimental writes*; *Family* ones are open only on the exactly identified Tulpar T5 GM5TG7Y and locked on any other model. |
| 5 | **Model gate** | On a laptop the program does not recognise (neither the exact SMBIOS match nor the same EC project id), every hardware write is refused until you turn on *Allow writes on an unrecognised model*. |
| 6 | **Capability probe** | Mappings that are only documented for the chassis family are checked at start-up (does the register answer, is the value plausible). When a probe fails the control stays disabled and says what failed. |
| 7 | **Consent** | The risky steps ask once; see the table below. |
| 8 | **Thermal gate** | Raising a CPU power limit is refused while the CPU is at 92 °C or hotter (both the EC path and the direct-register path). Lowering is always allowed. |
| 9 | **Write guard** | Every EC write - the program's own and the vendored GPU engine's - goes through an allow-list with bit masks and value rules. Details in [REGISTERS](REGISTERS.md#the-write-guard). |
| 10 | **Read-back and rollback** | After a write the value is read back. If the firmware did not keep it, the previous value is restored and the result says *reverted*. Groups (PL1/PL2/PL4) are written together, verified together and undone together. |

## What asks for your confirmation

When you confirm one of these once, the program remembers it so it does not ask again for the same kind of change. *Safety and service → Confirmations* shows what is allowed and has a *Reset confirmations* button.

Every confirmation dialog is answered by **clicking** its named button (*I understand, apply*, *Delete*, ...). Esc cancels; **Enter does nothing**, so a stray key press can never be the "yes". Likewise every slider that changes a hardware or Windows setting, and the drop-downs for refresh rate, cooling mode and boost mode, edit a *draft* that only their own *Confirm* button applies. (The position, margins and opacity of the in-game overlay are program preferences with no effect on the laptop; they apply at once so that you can see them.)

| Confirmation | Asked when |
|---|---|
| CPU power above the OEM maximum | PL1 or PL2 above 120 W, or PL4 above 200 W, and the new value is higher than what is set now (keeping or lowering an already-high value asks nothing). |
| Direct CPU register writes | The first time you write a power limit, time window, current limit, undervolt, prefetcher or similar value straight to the processor (the *Advanced CPU* panel of *Custom profiles*). |
| Fixed fan speed | Switching to a fixed fan duty. It also needs *Allow experimental writes*. |
| GPU power above the OEM maximum | A GPU power target above the OEM's 140 W maximum (145 W, 150 W). |
| Unverified GPU power steps | The 130 W and 135 W steps, which follow from the register layout but have not been observed on the live unit. |
| GPU power on an unlisted model | The SMBIOS board tag of the exact model is missing and you tell the program the laptop really is the supported one. |

## Hard limits

These are enforced in the controller *and* again in the write guard:

- CPU power through the EC: PL1 and PL2 from 10 to 200 W, PL4 from 20 to 250 W (the guard allows PL1/PL2 up to 210 W as a last line of defence), PL2 not below PL1, PL4 not below PL2. A PL4 value must be a multiple of 2 W when the firmware stores it doubled.
- CPU temperature limit: 70 to 100 °C.
- GPU power target: 125 to 150 W in 5 W steps.
- Fixed fan speed: 20 to 100 %.
- A processor register that the firmware has locked (`MSR 0x610` lock bit) is not written.

## The fan watchdog

A fixed fan duty switches off the EC's own thermal fan control, and a custom curve is only as good as the points you chose. While one of them is active, the service checks the temperatures every second - **also while the window is hidden in the tray** - and when the CPU is at 97 °C or more, or the GPU at 92 °C or more, in two consecutive checks, it releases the fixed duty and turns on fan boost. The *Fan watchdog* line on the Safety page shows when it has acted and why. Its reaction time is covered by a unit test for both a shown and a hidden window.

When the service is stopped on purpose (*Stop the service*, uninstalling, shutting Windows down) a fixed fan duty is handed back to the firmware on the way out. If the service is killed abruptly, the duty stays until the service manager restarts it a few seconds later (5, 15, then 60 s), which is why the restart policy is set; while no service is running, **nothing watches the temperatures**.

## The OEM service

The OEM Control Center's background service (GCUBridge) can write to the same registers and undo your changes. The Safety page shows whether it is installed and running, and offers *Take over from the OEM service*: it stops the service and sets it to Disabled so it cannot start again with Windows. This is reversible from the same page (*Give the OEM service back*), *Remove the service* does it automatically, and `ControlCenterAdvanced.exe --restore-oem` does it from a command prompt. While the OEM service is running, the program refuses to bring the EC's power-management loop up itself.

## The service and the window

- The window needs no administrator rights. The service (LocalSystem) does the privileged work.
- The service only ever runs a binary from a protected folder: installing from a portable copy first copies the program below `Program Files\Control Center Advanced`, so a normal user cannot swap the file the service runs.
- The pipe between them accepts local clients only, is protected against squatting and rate-limited, and has an *Only administrators can change settings* option for shared computers (to turn that off again you have to start Control Center Advanced as administrator).
- Everything that crosses the pipe is a typed request; a malformed or oversized one is dropped.

## What the program cannot protect you from

- **A bad value that every layer accepts.** The limits above are generous on purpose. 200 W for the CPU is inside them and your cooling may not handle it. The thermal gate and the watchdog react to temperature, not to the capacity of your VRM, battery or charger.
- **The firmware changing its mind.** Some values are reset by the firmware after sleep, a charger change or a display wake. *Keep my settings applied* puts back what you set, but there is a delay and the program cannot see everything the firmware does.
- **Another program writing to the same hardware.** The OEM Control Center and tools like ThrottleStop can overwrite values or fight with the program.
- **Wrong assumptions in the register map.** The map was assembled from the owner's measurements, the OEM program's behaviour and independent open-source work on the same EC family. On a different model it can be wrong. This is why unrecognised models are locked by default.
- **Bugs that no test has met.** There are 144 unit tests and a scripted run through every page; none of them is evidence about a real laptop.
- **Hardware failure.** Running hardware beyond the manufacturer's specification can shorten its life. That is your decision.

## Getting back to a clean state

1. *Safety and service → Read-only mode* stops all hardware writes immediately.
2. On the *Fan* page, *Auto* hands the fans back to the firmware. Applying a built-in profile on *Performance profiles* replaces the values you set by hand with the profile's.
3. *Remove the service* (or `ControlCenterAdvanced.exe --uninstall-service` as administrator) stops Control Center Advanced's service and gives the OEM service back.
4. A restart of the laptop is the last resort: values the firmware does not keep across a restart are gone, values Control Center Advanced re-applies at start-up are re-applied only while its service is installed and *Apply my settings when the service starts* is on.
5. The uninstaller removes the program and, when you agree, its saved settings. The OEM Control Center is untouched.

## Reporting a problem

If a write did something you did not expect, please open an issue with the *Diagnostics* card from the Safety page (it lists the number of writes, refused writes, the last error and the log tail). A change that touches the write guard, the never-written list or the evidence tiers needs a unit test and a note in the pull request that says what was measured and on which model.
