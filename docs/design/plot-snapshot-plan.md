# Plan: snapshot button for all graphs (issue #13)

Status: implemented (2026-10-20) on `13-plot-snapshot`. The user pages are in
[Plot snapshots](../snapshots.md).

## 1. Goal

From the issue:

- Export a high-quality PNG of the current state of each plot.
- The file name is `<recording-file-name>_capture_<plot-name>_<integer>.<extension>`.
- A main button at the bottom of the screen opens a window. The window selects the plots, has a
  folder (default `~/wvt-recordings/`), and has **Export** and **Cancel** buttons.

## 2. Decisions

| Question | Decision | Why |
|---|---|---|
| How to make the image | Read the rendered frame and crop each plot | The image is exactly what the user sees. No second render path to keep the same as the screen |
| Resolution | The screen resolution | "High quality" is met by a lossless PNG. For larger images, use a larger window or `--scale`. An off-screen render at 2x is possible later |
| PNG encoder | `stb_image_write.h` 1.16 (public domain / MIT), in `third_party/stb/` | One header, no new system library. zlib is not on all build hosts |
| Plots | Overview, spectrum, waterfall (with the color bar), I/Q, AM, FM, PM. Live: spectrum, waterfall | All the plots of the main window. The generator preview is not included |
| Time plot area | The header row (name, readout) and the plot | Since #19 the name and readout are outside the plot |
| Plots not on the screen | Not available in the window | A hidden plot has no pixels |
| Scrolled time plots | Saved as clipped, with a "partly hidden" note | The pixels outside the panel do not exist |
| Number | One number for all the files of one export: the first number that no plot of this recording uses in the folder | Files of one export group together; nothing is overwritten |
| Name without a file | `live` | Device mode has no recording name |
| Pointer | Hidden for two frames before the capture | The crosshair lags one frame. No crosshair or tooltip in the images |
| Where | **Snapshot...** button at the left of the status bar, and **File → Save plot snapshots...** | The issue asks for a button at the bottom |

## 3. Implementation notes

- `src/snapshot.*`: names, numbering, crop and PNG writing, without ImGui. `tools/snapshot_check`
  tests them.
- `src/app_snapshot.cpp`: the window, the plot rectangles (`snapNote*`), and the capture.
- Inside `BeginPlot`, the current ImGui window is the plot's own window, so its clip rectangle is
  the plot. The clip for the scrolled time plots comes from the `##plots` child window instead
  (`snapClipToWindow`).
- `X11Window::readFrame` reads the GL front buffer or the software renderer's pixels. The
  `--screenshot` option uses it too.
- Tests: Xvfb with the software renderer, file mode (7 plots, the numbers 1 and 2) and the
  simulator (2 plots). The GLX path uses the same read as `--screenshot`.
