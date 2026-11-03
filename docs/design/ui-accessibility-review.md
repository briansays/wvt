# Review: accessibility, usability and presentation of the UI

Branch: `ui-a11y-polish`, from `zmq-udp-stream`.
Status: **implemented** (2026-11-03).

## 1. Scope

The whole desktop UI: the file view, the live view, the signal generator, the export and audio
windows, and the state with no file. The review followed WCAG 2.2 AA where it applies to a desktop
program, and the layout, writing, typography, color and polish checks of the `better-interface`
review.

Evidence: screenshots under Xvfb (`wvt --software … --screenshot`) at 1600×950, 1024×700, 800×600
and `--scale 2` (200 %); contrast computed from the style colors with the WCAG formula; the source.

## 2. Findings and changes

| Finding | WCAG | Change |
|---|---|---|
| Secondary text (`TextDisabled`, about 100 uses: readouts, hints, the status bar) was 4.4:1 on the window and 3.5:1 on a field | 1.4.3 | `TextDisabled` 0.62 grey: 6.6:1 and 5.1:1 (`SetupStyle`) |
| Fields, checkboxes and buttons had no border; their fill was 1.3:1 (fields) and 1.7:1 (buttons) against the window | 1.4.11 | `FrameBorderSize` 1 and a solid border at 3.3:1 against the window, 3.4:1 against popups |
| The spectrum's average (blue) and peak-hold (red) traces were told apart by color only | 1.4.1 | A key beside the spectrum, file and live (`spectrumKey`) |
| The frequency tick labels overlapped at 1024 px and at 200 %, and the last one was cut by the plot edge | 1.4.4, 1.4.10 | Ticks in 1-2-5 steps spaced by the label width, half a label from each end (`setupSpacedTicks`) |
| A long status message was shortened with no way to read it | — | The full text in a tooltip (`textElided`) |
| Point markers could be put only with the pointer (`M`, Alt+click) | 2.1.1 | `Shift+P` and **View → Point at the next spectrum peak**: a marker on the strongest peak in view, then on the next one |
| An unset segment showed its stored value, `-1 s` | — | An empty field with *start* / *end* as placeholders (`InputSIOptional`) |
| With no file, empty plots with made-up axes; only the status bar said what to do | — | A message in place of the plots, with **Open file...** and **Stream from a device** |
| **Save File Session...** in title case among sentence-case menu items | — | **Save file session...** |

## 3. Not changed

- **Screen readers.** Dear ImGui on X11 has no accessibility tree, so no control has a name, a role
  or a state for assistive technology (WCAG 4.1.2). A fix needs an accessibility bridge, a new
  dependency, and is out of scope here.
- **Point markers on the time plots** (I/Q, AM, FM, PM) still need the pointer. `Shift+P` covers
  the spectrum, where most frequency and level measurements are made.
- **Keyboard focus.** ImGui keyboard navigation is on and its focus color is at least 3.4:1 against
  the surfaces it crosses. A full walk with the keyboard was not done: Xvfb had no tool to send keys.
- **The slider grab** covers part of its value text (for example the gain). It is ImGui's own
  slider, and the value stays readable.
- The ellipsis stays `...`: the font is loaded with the Latin-1 glyphs only.
