# Plan: point-to-point measurements on the plots (issue #80)

Branch: `80-point-measurements`, from `main` at `3779b49`.
Status: **implemented** (2026-10-22). Tested with a virtual X server (Xvfb) and synthetic input.
The user tested it on XQuartz (2026-10-22).

## 1. Goal

Issue #80 asks for these items:

1. Put a marker on a point of a plot (for example, the top of a pulse edge): the waterfall, or the
   AM, FM and PM plots.
2. Put a second marker, and see the difference between the two points in the units of the axis,
   horizontally and vertically.
3. Examples: the time between two pulse edges, the time between two pulses.

## 2. Starting point (`main`)

- The time markers (T1, T2) and the frequency markers (F1, F2) are lines. The status bar shows dT,
  1/dT and dF. They do not measure a value on the Y axis, and they cannot be put at a point of the
  signal with one action.
- All time plots and the waterfall have one time axis, and the spectrum and the waterfall have one
  frequency axis (ImPlot axis links). So a time difference between two different plots is correct.
- ImPlot 0.16 has `DragPoint` and `Annotation`. `App::valueAt` and `App::wfValueAt` read the plotted
  data.

## 3. Decisions

| Question | Decision | Reason |
|---|---|---|
| A point on a time plot: free, or on the trace? | On the trace. Only the time is kept; the value is read from the plotted data at each frame | A free point measures the pointer, not the signal. When you zoom in, the demodulator calculates again at a finer resolution, and the marker stays on the trace |
| How to put a marker | `M` on the hovered plot, or Alt+click. `Shift+M` clears | Ctrl+click (tune), Shift+click (play) and right-click (menus, box zoom) are already used. `M` was free |
| Snap | When placed, the marker goes to the largest or the smallest value within ±8 px, if that value is at least 3 px nearer to the pointer than the sample under it | A click a little above a pulse goes to its top. A click on an edge stays on the edge. Noise on a flat top does not move the marker to the side |
| dT between two plots | Yes | The axes are shared, so it is correct, and the issue's examples need it (a pulse in AM, a chirp in FM) |
| Value difference | Only on the same quantity: the same plot, or I with Q | Other pairs have different units |
| Signed differences | P2 minus P1 | The sign tells the order. 1/dT uses the absolute value |
| A marker out of the view | The time plots keep the last value that was read with the same demodulator settings. The waterfall shows `?` | The demodulator calculates only the view. To measure between two far-apart pulses, you zoom in on each one |
| Live mode | Not available | The data moves under the markers |

## 4. Implementation

| File | Change |
|---|---|
| `src/dsp/measure.*` | `MeasPlot`, `PointMarker`, the rules for which differences apply, `nearestIndex`, `snapIndex`. No ImGui, so `measure_check` tests it |
| `src/app/app_measure.cpp` | Placing, dragging and drawing the markers, the label at P2 and the status bar text |
| `src/app/app.cpp` | `drawPoints` and `pointInput` in the spectrum, the waterfall and `endTimePlot`. The status bar. `Shift+M`. A new file clears the markers. A new sample rate or center frequency moves them, as it moves the other markers |
| `src/app/app_menu.cpp`, `app_export.cpp` | **View → Point markers**, **Clear point markers**, the waterfall menu, the keyboard help |
| `src/io/session.*`, `app_session.cpp` | `"points"` in the session file. The version stays 1: an older session has no `"points"` and gets the defaults |
| `src/app/platform_x11.cpp` | macOS: when the Command key and Alt are on the same X modifier, the key that is down tells them apart (see below) |

### Alt and Command on one X modifier

On macOS, wvt reads the X modifier of the Meta and Super keys as Command, and sends it as Ctrl
(issue #72). Xvfb puts Alt_L and Meta_L on the same key and modifier (Mod1), and XQuartz can do this
with some key settings. Then Alt+click came to wvt as Ctrl+click and tuned the channel. Now, when a
state has a modifier that holds both, wvt uses the Alt or Command key that is down (from the key
events) to tell them apart. Linux builds do not change (`cmdMask` is 0).

## 5. Tests

- `measure_check`: the nearest sample, the snap (a peak, a valley, an edge, a blanked sample, noise
  on a flat top), the difference rules, the plot names.
- `session_check`: the points write and read back. A session without points, and a point on an
  unknown plot, read as "not set".
- On the screen (Xvfb, XTest input): `M` and Alt+click put P1 and P2 on the AM trace, dT and dAM in
  the status bar and the label; zoom in keeps the markers on the trace; a waterfall marker and an FM
  marker give dT only; a session with points opens and saves again.

## 6. Later

- Snap to the 50 % point of an edge (rise and fall time measurements).
- Markers in live mode, on a frozen view.
