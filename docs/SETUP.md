# Setup

## What you need

- A 64-bit Windows 10 or 11 on a Monster Tulpar T5 V21.7 (XMG Neo 15 M21, Tongfang GM5TG7). Other laptops of the same family are recognised as *Family* or *Unknown* and get fewer controls; the GPU power engine is off on everything but the exact unit.
- **The vendor's Control Center installed** (the one that came with the laptop; version 4.9.47.11 of the Monster build was studied). Its background services (`GCUBridge`, `GCUService`) and its ACPI driver do the privileged work: Control Center Advanced asks them, the way the vendor's own window does, and reads the embedded controller through the driver. Control Center Advanced does not contain, copy or replace any of it. You do not have to open the vendor's window; the services only have to be running.
- Administrator rights. The program asks for them when it starts (the usual Windows prompt) because reading the embedded controller needs them.

Control Center Advanced installs **no driver and no Windows service of its own**, needs no .NET, no web view and no third-party program, and talks to nothing but the vendor's service on the same computer (`127.0.0.1`).

## Install

### With the installer (recommended)

Run `Control-Center-Advanced-<version>-Setup.exe` and accept the administrator prompt. The installer is **unsigned**, so Windows SmartScreen shows "Windows protected your PC": choose *More info → Run anyway* if you trust the source. Check the file against `SHA256SUMS.txt` on the release page first.

| Component | What it does |
|---|---|
| **Control Center Advanced** (required) | Copies the program, the licence and the notices to `C:\Program Files\Control Center Advanced`. It refuses any folder outside Program Files: the program runs with administrator rights, so its file must not be replaceable by a normal user. |
| Start Menu shortcut | Default on. |
| Desktop shortcut | Default off. |
| Start with Windows | Default off. Creates a scheduled task that starts the program hidden in the tray when you sign in. See below. |

It also stops a running copy first and removes the background service of the first beta (`ControlCenterAdvanced`), if there is one. It does not touch the vendor's Control Center and changes no setting of the laptop.

Silent install, for scripts: `Setup.exe /S`, optionally `/AUTOSTART` (also creates the sign-in task) and, as the last argument, `/D=C:\Program Files\Some folder` for another folder below Program Files.

### Portable

`Control-Center-Advanced-<version>-portable.exe` (or the same file in the `.zip`) runs from any folder, with no installation. Everything the installer does beyond copying the file is optional. One limit: **"Start with Windows" is only offered for the copy in Program Files**, because a sign-in task that starts a program with administrator rights and no prompt must not point at a folder a normal user can write to. A portable copy shows a message that says so; start it by hand or use the installer.

## First start

1. Open Control Center Advanced and accept the administrator prompt. The top bar chip says **Service connected** (green) when the link to the vendor's service works. The *Overview* shows the power status, the active performance mode and what the hardware reports.
2. If the chip says **Service offline**, open *Service*:
   - *Vendor's Control Center → Not installed*: install the vendor's Control Center. Without it only what the embedded controller shows can be read, and the GPU power can be set.
   - *Installed, services Stopped*: press **Start the vendor's services**. It is the same as opening the vendor's window.
   - *Running but refused*: press **Reconnect**; the program tries the client names the service knows. If it still refuses, press **Copy diagnostics** and open an issue.
3. The values in the ready-made modes (Office, Gaming, Turbo) are the vendor's and are shown live but locked. To set your own limits, switch to the **Custom mode** on *Performance → Performance modes*, then use *Performance → Custom mode*.
4. Raising the GPU power (cTGP) above 140 W asks for confirmation and is checked and, if the hardware does not keep it, undone. See [SAFETY](SAFETY.md).

### Start with Windows

*Preferences → Application behaviour → Start with Windows* (or the installer component) creates the scheduled task `ControlCenterAdvanced_Autostart`: it runs the installed program with `--tray` at sign-in, with the highest rights of your account and no prompt. With *Keep my settings applied* on, the program then re-applies your GPU power setting after a restart. Turn the switch off, or uninstall, and the task is removed.

## Upgrading from the first beta (v1.0.0-beta)

The beta was a different design: it ran a service of its own. Installing this version over it:

- removes the beta's service and its sign-in entry;
- keeps your settings folder (`C:\ProgramData\ControlCenterAdvanced`); entries it no longer understands are ignored, and the beta's saved profiles are not used;
- if you had used the beta's *Take over the vendor's service*, the vendor's services may still be disabled. Open *Service → Vendor's Control Center → Start the vendor's services*; it re-enables them as they were (automatic start by default).

## Command line

| Command | What it does |
|---|---|
| `ControlCenterAdvanced.exe` | Opens the window (UAC prompt). One window per user session; a second start brings the first to the front. |
| `--tray` (also `/minimized`) | Starts hidden in the tray. |
| `--lang en` or `--lang tr` | Interface language for this run. |
| `--probe` | Prints the hardware report (model, features, the link, the values read) and exits. |
| `--autostart on` / `off` | Creates or removes the sign-in task and exits: exit code 0 done, 1 it did not work (the reason is printed), 2 wrong use. |
| `--sim` [`--sim-model exact`\|`family`\|`unknown`] [`--sim-version X.Y.Z`] | The window on a pretend laptop and a pretend service, labelled SIMULATION. Nothing real is touched. |
| `--shot <page> <out.png> [--size WxH] [--scale S] [--font F] [--drafts]` | Draws one page of the simulator to a PNG (used for the screenshots in this repository). |
| `--script <file>` | Runs a scripted interface test on the simulator (see [DEVELOPMENT](DEVELOPMENT.md)). |
| `--version`, `--help` | |

## Where things are kept

| Path | What |
|---|---|
| `C:\ProgramData\ControlCenterAdvanced\config.kv` | Switches and confirmations (read-only mode, keep settings applied, consents, ...). Readable by every user, writable by administrators and SYSTEM only. |
| `C:\ProgramData\ControlCenterAdvanced\desired.kv` | The GPU power you chose, to apply it again after sleep or a restart. |
| `C:\ProgramData\ControlCenterAdvanced\logs\local.log` | The log (rotated at 512 KiB, two files). No serial numbers, no user name. |
| `C:\ProgramData\ControlCenterAdvanced\oem_takeover.kv` | Only if the first beta disabled the vendor's services: how to put them back. |
| `%LOCALAPPDATA%\ControlCenterAdvanced\ui.kv` | The window's own preferences: language, accent colour, tray options, key mappings, overlay, application profiles. |

If you switch on *Also write the OEM profile file* on the Custom mode page, the program also writes the vendor's own profile file, after keeping a backup of the original next to it. Nothing is ever sent over the network; the only connection the program opens is the one to the vendor's service on `127.0.0.1`.

## Uninstall

*Settings → Apps → Control Center Advanced → Uninstall* (or `Uninstall.exe` in the program folder; `/S` for silent). It stops the program, removes the sign-in task, the files and the shortcuts, and asks whether to delete your settings and logs too (it keeps them if you say no, or when run silently). The vendor's Control Center is left exactly as it is. A portable copy is uninstalled by deleting the file and, if you created the settings folders above, deleting those.

## Trouble

| What you see | What to do |
|---|---|
| SmartScreen warns about the file | It is unsigned. Compare the SHA-256 with `SHA256SUMS.txt`, then *More info → Run anyway*. |
| Antivirus blocks or quarantines it | The program reads the embedded controller through the vendor's driver and starts at sign-in with administrator rights, which some products flag. The source and the build are in this repository ([DEVELOPMENT](DEVELOPMENT.md)); report false positives to the product's maker. |
| *Service offline* or *Service refused* | See *First start* above, then *Copy diagnostics* from the Service page. |
| Sliders are locked | The ready-made modes keep the vendor's values: use the Custom mode. Also check that *Read-only mode* (Service → Write protection) is off. |
| A change was refused with a message | The message says why (a range, a confirmation, the thermal gate, a service that did not confirm). Nothing was changed. |
| The window cannot be found | It is in the tray (the program keeps running when the window is closed; change that in Preferences). Starting the program again brings it to the front. |

Open an [issue](https://github.com/MehmetNY/Control-Center-Advanced/issues/new/choose) with the *Copy diagnostics* text from the Service page for anything else.
