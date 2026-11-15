# The link to the vendor's Control Center service

Control Center Advanced changes profile-level settings (power limits of the Custom mode, GPU clock offsets, the performance mode, fans, the keyboard light, the charge profile and a few switches) by sending the same kind of messages as the vendor's own window. This page describes those messages, what the program sends and what it never sends, and which parts have **not** been confirmed on a real laptop.

The code is in `src/oem/` (JSON, MQTT, TCP transport, the session in `link.cpp`, the message builders and allow-lists in `proto.cpp`) and `src/common/ctl_oem.cpp` (what the controller does with them). The simulator's fake service (`src/sim/fake_oem.cpp`) implements the same messages, so the tests and the screenshots exercise the real link code.

## Where this comes from

The structure was read **statically** from the vendor's program files on the owner's machine (the original Control Center 4.9.47.11 package for the Monster Tulpar T5): the topic names, the JSON keys of the status messages, the names of the actions. No vendor code was run for this, no protection was circumvented, and no vendor code, driver or file is copied into this program or shipped with it. The Monster build of the service keeps its method bodies encrypted; the message layout was therefore taken from the parts of the same program family that are readable (the user-interface side and a build of the service whose bodies are not protected). That is the main reason for the list of open points at the end of this page.

## Transport and identity

- MQTT 3.1.1 over TCP to **`127.0.0.1:13688`**. The broker is the vendor's `GCUBridge` service; the hardware service `GCUService` is another client of the same broker. QoS 0 everywhere.
- The broker only admits a client whose client id, user name and password follow the vendor client's pattern: a *client class* (a name prefix) plus a *slot* number from 0 to 19. The vendor's windows use certain slots; a second connection with the same client id drops the first one. This program uses **slot 18** so it never takes a slot from the vendor's own windows, and it asks for a clean session with a keep-alive of 30 s.
- The class the Monster build accepts for interface programs is chosen by two values under `HKLM\SOFTWARE\OEM\GamingCenter2` (`BridgeType`, `CustomizeTarget`). The program reads them, tries the selected class first and the other known classes after it when the broker refuses, and remembers which one worked (*Service → Link to the vendor's service* shows the class and slot).
- The password pattern is a fixed string of the vendor's client. It only identifies "a program of the same family on this computer" to a broker that listens on the loopback interface; it is not a secret of the user. It is **not repeated in the documentation** and lives in one place in the source, `src/oem/proto.cpp`.
- The transport refuses every host except `127.0.0.1` / `localhost` (`src/oem/tcp.cpp`); `tools/verify_pe.py` checks that this guard is in the release binary.
- Reconnecting uses a back-off between 1 and 15 s. After a reconnect, and once a minute while connected, the program asks every topic it uses for its status again.

## Topics

Control topics end in `/Control` or `/Ctrl` (we publish, the service listens); status topics end in `/Status`, `/Info` or `/Table` (the service publishes, we subscribe). Every payload is a JSON object.

| We publish | We subscribe to | What it is |
|---|---|---|
| `Fan/Control` | `Fan/Status`, `Fan/Table` | Performance mode, Turbo Fan, the values of a mode, fan curves |
| `Keyboard/Ctrl` | `Keyboard/Status` | Keyboard backlight |
| `Setting/Control` | `Setting/Status` | Switches (Fn lock, automatic boot, powered USB, ...) |
| `BatteryProtection/Control` | `System/BatteryProtection` | Charge profile |
| `Customize/Control` | `Customize/Info`, `Customize/SupportInfo` | What this model supports (keyboard type, battery protection type, ...) |

The *Record all link messages* switch on the Service page (off by default) additionally subscribes to everything on the broker, so a bug report can show what the vendor's own window sends. Nothing is sent anywhere by it.

## What we send

All keys are checked against an allow-list in `proto.cpp` before a message is built; a key that is not on it cannot be sent, and the unit tests cover the refusals.

### `Fan/Control`

JSON object with an `Action`:

| Action | Meaning |
|---|---|
| `GETSTATUS`, `GET_FAN_SPEED_CURVE_SETTING` | Ask for `Fan/Status` / `Fan/Table` |
| `OPERATING_OFFICE_MODE`, `OPERATING_GAMING_MODE`, `OPERATING_TURBO_MODE`, `OPERATING_CUSTOM_MODE` | Switch the performance mode (the vendor's names: Balanced, Enthusiast, Overboost, Customized). An optional `ProfileIndex` selects one of the mode's saved profiles. |
| `FAN_BOOST_ON`, `FAN_BOOST_OFF` | Turbo Fan |
| `SET_OPERATING_MODE_DETAIL` | Change values of the profile that is active. Only these numeric keys are ever sent: `PL1`, `PL2`, `PL4`, `CpuTccOffsetSwitch`, `CpuTccOffset`, `GpuCoreClockOffset`, `GpuCoreClockOffsetOC`, `GpuMemoryClockOffset`, `GpuMemoryClockOffsetOC`, `GpuTargetTemperature`. **Only while the Custom mode is active**; the controller refuses it in the other three modes. |
| `RESTORE_OPERATING_MODE_DETAIL` | Ask the service for its default values of the active profile |
| `SET_FAN_SPEED_CURVE_SETTING` | Fan curves: `Name` and `Type` as the service reported them, and `CPU` / `GPU` objects with `Duty`, `UpT`, `DownT` each holding `T0`..`T15` |
| `SET_FAN_CONTROL_RESPECTIVE` | Use the table above instead of the profile's own (`Name`, `FanControlRespective`) |
| `RESTORE_FAN_SPEED_CURVE_SETTING` | Default fan curves |

Deliberately **never** sent: `OverClockingSwitch`, `GpuConfigurableTGP*`, `GpuDynamicBoost*`, `CustomTGPinGCUforGN21_*` (they move the GPU power state that the verified GPU power engine owns, so two writers would undo each other), `FanSwitchSpeed*` (the unit of the value is not known), `CpuOffsetCoreVoltage*` and the memory-overclocking and performance-menu switches (the service keeps those in the firmware's variable store), and every AMD-only key.

### `Keyboard/Ctrl`

The service reads **both** `function` and `Action` from every message, so every message carries both. A status request is `{"function":"None","Action":"GETSTATUS"}`. The commands:

- `SetPower` with `powerstatus` (0/1).
- `SetEffectALL` with `mode` (1 = lighting), `effect`, `light` (0..4), `speed` (0..4), `direction`, `nv_save` (`true` also keeps the setting in the keyboard's own memory: *Save to the keyboard*), `backgroundcolor`, `alphabet` and `color`: `{"isCircular":false,"ColorBlocks":n,"ColorBuffer":[{"ID":0,"R":..,"G":..,"B":..}, ...]}`. One entry per light block; the program always sends every block, so setting one block never changes another.

Effects are the vendor's numbers (`0` static, `1` breathing, `2` wave, `3` reactive, `4` rainbow, `5` ripple, ... `13` music, `17` mix); directions are `-1` none, `0` left to right, `1` right to left, `2` bottom to top, `3` top to bottom, `4` from the pressed key, `5` synchronised. The Lighting page offers the effects the vendor's window offers for this kind of keyboard and shows any other effect that is active.

### `Setting/Control`

`{"Action": "<NAME>"}` with the names from the allow-list: the switches `FNKEY_LOCK/UNLOCK`, `ACRECOVERY_TOGGLE_ON/OFF` (power on when the charger is plugged), `USB_CHARGER_ON/OFF`, `WINKEY_LOCK/UNLOCK`, `NUMPAD_LOCK/UNLOCK`, `TOUCHPAD_*`, `OSD_HIDDEN_*` and so on, `SetKeyboardCloseTime` (`Mins`, `Seconds`: the keyboard light's *Idle timer*), `SETSCREENBRIGHTNESS`, and `GETSTATUS`. The polarity of a switch (does `_ON` mean "feature on" or "locked"?) is not the same for every switch, so the program does not trust the name: it sends the action, checks the embedded controller for the result, tries the other name if nothing changed, and remembers which one worked. A repeat is never sent blindly, because with the polarity unknown it could flip the switch back.

The idle timer is different: it is not an EC switch, so nothing but the service can say whether it took. The program sends the timer as minutes and seconds (the Lighting page offers *Never*, 10 and 30 seconds, and 1, 2, 5, 10 and 30 minutes) and counts it as applied only when `Setting/Status` reports `CloseTimer` equal to the total number of seconds; otherwise the change is shown as *not confirmed by the service*.

### `BatteryProtection/Control`

`PERFORMANCEDMODE` (full capacity), `BALANCEDMODE`, `HEALTHYMODE` (eco), and `Report`.

### `Customize/Control`

`GETSUPPORT` and `GETSETUPINFO`: what the model supports.

## What we read

- `Fan/Status` is one large object (about 86 fields): `OperatingMode`, the index of the active profile of each mode, `FanBoostEnable`, `ProfileName`, `CPU_PL1` / `CPU_PL1Maximum` / `CPU_PL1Minimum` (and PL2, PL4), `CPU_TccOffsetSwitch`, `CPU_TccOffset`, `GPU_CoreClockOffset` (+`OC`, limits), `GPU_MemoryClockOffset`, `GPU_TargetTemperature`, `IsNvGpu`, `OcSupport`, `FAN_TableName`. The sliders' ranges are the minimum/maximum values the service reports for the active mode.
- `Fan/Table`: the active fan table (`Name`, `Type`, `CPU` and `GPU` curves).
- `Keyboard/Status`: `type`, `powerStatus`, `effect`, `light`, `speed`, `direction`, `nv_save`, and `color` with `ColorBlocks` and `ColorBuffer`.
- `Setting/Status`, `System/BatteryProtection`, `Customize/Info`: the switches, the keyboard idle timer (`CloseTimer`), the charge profile (`HealthProtectionStatus`: 0 full capacity, 1 balanced, 2 eco), what is supported.

The program does not trust its own request: a value counts as applied when the service reports it **and**, where the embedded controller can be read (PL1/PL2/PL4, the TCC offset, fan boost and fan mode, the charge profile, the switches, the fan table), when the EC shows the same value. A service that says yes while the EC does not change is reported as *failed* with the difference, not as success.

## Open points

These are the things the program assumes or has not seen. Each is the reason for a design choice above, and each should be checked on the real laptop before anything is built on it (the *Service* page and *Record all link messages* are there for that):

1. **The client class and slot.** The class is derived from the registry values and the others are tried when the broker refuses; that this works with the Monster build's broker has not been seen. If the broker refuses every class, the Service page says *Service refused* and nothing is sent.
2. **Value types and units.** The status layout was read from the program, the numbers were not compared with a live message. The controller checks the echo and the EC afterwards, so a wrong unit shows up as a failed change, not as a wrong value on the hardware.
3. **Detail changes outside the Custom mode.** It is not known whether `SET_OPERATING_MODE_DETAIL` changes only the profile it targets in every mode, so the program never sends it in the ready-made modes (see [SAFETY](SAFETY.md)).
4. **Keyboard blocks and effects.** The number of light blocks is whatever `ColorBlocks` says. The picture on the Lighting page colours each key from the block under it, left to right; the true block-to-key map of the keyboard is not known. Which numbers belong to which effect icon of the vendor's window is taken from the names of the vendor's list.
5. **Save to the keyboard** (`nv_save`) is what the vendor's *Save to BIOS* button sends; what the firmware does with it was not observed.
6. **Switch polarity** (above) is learned at run time.
7. **TCC offset** (`CpuTccOffset`) is compared with the EC register that holds it; the exact relation (offset from the limit, or from a stored maximum) is assumed from the register notes, so a mismatch is reported instead of corrected.
8. **Idle timer** (`CloseTimer`). The unit is assumed to be seconds and `0` to mean *never*; neither was seen on a live message. If the service reports another unit, the change is reported as not confirmed (nothing is written anywhere else).
9. **GPU power.** The extended GPU power (cTGP) is not changed through this link at all; the verified engine does it through the embedded controller (see [REGISTERS](REGISTERS.md)).

If you have a Monster Tulpar T5 or another laptop with this Control Center and can run the program, a report with *Copy diagnostics* from the Service page (and, if you are willing, a *Record all link messages* capture of one change made in the vendor's own window) settles most of these.
