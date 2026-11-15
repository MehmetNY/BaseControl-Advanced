# Design

The goal was a control panel that keeps the layout and every setting of the vendor's Control Center, feels like part of Windows, costs almost nothing while it sits in the tray, and never leaves you wondering *what will happen when I touch this*. The look is darker and more saturated than the first beta (the beta's colours were too pale): a blue-black surface, an orange accent and bright status colours. It is drawn from scratch in native code: **no web view, no .NET, no UI framework, no bitmap assets.** The whole program is one executable of about 3.6 MB.

## Principles

1. **Live first.** Every slider starts at what the hardware reports right now and keeps following it, so a change made by another program shows up. A reading is marked *LIVE* (a green dot); a value the program only stores for later is marked *STORED* (a grey dot).
2. **Nothing happens until you say so.** Moving a slider edits a *draft*. Only that slider's own **Confirm** button sends it; the undo button next to it throws the draft away. This holds for every slider that changes a hardware or Windows setting, and for the drop-downs for refresh rate, cooling mode and similar choices that are worth a second look.
3. **Say why.** A control that cannot work is disabled and says why in plain words ("the vendor's service is not reachable", "the ready-made modes keep their own values") instead of disappearing or failing silently.
4. **Honest labels.** A setting row carries a chip with its evidence tier - *Verified* (measured on a GM5TG7Y), *Service* (done by the vendor's service, read back afterwards) or *OS* (a plain Windows call) - and a tooltip that says what it means. A simulated screen says SIMULATION. The Service page lists what the program never does.
5. **Quiet in the background.** A hidden window costs one timer tick per second, and the hardware is polled every few seconds.
6. **Two languages, equally good.** English and Turkish are both first-class, and the layout is checked in both (the screenshot tool renders any page in either language).

## Look

| | |
|---|---|
| Base / rail / panel / inset / elevated | `#0D1015` / `#12161D` / `#171C25` / `#10141B` / `#1F2631` |
| Hover / line | `#29323F` / `#2B3544` |
| Text / muted / faint | `#F5F8FC` / `#A3AFC2` / `#6C7A90` |
| Accent (default *Ember*) | `#FF7A2F`; also *Cyan* `#22D3EE`, *Lime* `#7BE35B`, *Violet* `#A78BFA`, *Rose* `#FF4D8D`, *Gold* `#FFC53D` (Preferences → Appearance) |
| Status colours: amber, green, blue, red | `#FFC53D`, `#3DDC84`, `#3BA7FF`, `#FF5470` |
| Corner radius | 13 px for cards; dialogs 16 px; controls smaller |
| Text font | The first installed of *Segoe UI Variable Text*, *Segoe UI*, *Inter*, *Noto Sans*, *Tahoma*, *Liberation Sans*, *DejaVu Sans*, *Arial* (semibold weight from *Segoe UI Semibold* when the variable font is chosen). `--font <family>` forces one. |
| Number font | The first installed of *Cascadia Mono*, *Consolas*, *DejaVu Sans Mono*, *Liberation Mono*, *Courier New* - used where digits must line up (register values, the board name). |
| Icons | 49 vector icons drawn from code, so they scale with the display and add no files. |
| Scaling | Per-monitor DPI awareness (PerMonitorV2 in the manifest). Layout is in logical pixels and multiplied by the window's DPI when it is drawn. |

The application icon is a bolt in the same orange on a dark rounded square. `assets/icon.svg` is its source; `res/app.ico` (16 to 256 px) and `assets/icon-256.png` are rendered from it, and `tools/make_art.py` draws the installer bitmaps from the same palette.

## Layout

The layout follows the vendor's window: the same seven groups, the same screens, in the same order.

- **Navigation rail**, 236 px wide: the brand, then seven groups - *Overview*, *General settings*, *Performance*, *Lighting*, *Battery*, *Display settings*, *Device info* - and *Service* pinned to the bottom with the model and board name above it. Below 1000 px of window width the rail shrinks to 68 px of icons with tooltips.
- **Top bar**: a breadcrumb (*Control Center Advanced / page*) on the left; on the right, from the edge inwards, the **EN | TR** switch, a **link chip** (*Service connected* in green, *Connecting...* in amber, *Service refused* or *Service offline* in red, *SIMULATION* in amber), the **active performance mode** and the **power source** with the battery level.
- **Page header**: a small section label, the page title, a sentence of explanation and, for groups with several pages, a row of tabs (*General settings* has five, *Performance* four, *Device info* two).
- **Pages are masonry grids of cards**: one to three columns (mostly two) depending on the window width, each card one idea (a title, a subtitle, rows of controls or readings). The page scrolls as a whole.

## A setting row

The slider row is the unit the performance pages are built from:

```
Title  [Service]                       ● LIVE  95 W  →  120 W     [↶] [ Confirm ]
├────────────────────────┃────────────────●───────────────────────┤
                      live marker      draft handle
```

- The thin vertical **marker** on the track is the live hardware value. The **handle** is your draft. While they differ, the live value is shown dimmed, then an arrow, then the draft in the accent colour (amber above a soft limit - the track turns amber too).
- **Confirm** is enabled only when the draft differs from the live value, passes the setting's own validation and nothing is pending; while the request runs it reads *Applying* and shows a spinner. Its tooltip says what will be sent ("Send 120 W to the hardware now. Nothing is sent until you press this.") or why it is disabled.
- Dragging the handle back onto the live value cancels the draft by itself.
- **Keyboard:** after a slider has been clicked, the arrow keys move it by one step, Page Up/Down by ten steps, Home/End to the ends.
- A slider whose value cannot be changed right now (the ready-made modes, a service that is not reachable, read-only mode) keeps showing the live value; its handle is locked and the reason is in the tooltip. A *Switch to Custom mode* button is offered where that is the way out.
- The row also shows the range, step and the setting's key in a tooltip, so every value can be traced to [SETTINGS](SETTINGS.md).

*Confirm all* on *Custom mode* sends every pending draft of the page. It first checks the set as a whole (PL1 not above PL2, PL2 not above PL4) and stays disabled with the reason if the set is inconsistent. It then applies the limits in an order that keeps PL1 <= PL2 <= PL4 true after every single step - limits that go up from the outermost gate inwards, limits that go down from the innermost outwards - and stops at the first value that is refused.

## Feedback

- **Toasts** appear at the bottom right for a few seconds and report the outcome of a request in one line (green tick, amber warning, red cross, blue info). The text of a refusal comes from the controller and is translated.
- **Dialogs** are used for things that deserve a second look: consent for risky writes, restoring the vendor's defaults or curves, deleting a mapping or a program entry, remapping a typing key. Every dialog has a *Cancel* and a clearly named action. **Esc cancels; Enter does nothing**, so a stray key press can never be the "yes".
- **Tooltips** explain disabled controls, evidence tiers and live/stored readings.

## Why GDI+ and immediate mode

- It is on every Windows installation: nothing to ship, nothing to patch, no runtime to update.
- An immediate-mode toolkit (draw the frame from the current state, repaint only when something changed) is small, needs no widget tree to keep in sync with the hardware snapshot, and can be driven offscreen by the same code that draws the real window - which is how the screenshots and the scripted UI tests in `tests/ui` are made.
- The window ticks every 100 ms while it is visible and once a second while it is hidden; a repaint happens only when the state changed, and a 16 ms animation timer exists only while something is animating (a hover fade, a dialog opening). A hidden Control Center Advanced therefore wakes up about once a second.
- Text measurements and wrapped lines are cached.

## Known gaps

- **Accessibility.** There is no Tab-key navigation and no UI Automation / screen-reader support in this version. Keyboard use is limited to text fields, clicked sliders, Esc in dialogs and the global hotkeys. This is the main cost of drawing everything ourselves, and the most important thing to improve before calling the interface finished.
- **Narrow windows.** Below about 980 px the layout still works (cards stack, the rail collapses) but has not been polished.
- **Everything here was reviewed in screenshots from the simulator** rendered under Wine, not on a real Windows desktop at different DPI settings.

## What was deliberately left out

- A web view, a chart library, animation for its own sake.
- A cloud account, telemetry, update checks - anything that talks to the internet.
- Downloading the panel's factory colour profile (the original fetches it from the manufacturer using the serial number stored in the BIOS).
- Anything of the first beta's extras that duplicated what the vendor's service already does (saved profiles of our own, the *Advanced CPU* panel, a service of our own). See [CAPABILITIES](CAPABILITIES.md).
