# Plan: labels outside the plots and a tidier UI (issues #12 and #19)

Branch: `19-ui-cleanup`, from `main` at `328f5be`.
Status: **steps 1–10 implemented** (2026-10-20). Not yet checked on screen by the user.

## 1. Goal

Issue #12: move the labels that are drawn inside the plots to just outside them, and
give the plots more white space, so the data is easier to see.

Issue #19: the UI has become cluttered.

- Replace many of the buttons with drop-down menu items.
- Keep the plots readable when the window is small.
- Group related controls so that they are easier to find.

Both issues change the same code (`src/app.cpp`, `src/app_live.cpp` and the plot
helpers), so this plan covers them together.

## 2. Starting point (`main`)

Screenshots taken with `--screenshot` on Xvfb, at 1600×950 and 1100×700, in file mode
(simulator recording, channel at +400 kHz) and in live mode (`--device sim`).

**Inside the plots (#12):**

| Where | What | Problem |
|---|---|---|
| I/Q, AM, FM, PM plots | Measurement readout in the top-left corner on a dark box (`endTimePlot` overlay), e.g. "power mean -30.3 dBFS min -69.7 max -22.4" | Hides the trace; at 1100 px it covers a third of the plot width |
| I/Q plot | Legend (I, Q) in the top-right corner | Hides the trace |
| Waterfall during *Select region* | "Drag a box to export; Esc cancels" | Over the image (short-lived, but in the way) |
| Signal generator preview | Legend in the top-right corner | Same as I/Q |
| Spectrum (file and live) | "Spectrum of view" / "Live spectrum" beside the plot | Already outside: it sits in the column kept free so that the spectrum lines up with the waterfall. It stays. |
| Markers | F1/F2 and T1/T2 tags | Already on the axes. They stay. |

**Spacing and clipping (#12):**

- `PlotPadding` is 18×6 px. The last X tick label is cut off at the right edge ("520 ms",
  "101.2 MI" in live mode).
- The colour bar is a fixed 4.5 em wide, so "-100 dB" shows as "-100 d".
- At 700 px of height the spectrum plot is about 70 px tall: its five dB tick labels touch
  each other.

**Clutter (#19):**

- File mode has three toolbar rows above the plots, with about 40 controls: the 4 window
  buttons, the file fields, 9 navigation buttons, span, start, sample, the bookmarks and
  3 marker checkboxes. At 1100 px the toolbar wraps to five rows plus the overview strip,
  and takes about 210 px before the first plot.
- The waterfall pane has 13 controls on 2–3 rows, and the channel pane has 11 controls
  on 2–3 rows.
- Every time plot has its own row of controls (mode, smooth, checkboxes, Auto Y).
- Live mode has the same pattern: the device row, the record row (prefix, format,
  samples, limit) and 14 spectrum controls.
- The Signal gen, Audio and Export windows open from buttons in the first row.

## 3. Design

### 3.1 Labels outside the plots (#12)

1. **Readouts** move out of the plot to the plot's header row, right-aligned after its
   controls, as dim text. When the row is too narrow they wrap to the next line (the
   existing `flow()` helper) instead of covering the plot. `endTimePlot` no longer draws
   on the plot.
2. **The I/Q legend** moves above the plot with `ImPlotLegendFlags_Outside` (North,
   horizontal). Clicking an entry still hides that trace. The signal generator preview
   gets the same treatment.
3. **The selection hint** ("Drag a box to export; Esc cancels") moves to the status bar
   while a selection is active. The waterfall shows only the crosshair.
4. **White space:**
   - `PlotPadding` becomes about 10×10 px, with room on the right for the last tick label.
   - A small gap between stacked plots.
   - The colour bar width is computed from the widest label ("-100 dB") instead of being
     a fixed 4.5 em.
5. **Minimum sizes:** the spectrum keeps at least about 5 lines of text in height, and a
   time plot at least about 7. When there isn't room, the plot stack scrolls (as it does
   now) instead of squashing.

### 3.2 Menu bar and grouped controls (#19)

**A menu bar** (ImGui's menu bar on the main window) replaces the window buttons and
the view checkboxes. Shortcuts are shown next to each item:

| Menu | Items |
|---|---|
| File | Open… (puts the cursor in the path box), Export region… (Ctrl+E), Quit (Ctrl+Q) |
| View | Time markers (T), Freq markers (F), Relative frequency, Peak hold, I/Q / AM / FM / PM plots, Overview strip, Show everything (A) |
| Tools | Signal generator (Ctrl+G), Audio (Ctrl+D), Use GPU (only when a GPU is available) |
| Help | Keyboard shortcuts, About (version, GPU, renderer) |

Status that must stay visible goes on the right of the menu bar: the source selector
(File / Device), a red **TX ON** while transmitting, and **REC** while recording.

**File row:**

- Kept on the row: the File path and Open, Sample rate, Center freq, Segment, and the
  file info.
- Format, Header bytes, Swap I/Q and Big endian move into a **format button** that shows
  a summary ("ci16 ▾", or "ci16 BE swap ▾") and opens a small popup. SigMF and gqrx
  names set these automatically, so they are rarely edited.

**Navigation row:**

- The `|< << < > >> >|  + - All` buttons stay, as one tight group.
- Span (preset and value) and Start stay.
- Sample moves into the Start field's popup (or tooltip) as "Go to sample".
- Bookmark and the bookmark list become one **Bookmarks ▾** combo, with "Add bookmark (B)"
  as its first item.

**Waterfall pane:**

- One row: FFT, Window, dB min, dB max, Auto, and a **⚙** button.
- The ⚙ button opens "Waterfall settings": Overlap, Avg, Rows, Cols, Colormap and Peak.
  GPU is in the Tools menu.
- The "FFT sees N%" warning stays on the row.

**Channel pane:**

- One row: Channel, Offset, BW, To peak, ⚙, then the info text ("out 300 kSps | D=8 | …").
- The ⚙ button opens Taps, Decim, Squelch (with its level), Max pts and Plot H.

**Each time plot:**

- The header is `☑ Name   readout …  ⚙`.
- The ⚙ button opens that plot's options: Mode, Smooth, Degrees, Rel. to channel and
  Auto Y.
- The same options are also added to the plot's right-click menu.

**Live mode** follows the same pattern:

- Device row: Device, Rescan, Center freq, Sample rate, Gain, AGC, Start/Stop, and ⚙
  for the device-specific settings (Bias-T, direct sampling, antenna, bandwidth).
- The record row shrinks to a red **Record** button, its status text, and ⚙ for the
  prefix, format, samples and limit.
- The spectrum controls use the waterfall pattern: FFT, Window, dB range, Auto, then ⚙
  for Avg, Rows/s, History, Cols, Spec avg, Colormap and Peak/reset.

**Shared helpers:** `settingsButton(id)` (a ⚙ button that opens a popup, using the
`beginForm`/`row` table layout already in `app_export.cpp`), plus a thin vertical
separator between groups on a row. The duplicated `tooltipAlways`/`row`/`beginForm`
helpers in four files move into `ui_util`.

### 3.3 Small windows

- Fewer controls means fewer wrapped rows. The target is 2 rows above the overview at
  1100 px wide, against 5 now.
- The overview strip can be hidden from the View menu. It hides by itself when the
  window is shorter than about 600 px.
- The left/right split keeps a minimum width for each side that fits one row of its
  controls.
- The status bar shortens long paths in the middle (`/private/…/sim_…sigmf-data`), so the
  GPU/fps text on the right is no longer covered (it is covered now at 1100 px).

### 3.4 Not changed

- Keyboard shortcuts, command-line options, mouse actions (Ctrl+click, Shift+click,
  right-drag zoom, the waterfall right-click menu) and the floating windows.
- The contents of the Signal gen, Audio and Export windows. Only their legends and
  spacing change.
- Settings are still not saved between runs (`NoSavedSettings`). This could be a later
  issue.

## 4. Implementation steps

1. **Helpers.** Move `tooltipAlways`, `row` and `beginForm` into `ui_util`, and add
   `settingsButton` and a group separator. No visible change.
2. **#12, readouts and legends.** Readouts go on the header rows; I/Q and preview legends
   go outside; the selection hint goes to the status bar.
3. **#12, spacing.** Plot padding, the colour bar width, minimum plot heights and the gap
   between plots.
4. **#19, menu bar.** File/View/Tools/Help, the source selector, and the TX/REC
   indicators. Remove the window buttons and the view checkboxes from the toolbar.
5. **#19, file and navigation rows.** The format popup, Bookmarks ▾, and the sample field
   moved under Start.
6. **#19, the waterfall and channel panes.** One row each, plus ⚙ popups.
7. **#19, the time plots.** `☑ Name  readout  ⚙` headers and the right-click options.
8. **#19, live mode.** The device, record and spectrum rows.
9. **Small windows.** Hiding the overview, the minimum widths of the split and the
   shortened status path.
10. **Docs.** Update the README GUI section (the menu bar, where each setting moved) and
    add implementation notes to this plan.

## 5. Testing

- **Screenshots** (`--screenshot`, Xvfb over TCP) at 1600×950, 1280×800, 1100×700 and
  900×600, in file mode with the channel on and in live mode with `--device sim`, before
  and after each step. A small script in the scratchpad takes the whole set.
- **Check by eye:**
  - nothing is drawn over the data, apart from the markers, the channel band and the
    hover lines;
  - no tick labels are cut off;
  - each pane fits on one row at 1600 px.
- **Self-tests:** all of them still pass. They don't touch the GUI, but `app.cpp` builds
  into the same targets.
- **The CLI options that drive the GUI still work:** `--chan`, `--view`, `--segment`,
  `--siggen`, `--gen`, `--export-window` and `--device`.
- **On screen (user):** menus and shortcuts, the ⚙ popups, legend click-to-hide, the
  region selection hint, and resizing the window down to about 900×600.

## 6. Decisions needed

1. **One branch or two?** Recommended: one branch, `19-ui-cleanup`, closing both issues
   with separate commits. The readouts from #12 go into the plot headers that #19
   rebuilds, so doing #12 alone first would mean redoing those headers.
2. **A menu bar or a single ☰ button?** Recommended: a menu bar. It costs one row but
   removes the window buttons and the checkboxes, and the shortcuts become easy to find.
3. **Sample rate and center freq:** keep them on the file row (recommended, since they
   are often typed for raw files), or move them into the format popup?
4. **Time plot options:** a ⚙ popup *and* the right-click menu (recommended), or only
   one of the two?
5. **Readouts:** right-aligned on the header row (recommended), or on a line under each
   plot?
6. **Sample field:** move it under Start (recommended) or keep it on the row?

## 7. Decisions (2026-10-20)

The user chose the recommendations: one branch (`19-ui-cleanup`) for both issues, a menu bar,
sample rate and center frequency kept on the file row, time-plot options behind ⚙ and on the
right-click, readouts on the header rows, and the sample field moved off the row.

## 8. Implementation notes

### Files

| File | Change |
|---|---|
| `src/app_menu.cpp` (new) | Menu bar, the right-hand indicators and source selector, the keyboard and mouse window, About |
| `src/app.cpp`, `src/app.h` | File and navigation rows, waterfall settings row and ⚙, channel row and ⚙, time-plot headers, options popups and right-click, `showAll`, `toggleExport`, `addBookmark`, Ctrl+Q, status-bar path shortening |
| `src/app_live.cpp` | Device ⚙ (RTL-SDR / USRP options), Record + ⚙, spectrum settings row and ⚙, source selector in the menu bar |
| `src/app_siggen.cpp`, `src/app_audio.cpp`, `src/app_export.cpp` | Toolbar buttons removed; shared helpers used; preview legend outside; selection hint moved to the status bar |
| `src/ui_util.*` | `tooltipAlways`, `beginForm` / `row`, `redButton` (moved from four files), `settingsButton` (a drawn gear: the font has no gear glyph), `colorbar` / `colorbarWidth`, `elideMiddle`; plot padding 24×10 px |
| `src/main.cpp` | The loop ends on File → Quit / Ctrl+Q |
| `README.md` | Screen layout, mouse and keyboard, and the places that named the old buttons |

### Differences from the plan

- **Sample field:** it went into a **Go to** drop-down together with *Add bookmark* and the
  bookmarks (one control instead of three). The Start field's tooltip shows the sample index.
- **Header layout:** `☑ Name ⚙ readout` — the ⚙ sits right after the name, so it does not move
  when the readout changes length or wraps.
- **Right-click on a time plot** now opens wvt's options popup instead of ImPlot's own plot menu
  (`ImPlotFlags_NoMenus`, as on the waterfall). Double-click still fits; right-drag still zooms.
- **dB min / max** lost their −/+ step buttons to fit on one row; type the value, or use *Auto*.
- **Peak hold** is in **View** and in the waterfall / live ⚙ options. **Use the GPU** moved to
  **Tools**.
- **Group separators:** not added; the rows keep the wider flow gap between groups, which
  reads clearly once each row has fewer controls.
- **Minimum split widths** were not added: with fewer controls, each pane wraps to at most two
  rows at 900 px.
- The live **record row** joined the device row (it wraps when there is no room).

### Toolbar rows above the overview

| Window | Before | After (file) | After (live) |
|---|---|---|---|
| 1600×950 | 3 | 2 | 2 |
| 1100×700 | 5 | 3 | 2 |
| 900×600 | — | 3 (no overview) | 2 |

### Verification (macOS)

- Screenshots on Xvfb (TCP display, `--software`) at 1600×950, 1280×800, 1100×700 and 900×600,
  file and live mode: no readout or legend over the traces, no cut-off tick labels, the colour
  bar shows "-100 dB" in full.
- Menus and popups opened with XTest clicks (a ctypes script) and captured with `xwd`: View menu,
  Format drop-down, waterfall ⚙, channel ⚙, a right-click on the AM plot, Go to, Help → Keyboard
  and mouse, live Record ⚙. File → Quit ends the program.
- `--export-window` and `--gen pulsed` open their windows as before.
- All six self-tests pass.
