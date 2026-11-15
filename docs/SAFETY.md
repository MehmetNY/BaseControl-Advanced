# Safety

Control Center Advanced can change things your laptop's firmware normally keeps to itself: the CPU power limits of the Custom mode, GPU clock offsets, the extended GPU power (cTGP), fan tables, the charge profile. A wrong value can make the machine unstable, hot, or - in the worst case - damage it. This page says what the program does to keep that unlikely, what it does *not* protect you from, and how to get back to a clean state.

> **Read this first.** The program is licensed GPL-3.0 *without any warranty*. It was verified with a cross-compiler, the Wine compatibility layer, a simulator that stands in for the laptop and for the vendor's service, and unit tests. **This version has not been run on a real laptop**; the one hardware path that has been (the GPU power engine, see [CAPABILITIES](CAPABILITIES.md)) was tested by the owner. Treat the first run like the first run of any tuning tool: start with the defaults, change one thing at a time, watch the temperatures, and keep the vendor's own Control Center installed so you can undo anything there.

## What it will never do

- It never flashes or edits the BIOS, the embedded controller's firmware, the flash chip or any UEFI/NVRAM variable.
- It never writes the registers on the *never-written* list ([REGISTERS](REGISTERS.md#never-written)), among them the EC power-limit bypass `0x0780`. The list wins over the allow-list, so even a future mistake in the allow-list cannot reach them.
- It never selects a charger rating for you and never claims that the CPU and GPU share one power budget that it has "unlocked". It does not touch the registers that identify the charger.
- It does not install, ship or modify a driver or a service of its own. There is no kernel component, no PawnIO, no access to the processor's model-specific registers and no access to the keyboard controller. The privileged work is done by the vendor's own service and driver, which are already on a laptop that runs the vendor's Control Center ([ARCHITECTURE](ARCHITECTURE.md)).
- It never connects to another computer. Its only network connection is the loopback connection to the vendor's service on this computer (`127.0.0.1`, port 13688); the code refuses any other address, and the release check fails if that guard is missing. The links on the *About* page hand an `https://` address to your default browser when you click; the program itself makes no request.
- It never edits the vendor's ready-made modes (Office, Gaming, Turbo; see step 5 below), and several keys of the vendor's protocol are deliberately not on the message allow-list at all ([PROTOCOL](PROTOCOL.md)).

## How a change travels

Every change you make passes these steps in this order. The first one that says no ends the request, and the window says *why* in plain words.

| # | Step | What it does |
|---|---|---|
| 1 | **Draft, then Confirm** | A slider or drop-down that changes the laptop edits a *draft*; nothing is sent until you press its own *Confirm* button. Values that are only program preferences (overlay position, theme) apply at once because they do not touch the laptop. |
| 2 | **Typed setting** | A request names a setting from the [table](SETTINGS.md), never an address, a topic or a raw message. |
| 3 | **Range check** | The value must be inside the setting's range and step. For the power limits and clock offsets the range is the **minimum and maximum the vendor's service reports** for the active profile; PL1 ≤ PL2 ≤ PL4 is enforced. Out-of-range values are refused, not clamped. |
| 4 | **Read-only switch** | *Service → Write protection → Read-only* refuses every EC write and every message to the vendor's service (Windows-only settings such as display brightness are not hardware writes). |
| 5 | **Ready-made modes are never edited** | The profile values (PL1, PL2, PL4, CPU temperature offset, GPU clock offsets, GPU temperature target) are sent **only while the Custom mode is active**. While Office, Gaming or Turbo is active, or while the service has not said which mode is active, the sliders show the live values, stay locked, and nothing is sent. The page offers *Switch to Custom mode*. |
| 6 | **Message allow-list** | Only the keys on the allow-list in `src/oem/proto.cpp` can become a message. Several keys of the vendor's protocol are deliberately absent ([PROTOCOL](PROTOCOL.md)). |
| 7 | **The vendor's service applies it** | The message goes to the service as the vendor's own window would send it. |
| 8 | **Echo and read-back** | The program waits for the service to report the new value and then reads the embedded controller. A service that says "done" while the EC did not change is reported as **failed**, with the difference, not as success. |

The GPU power (cTGP) is the one setting that does not go through the vendor's service. It uses the verified engine that was built for this laptop first ([MONSTER GPU POWER](https://github.com/MehmetNY/Monster-XMG-Gpu-Power-Control)), with its own gates:

| Gate | Meaning |
|---|---|
| **Identity** | The exact model must be recognised (SMBIOS and EC project id). On anything else the control stays off and says why. |
| **Read before write** | The registers are read first; a value that is already set is reported as *already active* and not written. |
| **Allow-list** | Only `0x0743` (its low three bits, `0x01` or `0x05`) and `0x0744` (0 to 25 W in 5 W steps) can be written; everything else is refused by the write guard ([REGISTERS](REGISTERS.md#the-write-guard)). |
| **Consent** | Asked once, and remembered until you press *Reset confirmations*: more than the vendor's 140 W maximum (145 W, 150 W); the 130 W and 135 W steps, which follow from the register layout but were not observed on the live unit; an unlisted model. |
| **Thermal gate** | Raising above 140 W is refused while the GPU is at 80 °C or hotter, **or while its temperature cannot be read**. Lowering is always allowed. |
| **Confirmation by the driver** | After the write the graphics driver's power limit is read; a change that was not kept is undone, and the undo is itself checked. |
| **Cross-process lock** | When the standalone MONSTER GPU POWER program or its start-up task is applying a value at the same moment, this program waits for it (5 s) instead of writing at the same time. |

Every confirmation dialog is answered by **clicking** its named button. Esc cancels; **Enter does nothing**, so a stray key press can never be the "yes".

## Hard limits

- Power limits and clock offsets: the vendor service's own range for the profile, nothing wider.
- GPU power target: 125 to 150 W in 5 W steps; 330 W is never selected, whatever the charger label says.
- Fan tables: 16 points, the duty values and temperatures must rise and the table is checked before it is sent; the service must have said which table is active first.
- Keyboard colours must be `#rrggbb`; every light block is always sent, so one block never changes another.
- Only the addresses of the two cTGP registers can be written to the EC at all.

## The vendor's service and this program

- The vendor's service (`GCUService`) stays the owner of the stored profile values. This program asks it, as its own window does, and checks the result; it does not keep a copy that it would apply later (except the one setting *Keep my settings applied*, below).
- The vendor's service can change a value again later (after a display wake, a charger change, a profile switch). *Keep my settings applied* re-applies only the GPU power that you set; the other values are whatever the service holds. The page shows live values, so a change made by another program (the vendor's window, a script) appears at once.
- The program never writes the vendor's profile file unless you switch on *Also write the OEM profile file* (Custom mode, off by default). A backup of the original is made first, you are asked when the file holds a value this program did not write, and a write that fails is undone.
- If the vendor's service is not running, the pages that depend on it say so and are locked; *Service → Start the vendor's services* starts it when it is installed but stopped.

## What the program cannot protect you from

- **A value that every step accepts.** The ranges are the service's own. A power limit inside them can still be more than your cooling, your battery or your charger can sustain. The thermal gate of the GPU power reacts to temperature, not to the capacity of your charger.
- **The service changing its mind.** Some values are reset by the firmware or the service after sleep, a charger change or a display wake (see above).
- **Another program writing to the same hardware.** The vendor's Control Center, other tuning tools or scripts can overwrite values or fight with this program.
- **Wrong assumptions about the vendor's protocol.** The message layout was read from the vendor's files without running them; several details were not observed on a real laptop ([PROTOCOL → open points](PROTOCOL.md#open-points)). The echo and EC read-back checks exist to expose a wrong guess as a *failed* change, not as a wrong value on the hardware.
- **Bugs that no test has met.** There are 154 unit tests and a scripted run through every page against a simulated laptop and a simulated service; none of them is evidence about a real one.
- **Hardware failure.** Running hardware beyond the manufacturer's specification can shorten its life. That is your decision.

## Getting back to a clean state

1. *Service → Write protection → Read-only* stops all hardware writes and all messages to the vendor's service immediately.
2. On *Performance modes*, choose one of the vendor's ready-made modes: they use their own stored values. On *Custom mode*, *Vendor defaults → Restore defaults* asks the service to put its default values back into the Custom mode (it asks first; it is only offered while the Custom mode is active).
3. On *Fan settings*, set the cooling mode to *Auto* and switch *Turbo fan* off; *Vendor's curves* asks the service for its default fan table.
4. The GPU power returns to the vendor's default (125 W) from its slider on *Custom mode*, or from the vendor's own window.
5. A restart of the laptop is the last resort: values the firmware does not keep across a restart are gone, and this program re-applies only the GPU power, and only while *Keep my settings applied* / *Apply on start* is on.
6. The uninstaller removes the program, its start-up task and, when you agree, its saved settings. The vendor's Control Center is untouched.

## Reporting a problem

If a change did something you did not expect, please open an issue with *Copy diagnostics* from the *Service* page (it lists the link state, the number of writes and refused writes, the last error and the log tail; it contains no personal data). A change that touches the write guard, the never-written list, the message allow-list or the ready-made-mode rule needs a unit test and a note in the pull request that says what was measured and on which model.
