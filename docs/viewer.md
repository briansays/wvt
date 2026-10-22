# The file viewer

The file viewer shows a raw IQ recording. This page tells you how wvt opens a file, and what each
area of the window shows.

## Open a file

Give the file on the command line, or type its path in the file row and click **Open**:

```sh
wvt recording.sigmf-meta
```

To open a saved session (the recording and the view, plots and channel that you saved), open the
session file in the same way. Refer to [File sessions](sessions.md).

### Sample formats

wvt reads these sample formats. Add `_be` for big-endian samples, for example `ci16_be`.

| Format | Sample |
|---|---|
| `cf32` | Complex 32-bit float |
| `cf64` | Complex 64-bit float |
| `ci32` | Complex 32-bit signed integer |
| `ci16` | Complex 16-bit signed integer |
| `ci8` | Complex 8-bit signed integer |
| `cu8` | Complex 8-bit unsigned integer (RTL-SDR) |

### How wvt finds the file settings

wvt gets the sample format, the sample rate and the center frequency from these sources, in this
order:

1. **SigMF** `.sigmf-meta` and `.sigmf-data` files.
2. **gqrx** file names, for example `gqrx_YYYYMMDD_HHMMSS_<fc>_<rate>_fc.raw`.
3. Values in the file name, for example `2.4MSps` or `915MHz`.
4. The file extension, for example `.cu8`, `.cs8`, `.cs16`, `.cf32`, `.cfile` or `.raw`.

wvt reads the SigMF `core:datatype` from `global`, and `core:sample_rate`, `core:frequency` and
`core:datetime` from `global` or the first capture. The datatype must be a complex type in the
table above (`_le` or `_be`). wvt does not open a SigMF file with a different datatype (for
example `ri16_le`, real samples), because it would show noise. To read the file with a
different format, give `-f`. If the metadata has no datatype, wvt reads the file as `cf32` and
shows a warning.

The command-line options override all the values that wvt finds:

| Option | Value |
|---|---|
| `-f`, `--format FMT` | The sample format |
| `-r`, `--rate HZ` | The sample rate. SI suffixes are permitted, for example `2.4M` |
| `-c`, `--fc HZ` | The center frequency |
| `-H`, `--header BYTES` | The number of bytes to skip at the start of the file |
| `--swap-iq` | Swap I and Q |

While the file is open, you can change each value in the file row. The **Format** drop-down has
the sample format, the I/Q swap, the byte order and the header size.

### Large files

wvt memory-maps the file. Thus wvt can open files that are larger than the memory of the
computer. For files that are very large, load a part of the file only. Refer to
[Load a segment](segments.md).

wvt does not support a file that a different program is still writing. If the file gets shorter
while it is open (for example, a recorder that starts the file again), the part after the new
end shows as zeros, and the status bar shows a warning. To see the new length, open the file
again.

## Screen layout

| Area | Contents |
|---|---|
| **Menu bar** | The **File**, **View**, **Tools** and **Help** menus. Refer to [Menus](#menus). On the right: **TX ON**, **REC**, a running audio or export, and the **Source** (File or Device) |
| **File row** | The file path, **Format**, sample rate, center frequency, segment (start and end time to load), and file size |
| **Navigation row** | Buttons to go to the start or the end, and to move back or forward by 10 % or by one page. Zoom in, zoom out and zoom all. The span presets, the span and the start time. **Go to** has a sample index, *Add bookmark* and the bookmarks |
| **Overview strip** | The power over the full file or segment. The white box is the current view. Drag the box to move the view. Drag its edges to change the view size. Click a different point to go there. You can hide the strip in the **View** menu. wvt also hides it when the window is short |
| **Scrollbar** | Moves the view through the file. Drag the thumb, click the track to move one page, or turn the mouse wheel |
| **Left** | The waterfall settings: FFT size, window, dB range or auto. The ⚙ button has the overlap, averaging, rows, columns, colormap and peak hold. Below the settings are the spectrum of the current view, the waterfall, and its color bar |
| **Right** | The channel settings: on or off, offset, bandwidth, and *to peak*. The ⚙ button has the taps, decimation, squelch, maximum points and plot height. Below the settings are the I/Q, AM, FM and PM plots, which you can scroll. Each plot header has a show checkbox, a ⚙ button for the plot options, and the measurement readout. Thus no text covers the traces |
| **Status bar** | The **Snapshot...** button (refer to [Plot snapshots](snapshots.md)). The values at the pointer: time, sample number, frequency, dB, AM, FM and PM. The marker ΔT, 1/ΔT and ΔF. `GPU <model>` or `CPU`, the renderer, and `computing... NN%` while wvt calculates the plots (the percentage done) |

## Menus

| Menu | Items |
|---|---|
| **File** | Open file, export region, save plot snapshots, save file session (refer to [File sessions](sessions.md)), quit |
| **View** | Time markers, frequency markers, relative frequency, peak hold, the overview strip, the time plots to show, show everything |
| **Tools** | Signal generator, audio, use the GPU |
| **Help** | Keyboard and mouse, about wvt |

## Linked views

- All time axes have one range: the vertical axis of the waterfall and the horizontal axis of each
  plot on the right.
- The spectrum and the waterfall have one frequency range.
- When the pointer is on a plot, all the other plots show a crosshair at the same time or
  frequency.
- The time markers (T1 and T2) show in all plots. You can drag them in each plot. Press `Z` to
  zoom to the span between them.

## The spectrum of the view

The spectrum above the waterfall shows the average and the peak hold of all the waterfall rows in
the view. When you pan or zoom, wvt calculates the spectrum again. For the calculation, refer to
[Signal processing](signal-processing.md).

## The channel filter and the time plots

The channel filter selects one signal for the I/Q, AM, FM and PM plots.

- To enable the filter, press `C`, or tick **Channel** in the channel settings.
- To tune the channel, Ctrl+click the spectrum or the waterfall.
- To move the channel, drag its center line (cyan). To change the bandwidth, drag an edge line.
- **To peak** puts the channel on the strongest signal in the visible spectrum.

To start with the filter on, use `--chan OFFSET:BW`, for example `--chan 300k:150k`. To start with
a time range, use `--view T0:T1`, for example `--view 0.5:0.52`.

Each time plot has options. To see them, click its ⚙ button, or right-click the plot. For the
calculations, refer to [Signal processing](signal-processing.md).

## Bookmarks and markers

- Press `B` to add a bookmark at the view. **Go to** shows the bookmarks.
- Press `T` to show or hide the time markers. Press `F` to show or hide the frequency markers.
- The status bar shows the time between the markers (ΔT), its inverse (1/ΔT) and the frequency
  difference (ΔF).
