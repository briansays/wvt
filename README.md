# wvt — IQ waveform visualization tool

An X11 desktop tool for looking at raw IQ recordings from software-defined radios.
On the left is a spectrum and waterfall. On the right are I/Q, AM, FM and PM plots,
all sharing one time axis. Pan or zoom any plot and every other plot follows.

It can also stream live from an **RTL-SDR v3** or a **USRP B200 / B205mini**, showing
the spectrum and a scrolling waterfall in real time, and record the stream to SigMF or
raw IQ files that open straight back up in wvt. See [Live devices](#live-devices).

<img width="2125" height="1407" alt="image" src="https://github.com/user-attachments/assets/71d188af-cc07-4a0c-93ef-36d082b5829b" />

## Roadmap 

| Version | Features | | 
|---|---|---| 
| 1.0 | Receive via UHD and RTL-SDR v3 | ✅ | 
| 1.1 | Signal Generator Support via UHD | | 
| 1.2 | Support for Signal Hound SDRs | | 
| 1.3 | Support for EPIQ SDRs | | 

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

### Optional: device support

Live streaming needs librtlsdr (RTL-SDR) and/or UHD (USRP). CMake finds them automatically and
reports what it enabled (`wvt: RTL-SDR support enabled ...`). Without either library wvt still
builds, with the file viewer, recording and a built-in signal simulator.

| | RTL-SDR | UHD (USRP B200/B205) |
|---|---|---|
| Debian/Ubuntu | `sudo apt install librtlsdr-dev` | `sudo apt install libuhd-dev uhd-host` |
| Fedora | `sudo dnf install rtl-sdr-devel` | `sudo dnf install uhd-devel` |
| Arch | `sudo pacman -S rtl-sdr` | `sudo pacman -S libuhd` |
| macOS | `brew install librtlsdr` | `brew install uhd` |

To require a backend (fail if it's missing) or leave it out:
`cmake -S . -B build -DWVT_WITH_RTLSDR=ON -DWVT_WITH_UHD=OFF` (each takes `AUTO`, `ON` or `OFF`).

wvt uses UHD's C API, so Boost headers aren't needed to build it.

### Device setup

**RTL-SDR v3 on Linux**
- Stop the kernel's DVB-T driver from claiming the dongle:
  `echo 'blacklist dvb_usb_rtl28xxu' | sudo tee /etc/modprobe.d/blacklist-rtlsdr.conf`, then replug
  (or `sudo modprobe -r dvb_usb_rtl28xxu`).
- Allow non-root access: the `rtl-sdr` package installs a udev rule on most distributions. If
  `rtl_test` only works as root, add your user to the `plugdev` group and replug.

**USRP B200 / B205mini**
- Download the FPGA/firmware images once: `sudo uhd_images_downloader`
  (macOS/Homebrew: `uhd_images_downloader` without sudo).
- On Linux, install UHD's udev rule (`uhd-host` does this on Debian/Ubuntu) so it works without root.
- Use a USB 3 port for rates above about 8 MS/s. Run `uhd_find_devices` to check the device is seen.

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

live devices (-c and -r set the tuning):
      --device DEV     rtlsdr[:INDEX|SERIAL] | uhd[:ARGS] | sim
      --gain DB|auto   receiver gain (default 30 dB)
      --antenna NAME   USRP antenna (RX2 or TX/RX)
      --bandwidth HZ   USRP analog bandwidth (default: automatic)
      --bias-tee       RTL-SDR: power an LNA / active antenna over the coax
      --direct-sampling I|Q  RTL-SDR: HF direct sampling (v3: Q)
      --ppm N          RTL-SDR frequency correction
      --list-devices   print attached devices and exit

recording (needs --device):
      --record PREFIX  record to PREFIX_<date>_<time>_<fc>_<rate>.<ext>
      --record-format sigmf|raw     (default sigmf)
      --record-sample native|cf32   (default native: cu8 for RTL-SDR, ci16 for USRP)
      --record-duration T           stop after T seconds
      --no-gui         record without opening a window (Ctrl+C stops)
```

wvt fills in format, sample rate and center frequency from, in order:
1. **SigMF** `.sigmf-meta` / `.sigmf-data` files
2. **gqrx** filenames (`gqrx_YYYYMMDD_HHMMSS_<fc>_<rate>_fc.raw`)
3. hints in the filename such as `2.4MSps` or `915MHz`
4. the file extension (`.cu8 .cs8 .cs16 .cf32 .cfile .raw …`)

Command-line options override anything detected. Every value can also be changed in the UI while the file is open.

The window renders with GLX/OpenGL when the X server offers it. Otherwise it falls back to a
built-in software rasterizer, so it also works over `ssh -X`, in Xvfb, and in VNC sessions without GL.

## Live devices

Choose **Source → Device** in the toolbar (or start with `--device`). Pick the device, set the
center frequency, sample rate and gain, and press **Start**. Only one device streams per wvt window.

```sh
wvt --device rtlsdr -c 100.1M -r 2.4M --gain 30          # RTL-SDR v3, FM broadcast band
wvt --device rtlsdr -c 7.1M -r 2.4M --direct-sampling Q  # RTL-SDR v3, HF
wvt --device uhd:type=b200 -c 915M -r 20M --gain 40      # USRP B200 / B205mini
wvt --device sim -c 100M -r 2.4M                         # built-in simulator, no hardware
```

| | RTL-SDR v3 | USRP B200 / B205mini |
|---|---|---|
| Tuning | 24 MHz - 1.766 GHz; HF below that with direct sampling (Q) | 70 MHz - 6 GHz |
| Sample rate | 225-300 kS/s or 0.9-3.2 MS/s (2.4 MS/s or less is reliable) | up to 56 MS/s (USB 3) |
| Gain | 0-49.6 dB in tuner steps, or AGC | 0-76 dB, or AGC |
| Extras | bias-tee, PPM correction | antenna RX2 / TX/RX, analog bandwidth |
| Samples | cu8 | ci16 (sc16) |

- **Frequency and gain** change while streaming. Changing the rate or a device option restarts the stream.
- The **waterfall** scrolls with the newest row at the top; its vertical axis is seconds ago.
  *Rows/s* sets the scroll speed and *History* how many rows are kept. *Avg* `auto` averages every
  sample received since the previous row (up to 20 MS/s; the status bar shows what fraction the
  FFT sees). Lower it to see short bursts more sharply.
- The **spectrum** shows a running average (*Spec avg* sets its time constant in rows) and a peak
  hold (*reset* restarts it).
- The **status bar** shows the device, actual rate and frequency (devices may adjust what you asked
  for), and any overflows: samples the device produced but the computer didn't receive in time.
- The time-domain plots (I/Q, AM, FM, PM) are not available while live. Record, then use
  **Open recording** to analyze the capture in file mode.

### Recording

The second toolbar row records the live stream. **Record to** is a path prefix: wvt appends the
date, time, frequency and rate, e.g. `~/wvt-recordings/wvt_20261016_142233_915.000MHz_2.4MSps.sigmf-data`,
and creates missing folders.

- **SigMF** (default) writes `.sigmf-data` and `.sigmf-meta`. The metadata holds the sample format,
  rate, device, start time and frequency. Retuning while recording starts a new capture segment at
  that sample, and every gap (dropped samples) gets an annotation saying where and how many. The
  metadata is rewritten as the recording goes, so it's usable even if wvt is killed.
- **Raw** writes bare samples (`.cu8`, `.cs16` or `.cf32`) with the rate and frequency in the file
  name, where wvt's metadata detection finds them. A raw file can't record a frequency change, so
  tuning is locked while recording raw.
- **Samples**: *native* saves exactly what the device sends (cu8 for RTL-SDR, ci16 for USRP;
  smallest and lossless). *cf32* converts to complex float32 for tools that only read float.
- The sample rate is locked while recording (one file has one rate). **Limit** stops after a set time.
- Storage has to keep up: 2.4 MS/s cu8 is 4.8 MB/s, but a B205 at 56 MS/s ci16 is 224 MB/s.
  The **buffer** figure shows how far the writer lags; if it fills, samples are dropped and annotated.
- **Open recording** stops the device and opens the last recording in file mode.

`--no-gui` records without a window, e.g. on a headless machine:

```sh
wvt --device uhd:type=b200 -c 915M -r 10M --record ~/caps/ism --record-duration 30 --no-gui
```

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
| `src/app_live.cpp` | Live mode UI: device and recording toolbar, live spectrum and scrolling waterfall |
| `src/live.*` | Live engine: owns the device, sample ring, recorder and spectrum thread |
| `src/sample_ring.h` | Ring buffer between the device thread, the spectrum thread and the recorder |
| `src/recorder.*` | SigMF / raw recorder |
| `src/sdr_device.*` | Device interface, driver list, sample-rate checks |
| `src/sdr_rtlsdr.cpp` | RTL-SDR backend (librtlsdr) |
| `src/sdr_uhd.cpp` | USRP backend (UHD C API) |
| `src/sdr_sim.cpp` | Simulated device with synthetic signals |
| `tools/iqgen.cpp` | Synthetic test-signal generator |
