# Plan: export a region of a file (issue #18)

Branch: `18-region-export`, from `main` at `8896568`, rebased on `2c3c7cd` (after the GPU
merge, PR #20).
Status: **steps 1–9 implemented** (2026-10-20). The GUI still needs a test on screen (§8).

## 1. Goal

Issue #18 asks for these items:

1. Export a smaller part of a data file to a new file.
2. A new **panel** (window) where the user sets the time (or sample) range and the
   frequency range of the export.
3. Two ways to fill the panel:
   - from the current **channel** setup;
   - from a **selection on the waterfall**: right-click the waterfall, choose an export
     mode, and drag a box with a crosshair cursor.
4. The values go into the panel, and the user checks them before the file is written.

## 2. Starting point (`main`)

| Piece | Where | Use for the export |
|---|---|---|
| Channel filter | `processRange`, `src/engine.cpp` | Mix down with the phase from the absolute sample index, Blackman windowed-sinc FIR, compute only the kept (decimated) outputs. Same method for the export, but streamed in chunks over a range of any length. |
| Channel settings | `demod_.chanEnable`, `chanOffset`, `chanBw` (`src/app.cpp`) | "From channel". |
| Time and frequency markers | `tm1_`, `tm2_`, `fm1_`, `fm2_` | "From markers". |
| Time segment (#16) | `tBegin()`, `tEnd()`, `segS0_`, `segS1_` | The export stays inside the loaded segment. |
| SigMF writer | `Recorder` (`src/recorder.*`): the stream comes through a `SampleRing`; the name is `PREFIX_<date>_<time>_<fc>_<rate>`; `core:datetime`, captures and annotations | `FileGenJob` (`src/siggen_out.cpp`) already uses it to write a file from a background job, with progress and cancel. |
| Background job with progress | `FileGenJob`, `WavExportJob` | The same pattern: thread, progress, cancel, status. |
| Headless export | `--audio-out` (`runAudioExport`, `src/main.cpp`) | The same pattern for a test without a display. |
| Draggable rectangle | `ImPlot::DragRect` (overview strip, `src/app.cpp`) | The selection box on the waterfall. |
| Right mouse on the waterfall | ImPlot defaults: **right-drag = box zoom** (in the README), right-click = ImPlot's own context menu | The new menu must not break the box zoom. |
| SigMF reader | `guessMeta` (`src/iq_file.cpp`) | Reads only format, rate and frequency. No `core:datetime`, no annotations. |
| Separate windows | Audio (`auOpen_`, Ctrl+D), signal generator (Ctrl+G) | The export window works the same way. |

## 3. Design decisions

| Topic | Decision | Reason |
|---|---|---|
| What is exported | A time range `[t0, t1)` and, optionally, a frequency band `[f_lo, f_hi]`. | The issue asks for time / sample and frequency. |
| Full band (no frequency selection) | Copy the raw bytes of the sample range unchanged: same sample format, same rate, same center frequency. The header is not copied. | Lossless and fast. This is the common "cut a piece out of a big file" case. |
| Frequency band selected | Mix the band center down to 0 Hz, low-pass filter, and decimate. The output center frequency is `fc + band center`. The output rate is `fs / D`. | A smaller file at a lower rate. This is what "frequency range" means for IQ data. |
| Filter for the export | The band is the **passband**. A transition band of 25 % of the bandwidth on each side. `D = max(1, ⌊fs / (1.25 · BW)⌋)`, so the transition band does not alias into the passband. Blackman windowed sinc with `N ≈ 5.5 · fs / (0.25 · BW)` taps (cutoff in the middle of the transition band). | The demod filter (`taps ≈ 12 · fs / BW`, cutoff at BW / 2) attenuates the band edges by 6 dB. An export must keep the whole selected band. The Blackman window gives about 74 dB of stopband. |
| Cost of narrow bands | No limit on the number of taps. The FIR computes only the kept outputs, so the work per input sample is `N / D ≈ 27` multiply-adds for any bandwidth. | A narrow band in a wide file (20 MS/s to 10 kHz: `D = 1600`, `N ≈ 44 000`) costs the same per input sample as a wide band. |
| Output rate | `fs / D`, an integer decimation. The window shows the exact rate. | A rational resampler to a chosen rate is more work. It can come later if needed. |
| Output format | Full band: the source format. Band selected: `cf32` (default) or `ci16`. With `ci16` the export scales by 32768 and counts clipped samples (WARN). | The filter output is float. `ci16` halves the size. |
| File type | SigMF (default) or raw, as for recordings. A raw name carries the rate and frequency, so wvt can open it with no options. | Same as the recorder. |
| Metadata (SigMF) | `core:sample_rate`, `core:datatype`, `core:frequency` (in the capture), `core:datetime` = the source start time + t0 when the source SigMF has one, `core:description`. A `wvt:extract` object with the source file name, sample range, band, D, taps and filter. | The export can be traced back to its source. |
| Source annotations | Not copied (the reader does not parse them). | Can come later with the SigMF reader work. |
| Writer | Reuse `Recorder` through a `SampleRing`, as `FileGenJob` does. Add `RecordConfig::exactPath`: the user's file name instead of `PREFIX_<date>_…`. | One SigMF writer. `exactPath` keeps the user's name. |
| Range limits | Time inside the loaded segment (#16). Band inside `fc ± fs / 2`. BW at least `fs / 100 000` (D at most 80 000). | Same limits as the rest of the view. |
| Background job | `ExtractJob` (`src/extract.*`): its own thread, chunks of about 4 M input samples, progress, cancel. Cancel **deletes** the partial files. An existing file is not overwritten unless the user confirms. | A cancelled export is not a usable file. |
| Disk space | Before starting, check the free space (`statvfs`) against the size estimate. Too little: an error, nothing is written. | Large exports can fill a disk. |
| GPU (#3, PR #20) | The export filter is CPU code in `src/extract.cpp`. When #20 is merged, it can use `ChanJob` / `gpuChannelize`. | #20 is not merged yet; #18 must not wait for it. |
| Logging (#14) | INFO at start (source, range, band, D, taps, output) and at the end (file, size, time). WARN for clipping and for clamped values. ERROR for failures, once, by the code that shows them. | `docs/verbose-logging-plan.md`. |

### The export window

Opened from a new **Export** button in toolbar row 1 (next to **Audio**), with **Ctrl+E**, or
from the waterfall menu.

| Row | Contents |
|---|---|
| Time | **Start** / **End** in seconds, or in samples (switch). The length and the sample count. |
| Band | **Full band** checkbox. When off: **Low** / **High**, or **Center** / **BW** (switch). Absolute frequency, or relative to fc with **Rel. freq**. |
| Fill from | **View** · **Channel** (offset and BW; time = the view) · **Markers** (T1 / T2, F1 / F2) · **Select on waterfall** |
| Output | Format (`source` / `cf32` / `ci16`), SigMF / raw, path, as a text field (default: next to the source file, `<name>_<t0>s_<fc>_<rate>`). |
| Result | Output rate, D, taps, number of samples, file size, estimated time. Errors in red (for example "the band is outside the capture"). |
| Actions | **Export**, then a progress bar, **Cancel**, and the result line with the path. |

### The selection on the waterfall

- **Right-click** (press and release without moving) on the waterfall opens a wvt menu:
  - **Select a region to export…**: crosshair mode;
  - **Export this view**: fill the window with the view (time and full band);
  - **Export the channel**: fill it from the channel.
- **Right-drag** keeps the box zoom. The waterfall gets `ImPlotFlags_NoMenus`. ImPlot's own
  menu then no longer opens on the waterfall: it has only axis settings, which the toolbar and
  double-click already give.
- **Crosshair mode**: the cursor is a crosshair. A left-drag draws the box, and panning is off
  until the mouse is released. Esc cancels. The box fills Start / End / Low / High and opens
  the export window.
- While the export window is open, the region shows as a **draggable rectangle**
  (`ImPlot::DragRect`) on the waterfall. Its edges and the panel values stay in step. The
  time range also shows as a shaded band on the I/Q, AM, FM and PM plots.

### Command line

`--extract-out PATH --extract T0:T1 [--extract-band LO:HI] [--extract-format cf32|ci16]
[--record-format sigmf|raw]`. Headless, with exit status 0 on success. `--segment` limits it
the same way as in the window. It is for tests and scripts.

## 4. Implementation steps

1. **Extraction core** (`src/extract.h`, `src/extract.cpp`).
   - `ExtractConfig` (source, sample range, band, format, file type, path) and
     `ExtractPlan` (D, taps, cutoff, output rate, sample count, bytes, or the error).
   - `planExtract(cfg)`: all the checks of §3, and no I/O, so the window can call it every
     frame.
   - Full band: chunked byte copy. Band: chunked mix + FIR + decimate with the phase from
     the absolute index (each chunk reads its own input with the filter overlap, as
     `processRange` does).
   - Output through `Recorder` with `exactPath`. `wvt:extract` metadata.
2. **Recorder** (`src/recorder.*`): `RecordConfig::exactPath` and the start datetime from the
   source.
3. **Background job**: `ExtractJob` (progress, cancel deletes the files, status) and the
   disk-space check.
4. **Command line**: `--extract-out`, `--extract`, `--extract-band`, `--extract-format`, and
   the usage text.
5. **Self-test** `tools/extract_check.cpp` (§5).
6. **Export window** (`src/app_export.cpp`): the window of §3, **Export** button, Ctrl+E, the
   "Fill from" buttons, progress, the result.
7. **Waterfall**: `NoMenus`, the right-click menu, crosshair mode, the `DragRect` and the time
   bands on the right-hand plots.
8. **Source start time**: `guessMeta` also reads `core:datetime` of the first capture, for the
   exported `core:datetime`.
9. **README**: the window, the waterfall menu, the options, the file table.

## 5. Testing

| Test | Check |
|---|---|
| `extract_check`: full band | A cf32 / ci16 / cu8 file with a header: the output bytes are exactly the source bytes of the range. The SigMF metadata has the same rate, format and frequency. |
| `extract_check`: band | Two tones, one inside and one outside the band. Out: the inside tone at `f - band center`, level within 0.1 dB. The outside tone at least 70 dB down. Rate `fs / D`. |
| `extract_check`: edges of the band | Tones at 0 %, 50 % and 100 % of the band: all within 0.1 dB. |
| `extract_check`: chunks | One export in one chunk and in many chunks: the same samples. |
| `extract_check`: checks | Band outside the capture, end before start, range outside the segment, too narrow: an error, no file. |
| `extract_check`: ci16 | Scaling and the clip count. |
| `extract_check`: cancel | The partial files are deleted. |
| Headless | `wvt FILE --extract 1:2 --extract-band 100.2M:100.4M --extract-out x` writes the file. wvt opens it, and the tone shows at the right frequency. |
| GUI (**you must do this**) | The window, each "Fill from" button, the right-click menu, right-drag still zooms, crosshair mode, Esc, the rectangle and its edges, progress and Cancel, the overwrite question, an export of a 7 GB file. |
| Regression | All self-tests pass. Without the new options, the output and the log are the same as `main`. |

## 6. Decisions needed

1. **Full band** exports copy the bytes unchanged. Do you agree, or should the output always
   be one format (for example cf32)?
2. **Output rate**: integer decimation only (rate `fs / D`), or also an exact rate that you
   type (needs a rational resampler)?
3. **Filter**: passband = the selected band and a 25 % transition band (larger file, flat
   band). Or the demod filter (smaller file, edges 6 dB down)?
4. **Right-click on the waterfall**: replace ImPlot's own menu there with the wvt menu, and
   keep right-drag for the box zoom?
5. **Cancel** deletes the partial file. Agreed?
6. **GPU**: CPU first, and the GPU after #20 is merged?
7. **Range**: inside the loaded segment only, or anywhere in the file?

## 7. Decisions (2026-10-20)

The user said to go with the recommendations:

1. **Full band:** the bytes are copied unchanged.
2. **Output rate:** integer decimation only (`fs / D`).
3. **Filter:** the band is the passband, with a 25 % transition band.
4. **Right-click on the waterfall:** wvt's menu replaces ImPlot's; right-drag keeps the box
   zoom.
5. **Cancel** deletes the partial file.
6. **GPU:** #20 is merged, so the export uses `ChanJob` / `gpuChannelize` from the start, with
   `channelizeCpu` as the fallback.
7. **Range:** inside the loaded segment only.

## 8. Implementation notes

### Files

| File | Change |
|---|---|
| `src/extract.h`, `src/extract.cpp` | `ExtractConfig`, `ExtractPlan`, `planExtract`, `defaultExtractPath`, `ExtractJob`. |
| `src/app_export.cpp` | The export window, the fill buttons, the waterfall menu, the crosshair selection, the region on the plots. |
| `src/app.h`, `src/app.cpp` | Export state, the **Export** button, Ctrl+E, `NoMenus` and the axis lock on the waterfall, the region in the time plots, `fileStart_`, `busy()` includes the export. |
| `src/recorder.h`, `src/recorder.cpp` | `RecordConfig::exactPath`, `RecordConfig::startTime` (negative: no `core:datetime`), `rawFileExt`. |
| `src/iq_file.h`, `src/iq_file.cpp` | `FileMetaGuess::startTime` from `core:datetime`; `parseIsoTime`. |
| `src/main.cpp` | `--extract-out`, `--extract`, `--extract-band`, `--extract-format`, `--extract-overwrite`, `--export-window`; usage text. |
| `tools/extract_check.cpp` | Self-test (§5). |
| `README.md` | "Export a region to a new file", the options, the layout and key tables, the file table. |

### Differences from the plan

- **Filter length:** `N = ⌈5.5 · fs / (0.25 · BW)⌉ | 1` and cutoff `0.625 · BW / fs`, as planned.
  In the test, the band edges are 0.002 dB down and a tone outside the band is 92.8 dB down.
- **Full band, big-endian or swapped I/Q:** the copy puts the bytes in the standard order
  (byte and I/Q swaps only), because the SigMF and raw names describe little-endian data in
  I/Q order.
- **No source start time:** the export leaves out `core:datetime` instead of writing the time of
  the export.
- **Crosshair:** the X11 layer does not change the system cursor, so selection mode draws a
  crosshair and a hint inside the waterfall.
- **The region box:** ImPlot's `DragRect` moves the whole box when you drag inside it, so the
  view does not pan inside the box while the export window is open. Pan outside it.
- **`--export-window`:** opens the window at start (the `--chan` band, or the view). It is used
  for the screenshots below.
- **Default name:** `<source name>_<t0>s_<fc>MHz_<rate>Sps`. The rate and frequency already in
  the source name are removed, because `guessMeta` takes the first match.
- **Window position:** on the right, left of the audio window when that is open.

### Verification (macOS)

- The build has no warnings. `extract_check` (36 checks), `gpu_check`, `segment_check`,
  `siggen_check`, `audio_check` and `log_check` pass.
- `--help` differs from `main` only by the new lines. A signal-generator file (`--gen-out`)
  has the same data as `main`, and the same metadata apart from `core:datetime`.
- Headless: `iqgen` capture, `--extract 0.5:1.5 --extract-band 100.25M:100.35M` gives 125 000
  samples at 125 kS/s, 100.3 MHz; an NFM WAV export of that file plays the tone. A second
  export to the same name fails with status 2.
- Screenshots (Xvfb in a container, `--export-window`): the window with the view (full band,
  the two time lines on the waterfall) and with `--chan 300k:150k` (band 100.225–100.375 MHz,
  200 kS/s, 295 taps, decimation 10, the box on the waterfall).

### GUI: still to test on screen

The right-click menu, right-drag still zooming, select mode (drag, Esc), moving and resizing
the box, the shaded range on the time plots, each "Fill from" button, Export with progress and
Cancel, the "Replace?" question, and an export of a large file.
