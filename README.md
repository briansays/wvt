# wvt — IQ waveform visualization tool

An X11 desktop tool for looking at raw IQ recordings from software-defined radios.
On the left is a spectrum and waterfall. On the right are I/Q, AM, FM and PM plots,
all sharing one time axis. Pan or zoom any plot and every other plot follows.

## Build

Requirements: a C++20 compiler, CMake ≥ 3.16, X11 + GL headers/libraries.

- **Linux:**
  - Debian/Ubuntu: `sudo apt install build-essential cmake libx11-dev libgl1-mesa-dev`
  - Fedora: `sudo dnf install gcc-c++ cmake libX11-devel mesa-libGL-devel`
  - Arch: `sudo pacman -S base-devel cmake libx11 mesa`
- **macOS:** install [XQuartz](https://www.xquartz.org). The build links `/opt/X11/lib/libX11` and `libGL`, so the binary is a real X11 client.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/wvt recording.sigmf-meta
```

Dear ImGui 1.91.9 and ImPlot 0.16 are vendored in `third_party/`, so the build needs no network access.
The UI font is compiled into the binary.

## Running

```
wvt [options] [file]
  -f, --format FMT     cf32 | cf64 | ci16 | ci32 | ci8 | cu8   (suffix _be for big-endian)
  -r, --rate HZ        sample rate (SI suffixes OK: 2.4M, 250k)
  -c, --fc HZ          center frequency
  -H, --header BYTES   bytes to skip at the start of the file
      --swap-iq        swap I and Q
      --chan OFF:BW    start with the channel filter on, e.g. --chan 300k:150k
      --view T0:T1     start with this time range, e.g. --view 0.5:0.52
      --software       force the software renderer
      --scale S        UI scale (or env WVT_SCALE), useful on HiDPI
      --geometry WxH   initial window size
      --screenshot F   render once, write a PPM to F, exit (for automation)
```

wvt fills in format, sample rate and center frequency from, in order:
1. **SigMF** `.sigmf-meta` / `.sigmf-data` files
2. **gqrx** filenames (`gqrx_YYYYMMDD_HHMMSS_<fc>_<rate>_fc.raw`)
3. hints in the filename such as `2.4MSps` or `915MHz`
4. the file extension (`.cu8 .cs8 .cs16 .cf32 .cfile .raw …`)

Command-line options override anything detected. Every value can also be changed in the UI while the file is open.

The window renders with GLX/OpenGL when the X server offers it. Otherwise it falls back to a
built-in software rasterizer, so it also works over `ssh -X`, in Xvfb, and in VNC sessions without GL.

## Screen layout

| Area | Contents |
|---|---|
| **Toolbar row 1** | File path, sample format, sample rate, center frequency, header bytes, I/Q swap, endianness |
| **Toolbar row 2** | Start / back a page / back 10% / forward 10% / forward a page / end, zoom in/out/all, span presets, start-time and span entry boxes, jump to sample index, bookmarks, time and frequency markers, relative-frequency axis labels |
| **Overview strip** | Power over the whole file. The white box is the current view: drag it to move, drag its edges to resize, or click anywhere to jump there |
| **Scrollbar** | Scrolls the view through the file (drag the thumb, click the track to move a page, or use the wheel) |
| **Left** | Waterfall settings (FFT size, window, overlap, averaging, rows/cols, dB range/auto, colormap) above the spectrum of the current view, the waterfall, and its color bar |
| **Right** | Channel settings, then scrollable I/Q, AM, FM and PM plots, each with its own settings and a readout of measurements |
| **Status bar** | Values under the cursor (t, sample #, f, dB, AM/FM/PM), marker ΔT, 1/ΔT and ΔF, renderer, and a busy indicator |

### Linked views
- Every time axis shares one range: the waterfall's vertical axis and the horizontal axis of each right-hand plot.
- The spectrum and waterfall share one frequency range.
- Hovering any plot draws a crosshair at the same time or frequency on all the others.
- Time markers (T1/T2) appear in every plot and can be dragged from any of them. Press `Z` to zoom to the span between them.

### Mouse and keyboard
| Action | Input |
|---|---|
| Pan | Left-drag |
| Zoom | Mouse wheel (hover an axis to zoom only that axis) |
| Zoom to a region | Right-drag a box (Alt / Shift limit it to one axis) |
| Fit to data | Double-click |
| Tune the channel | Ctrl+click the spectrum or waterfall, or drag the cyan lines (center line moves it, edge lines change bandwidth) |
| Pan the view | `←` `→` (10%), `PgUp` `PgDn` (one page) |
| Zoom the time axis | `+` / `-` |
| Jump to start / end | `Home` / `End` |
| Show everything | `A` |
| Toggle markers | `T` time markers, `F` frequency markers |
| Toggle channel filter | `C` |
| Add bookmark | `B` |

## Signal processing

- **Waterfall:** windowed FFTs with fftshift, in dBFS. The FFT step adapts so the view fits in at most *Rows* rows. Adjacent bins are max-pooled down to *Cols* columns so narrow signals don't disappear. The spectrum plot shows the average and peak-hold over the rows in view.
- **Channel** (optional): mixes the chosen offset down to 0 Hz, applies a windowed-sinc FIR low-pass (Blackman), then decimates. Auto settings: decimation = ⌊Fs / 2·BW⌋ and taps ≈ 12·Fs / BW. The mixer phase is tied to the absolute sample number, so results don't shift as you pan.
- **AM:** |x|, as linear, dB, or normalized m(t) = |x| / mean − 1. Modulation index m = (max − min) / (max + min).
- **FM:** instantaneous frequency arg(x[n]·x*[n−1])·Fs / 2π, either absolute (offset from the capture center) or relative to the channel center. The readout shows mean, peak deviation and rms deviation.
- **PM:** phase, either wrapped, unwrapped, or unwrapped with the best-fit line removed (this cancels any leftover carrier offset). Radians or degrees.
- **Smoothing:** a moving average for each of AM, FM and PM.
- **Squelch:** blanks FM and PM wherever smoothed power is below the threshold.
- **Large files:** files are memory-mapped. Views up to *Max pts* samples (and ~32 M input samples) are demodulated at full resolution. Wider views switch to **SPARSE** mode, which demodulates a short block per screen pixel. All plots show exact per-pixel min/max envelopes.
- **Background work:** each computation runs on its own worker thread. A new request replaces the old one, so editing a setting shows its effect right away and the UI never blocks.

## Test signal

`build/iqgen out.sigmf-data --fmt ci16` writes a 2 s, 2 MSps capture containing:

| Offset | Signal |
|---|---|
| +300 kHz | FM, 1 kHz tone, 50 kHz deviation, slow fade |
| −250 kHz | AM, 400 Hz tone, 60% depth |
| −600 kHz | PM, 2 rad sine, plus 90° steps every 20 ms |
| +650 kHz | 2-FSK bursts |
| +100 kHz | weak CW tone |
| — | a chirp from 1.0 to 1.4 s |

Try `wvt out.sigmf-meta --chan 300k:150k --view 0.5:0.505`.

## Source layout

| File | Role |
|---|---|
| `src/main.cpp` | CLI parsing, ImGui/ImPlot setup, event loop |
| `src/platform_x11.*` | Xlib window, input, clipboard, GLX or software presentation |
| `src/sw_renderer.*` | Software rasterizer for ImGui draw data |
| `src/app.*` | UI: toolbar, linked plots, parameter panels |
| `src/engine.*` | Worker threads; waterfall, channelizer/demodulator, overview |
| `src/dsp.*` | FFT, windows, FIR design, parallel-for, smoothing |
| `src/iq_file.*` | mmap reader for all sample formats; SigMF and filename detection |
| `src/ui_util.*` | SI-unit inputs and formatters, scrollbar widget, style |
| `tools/iqgen.cpp` | Synthetic test-signal generator |
