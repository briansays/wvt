# Plot snapshots

wvt saves the plots as PNG files, as they show on the screen. Use the snapshots in reports or to
compare two recordings.

## Save snapshots

1. Set the plots as you want them in the snapshot.
2. Click **Snapshot...** at the bottom left of the window, or select **File → Save plot
   snapshots...**.
3. In the **Save plot snapshots** window, tick the plots to save. **All** and **None** tick all
   the plots or no plots.
4. Make sure that the **Folder** is correct. The default is `~/wvt-recordings/`. wvt makes the
   folder if it does not exist.
5. Click **Export**.

The window closes, and wvt saves one PNG file for each plot. The status bar shows the result. To
close the window without a save, click **Cancel**.

## Plots

| Plot | Mode | Contents |
|---|---|---|
| Overview strip | File | The power over the full file or segment, with the view box |
| Spectrum | File, Device | The spectrum of the view, or the live spectrum |
| Waterfall | File, Device | The waterfall with its color bar |
| I / Q, AM, FM, PM | File | The time plot with its header (name, readout) |

- A plot that is not on the screen is not available. For example, a hidden time plot, or the time
  plots in Device mode.
- The time plots scroll. If a time plot is only partly on the screen, the window shows **partly
  hidden**. wvt then saves only the part on the screen. To save all of the plot, scroll the plots
  or set a smaller plot height in the channel options.

## File names

The file name is `<recording>_capture_<plot>_<n>.png`, for example
`cap_915MHz_capture_waterfall_3.png`.

- `<recording>` is the name of the open recording without its folder and extension. In Device mode,
  it is `live`.
- `<plot>` is `overview`, `spectrum`, `waterfall`, `iq`, `am`, `fm` or `pm`.
- `<n>` is the first number that no snapshot of this recording in the folder uses. All the files of
  one export have the same number. Thus wvt does not write over a previous snapshot.

The window shows the names before you export.

## Image quality

- The PNG files have no loss. Each pixel is the same as on the screen.
- The image size is the size of the plot on the screen. For larger images, make the window or the
  plot larger, or start wvt with a larger `--scale`.
- The pointer crosshair and tooltips are not in the snapshots.

## How it works

Each plot records its screen rectangle on each frame. When you click **Export**, wvt hides the
pointer for two frames, so that no crosshair or tooltip shows. After the next frame is drawn, wvt
reads the frame (from OpenGL or the software renderer) and cuts out each plot. The PNG encoder is
`stb_image_write` in `third_party/stb/`. The self-check `build/snapshot_check` tests the file
names, the numbering, the crop and the PNG files.

For the design, refer to [the snapshot design record](design/plot-snapshot-plan.md).
