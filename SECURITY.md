# Security policy

Control Center Advanced runs with administrator rights, asks the vendor's service to change hardware settings and writes two GPU power registers itself, so security and safety bugs are taken seriously. This is a one-person hobby project: reports are read and answered on a best-effort basis, with no guaranteed response time.

## Supported versions

Only the latest release receives fixes. The 2.x releases are **pre-releases that have not been verified on real hardware** (see [Capabilities and verification](docs/CAPABILITIES.md)); the 1.0.0 beta is a different design and is no longer maintained. Do not rely on any of them for anything that matters.

## How to report a problem

- **A vulnerability** (privilege escalation through the program, its installer or its start-up task, a way past the write guard or the message allow-list, a way to make the elevated program load or run something it should not, a way to make it connect anywhere but the loopback address): use **Security → Report a vulnerability** on the [repository page](https://github.com/MehmetNY/Control-Center-Advanced/security) so that the report stays private until a fix exists. If that button is not available, open an ordinary issue titled "Security: please contact me" **without any details** and wait for a reply.
- **A safety bug** (a setting that applies something other than what you asked, a limit that is not enforced, a protection that does not trigger, a value the hardware keeps after the program says it was reverted): this is not secret. Open a [bug report](https://github.com/MehmetNY/Control-Center-Advanced/issues/new?template=bug_report.yml) with the *Copy diagnostics* text from the *Service* page.

Please include the version (*Device info → About*), your Windows version, and how to reproduce the problem. Please do not test against other people's computers.

## What the design already assumes

These are documented behaviours, not vulnerabilities (details in [SAFETY](docs/SAFETY.md) and [ARCHITECTURE](docs/ARCHITECTURE.md)):

- The program runs **elevated** (it asks for administrator rights when it starts) because reading the embedded controller needs them. It has no service, no named pipe and no listening socket of its own.
- Its only connection is an outgoing one to the vendor's own broker on `127.0.0.1`. The program refuses every other address, and the release check fails if that guard is missing. The broker belongs to the vendor's Control Center: it accepts clients on the local machine whether or not this program exists, and this program neither opens nor widens that access. It identifies itself to the broker the way the vendor's own window does (the pattern is in `src/oem/proto.cpp`).
- Requests carry setting names and values, never register addresses or free-form messages. Every message to the vendor's service passes an allow-list; the only registers written to the embedded controller are the two GPU power (cTGP) registers, behind a write guard with a never-write list.
- A no-prompt elevated start at sign-in ("Start with Windows") is only created for a copy below `Program Files` or the Windows folder, and the installer refuses any other folder: a program that runs with administrator rights must not live where a normal user can replace it.
- The settings folder `C:\ProgramData\ControlCenterAdvanced` can be changed by administrators and SYSTEM only; other users can read it. It holds no secrets.
- The program contains no Internet client code. It opens `https://` links in your default browser only when you click them.
- The program is **unsigned**. Verify the SHA-256 checksum published with each release.

## What is in scope

- The program, the installer and uninstaller and the start-up task.
- The write guard, the message allow-list, the ready-made-mode rule and the never-written register list.
- Anything that lets an unprivileged user or another program obtain more than the above allows (for example through the settings folder, the start-up task or the folders the installer accepts).

Out of scope: a bad value that is inside the documented limits, the OEM Control Center and its services and driver (including the fact that any program on the same computer can talk to the vendor's broker), physical access to the laptop, and hardware or firmware behaviour that Control Center Advanced cannot see.

## Disclosure

When a fix is released, the release notes say what was wrong and who found it (if you want to be named). Please give the project a reasonable time to fix a vulnerability before you publish details.
