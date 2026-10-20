# Export a region

The export window writes a time range of the file, and as an option a frequency band, to a new
SigMF or raw file.

## Open the export window

Do one of these steps:

- Select **File → Export region…**.
- Press **Ctrl+E**.
- Right-click the waterfall (do not drag). Then select *Select a region to export…*, *Export this
  view…* or *Export the channel…*.

## Select the region

**Fill from** sets the region from one of these sources:

| Source | Time range | Band |
|---|---|---|
| **View** | The view | The full band |
| **Channel** | The view | The band of the channel filter |
| **Markers** | The time markers | The frequency markers |
| **Select…** | The box that you drag on the waterfall | The box that you drag on the waterfall |

You can also type or adjust each value. The start and the end are in seconds or in samples. The
band is a low and a high frequency, or a center frequency and a width.

To select a box on the waterfall:

1. Click **Select…**, or right-click the waterfall and select *Select a region to export…*.
2. Drag a box with the left mouse button. The view does not pan while you drag.
3. To cancel, press **Esc**.

A right-drag still zooms to a box.

While the export window is open, the waterfall shows the region:

- To move the region, drag the box.
- To change its size, drag its edges or corners.
- A full-band region shows as two time lines.
- The I/Q, AM, FM and PM plots show the time range as a shaded area.

## Full band or a band

**Full band:** wvt copies the samples without a change. The output has the same format, rate and
center frequency as the source. If the source is big-endian or has I and Q swapped, wvt writes the
output in the standard order.

**A band:** wvt makes a new recording of the band only. The window shows the output rate, center
frequency, taps, decimation, size and file name before you export. The output format is one of:

- `cf32`.
- `ci16`, at half the size. wvt clips values above full scale and shows the number of clipped
  samples.

## Write the file

1. Make sure that the file name and the format are correct.
2. Click **Export**.

The export runs in the background with a progress bar.

- **Cancel** stops the export and deletes the partial file.
- If the file exists, wvt replaces it only after you confirm.
- wvt checks the free disk space before it starts.
- The export stays in the loaded segment.

The default name carries the rate and the frequency, for example
`capture_12.5s_100.3MHz_200kSps`. Thus a raw export opens in wvt without options.

## Metadata

The SigMF metadata of the output has:

- The sample rate and the center frequency.
- `core:datetime`: the start time of the source plus the start of the range, if the source has a
  start time.
- A `wvt:extract` object with the source file, the sample range, the band and the filter.

## Command line

| Option | Function |
|---|---|
| `--extract-out PATH` | Write the region to `PATH` (without the extension), then stop |
| `--extract T0:T1` | The time range in seconds. The default is the full segment |
| `--extract-band LO:HI` | Only this band, in absolute Hz. Without this option, wvt copies the samples |
| `--extract-format cf32\|ci16` | The output format for a band. The default is `cf32` |
| `--extract-overwrite` | Replace output files that exist |
| `--export-window` | Open the export window with the `--chan` band, or with the view |

`--segment` limits the export. `--record-format sigmf|raw` sets the file type.

```sh
wvt cap.sigmf-meta --extract 10:20 --extract-out part                               # 10 s, copied
wvt cap.sigmf-meta --extract 10:20 --extract-band 100.2M:100.4M --extract-out fm    # a 200 kHz band
```

## How it works

For a band, wvt does these steps:

1. It mixes the center of the band to 0 Hz.
2. It applies a low-pass FIR filter. The passband is the band, flat to the edges. Each side has a
   25 % transition band. The stopband is approximately 74 dB down.
3. It decimates to `fs / D`, where `D = ⌊fs / (1.25 · BW)⌋`.

If a GPU is available, the filter runs on the GPU. The mixer phase is a function of the absolute
sample number. The self-check `build/extract_check` tests the copies, the band filter, the
metadata, the input checks and the cancel.

For the design, refer to [the region export design record](design/region-export-plan.md).
