# Security policy

Control Center Advanced runs a background service as LocalSystem and writes to firmware-adjacent hardware registers, so security and safety bugs are taken seriously. This is a one-person hobby project: reports are read and answered on a best-effort basis, with no guaranteed response time.

## Supported versions

Only the latest release receives fixes. The 1.x releases are **betas that have not been verified on real hardware** (a first trial found problems with the keyboard lighting) (see [Capabilities and verification](docs/CAPABILITIES.md)); do not rely on them for anything that matters.

## How to report a problem

- **A vulnerability** (privilege escalation, a way past the write guard, a way to make the service run or load something it should not, a way to reach the pipe from another machine): use **Security → Report a vulnerability** on the [repository page](https://github.com/MehmetNY/Control-Center-Advanced/security) so that the report stays private until a fix exists. If that button is not available, open an ordinary issue titled "Security: please contact me" **without any details** and wait for a reply.
- **A safety bug** (a setting that applies something other than what you asked, a limit that is not enforced, a protection that does not trigger, a value the hardware keeps after the program says it was reverted): this is not secret. Open a [bug report](https://github.com/MehmetNY/Control-Center-Advanced/issues/new?template=bug_report.yml) with the *Diagnostics* card from *Safety and service*.

Please include the version (*Device info → About*), your Windows version, and how to reproduce the problem. Please do not test against other people's computers.

## What the design already assumes

These are documented behaviours, not vulnerabilities (details in [SAFETY](docs/SAFETY.md)):

- The service accepts requests from **any local, authenticated user** over its named pipe unless *Only administrators can change settings* is on. A local user can therefore change power limits and fan behaviour on a shared computer; switch the option on if that matters to you. Settings are typed and range-checked in the service, and the pipe is local-only, rate-limited and protected against squatting.
- Requests carry setting names and values, never register addresses; every EC write passes an allow-list with a never-write list.
- The service runs a binary only from a protected folder (`Program Files`).
- The program contains no networking code. It opens `https://` links in your default browser only when you click them.
- Direct CPU register access needs the optional PawnIO driver, which you install and trust yourself; Control Center Advanced does not ship a driver.
- The program is **unsigned**. Verify the SHA-256 checksum published with each release.

## What is in scope

- The service, the pipe protocol and the installer/uninstaller.
- The write guard, the evidence-tier gates and the never-written register list.
- Anything that lets an unprivileged user or another program obtain more than the above allows.

Out of scope: a bad value that is inside the documented limits, the OEM Control Center, PawnIO or the OEM driver, physical access to the laptop, and hardware or firmware behaviour that Control Center Advanced cannot see.

## Disclosure

When a fix is released, the release notes say what was wrong and who found it (if you want to be named). Please give the project a reasonable time to fix a vulnerability before you publish details.
