# Setup

## What you need

| | |
|---|---|
| **Windows** | Windows 10 or 11, 64-bit. |
| **A supported laptop** | The *Monster Tulpar T5 V21.7* (SMBIOS board `GM5TG7Y`) is the one unit the register map was measured on, and the only one Control Center Advanced treats as *exactly identified*. Laptops with the same embedded-controller project id (the same chassis family: XMG Neo 15 M21 `GM5TG7W`, Eluktronics MAX-15, the Mechrevo relatives) are recognised as *family*: most features are then locked until you allow experimental writes, and nobody has tested them. The GPU power target is never offered on a *family* laptop: it needs the exact identity (manufacturer, model and BIOS string of the Tulpar T5 V21.7). Anything else is *unrecognised* and gets no hardware writes unless you explicitly allow them. |
| **The OEM Control Center, installed once** | Control Center Advanced reaches the embedded controller through the `ACPIDriver` device that the OEM's package installs. Control Center Advanced does not ship, copy or modify that driver. You do not have to *use* the OEM program afterwards - and while its service runs, it can undo your settings (see [take over](#taking-over-from-the-oem-service)). |
| **PawnIO + a signed module** (optional) | Only for the *Advanced CPU* panel. See [below](#optional-advanced-cpu-registers-pawnio). |

## Install

### Installer (recommended)

Run `Control-Center-Advanced-<version>-Setup.exe` from the [releases page](https://github.com/MehmetNY/Control-Center-Advanced/releases). Windows will ask for administrator rights and, because the file is not code-signed yet, SmartScreen may show a warning (*More info → Run anyway*). Compare the checksum with `SHA256SUMS.txt` from the release first if you want to be careful:

```powershell
Get-FileHash .\Control-Center-Advanced-1.0.0-beta-Setup.exe -Algorithm SHA256
```

The installer offers four components:

- **Control Center Advanced** (required): the program, licence and notices, installed to `C:\Program Files\Control Center Advanced`.
- **Background service** (recommended): installs and starts the `Control Center Advanced` Windows service. It lets the window run without administrator rights, applies your settings at start-up and keeps the fan watchdog running. Without it you have to start the program as administrator each time.
- **Start Menu shortcut**.
- **Desktop shortcut** (off by default).

Silent install: `Setup.exe /S`. Add `/NOSERVICE` to skip the service, and `/D=C:\path` as the very last argument for another folder. When that folder is not a protected one, the service installer copies the program below Program Files itself, because the service must never run a file that a normal user can replace.

### Portable

`Control-Center-Advanced-<version>-portable.exe` is the same program without an installer. Run it anywhere. On the *Safety and service* page, *Install and start the service* copies it to `C:\Program Files\Control Center Advanced` first and installs the service from that copy, never from where you started it. Or skip the service: when the window cannot reach the service it offers *Run elevated without the service* (`--local` mode). One elevated process then does everything, and closing the window ends hardware control.

## First run

1. Open **Overview**. *Embedded controller* should say *Connected*. If it says *Not reachable*, the `ACPIDriver` device is missing: install the OEM Control Center package once, restart, and try again.
2. Open **Safety and service** and read the cards once. *Write protection* shows what is allowed; *Confirmations* is empty; *OEM Control Center* shows whether the OEM service is running.
3. If you only want to look around, switch *Read-only mode* on. Every reading keeps working, every hardware write is refused.
4. Change one thing at a time on **Custom profiles**. A slider shows the live value; moving it changes a draft only. Press **Confirm** to send it. The first risky step opens a confirmation (see [SAFETY](SAFETY.md)).
5. Watch the temperatures on **Overview** and **Components and status** while you test.

## Taking over from the OEM service

The OEM Control Center's background service (GCUBridge) can write to the same registers and silently undo your changes. On **Safety and service → OEM Control Center**, *Take over from the OEM service* stops it and sets it to *Disabled*. The OEM program stops working meanwhile. *Give the OEM service back* undoes this at any time, removing Control Center Advanced's service does it automatically, and from a command prompt:

```bat
"C:\Program Files\Control Center Advanced\ControlCenterAdvanced.exe" --restore-oem
```

If the EC's power-management loop is not active (it is normally switched on by the OEM service), power-limit writes are refused with a message telling you to take over. Once you have taken over and the OEM service is stopped, Control Center Advanced brings the loop up itself before the first write.

## Optional: Advanced CPU registers (PawnIO)

The *Advanced CPU* panel of **Custom profiles** (package PL1/PL2 with time windows, PSys limits, current limit, undervolt offsets, prefetchers, BD PROCHOT) reads and writes processor registers. Control Center Advanced does not contain a driver for that and does not bundle one. It uses [PawnIO](https://pawnio.eu), a signed driver whose signed *modules* enforce an allow-list of registers in the kernel and which refuses unsigned modules:

1. Install PawnIO with its official setup from [pawnio.eu](https://pawnio.eu).
2. Download the signed `IntelMSR.bin` module from the [PawnIO.Modules releases](https://github.com/namazso/PawnIO.Modules/releases).
3. Put the file in `C:\ProgramData\ControlCenterAdvanced\modules\` (create the `modules` folder; only administrators can write there). Control Center Advanced also looks next to the program (`modules\IntelMSR.bin`, `IntelMSR.bin`) and in the PawnIO installation folder.
4. On **Safety and service**, stop and start the service (or restart Windows), then open **Custom profiles → Advanced CPU...**.

If something is missing, the panel says what (for example *PawnIO is not installed*, *IntelMSR.bin not found*, *PawnIO refused the module*) and the rest of the program is unaffected. The processor registers are *Experimental* tier, ask for a confirmation the first time, and a register the firmware has locked is never written. Undervolting is closed by most 11th-generation BIOSes; if yours is, the panel shows the offsets as unavailable.

Control Center Advanced never asks you to turn off a Windows security feature. If Windows blocks the driver, the panel says so.

## Where things are

| What | Where |
|---|---|
| Program and uninstaller | `C:\Program Files\Control Center Advanced` |
| Settings the service applies (`config.kv`, `desired.kv`, `profiles.kv`, `oem_takeover.kv`) | `C:\ProgramData\ControlCenterAdvanced` (writable by SYSTEM and administrators only) |
| Service log | `C:\ProgramData\ControlCenterAdvanced\logs\service.log` |
| Window preferences (`ui.kv`) | `%LOCALAPPDATA%\ControlCenterAdvanced` |
| PawnIO module | `C:\ProgramData\ControlCenterAdvanced\modules\IntelMSR.bin` |

All of them are plain text; nothing is encrypted and nothing is sent anywhere.

## Command line

`ControlCenterAdvanced.exe --help` prints this list.

| Command | What it does |
|---|---|
| *(no arguments)* | The window, talking to the service. |
| `--tray` | Start hidden in the notification area. |
| `--local` | Window and hardware access in one elevated process, without the service. |
| `--lang en` / `--lang tr` | Interface language for this run. |
| `--install-service`, `--uninstall-service`, `--start-service`, `--stop-service` | Manage the service (administrator). `--uninstall-service` also gives the OEM service back. |
| `--restore-oem` | Re-enable and start the OEM service (administrator). |
| `--probe` | Print what Control Center Advanced reads from this machine and exit. Redirect it (`--probe > probe.txt`) to attach it to a bug report. |
| `--service-console` | Run the service logic in a console window (for debugging). |
| `--sim [--sim-model exact\|family\|unknown]` | The window on a simulated laptop. Touches no hardware. |
| `--version`, `--help` | |

## Uninstall

*Settings → Apps → Control Center Advanced*, or `Uninstall.exe` in the program folder. The uninstaller removes the service (and gives the OEM service back), the program, the shortcuts and the start-with-Windows entry, and asks whether to delete your saved settings and logs too (the default is to keep them). The OEM Control Center and PawnIO are left alone.
