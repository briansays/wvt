# wvt — IQ waveform visualization tool

An X11 desktop tool to examine raw IQ recordings from software-defined radios.
The left side shows a spectrum and a waterfall. The right side shows I/Q, AM, FM and PM plots
that share one time axis. When you pan or zoom one plot, all the other plots follow.

<img width="2125" height="1407" alt="image" src="https://github.com/user-attachments/assets/71d188af-cc07-4a0c-93ef-36d082b5829b" />

wvt can also stream live from an **RTL-SDR v3** or a **USRP B200 / B205mini**. It shows the
spectrum and a scrolling waterfall in real time. It can record the stream to SigMF or raw IQ
files, and wvt opens these files directly. See [Live devices](#live-devices).

A **signal generator** window makes CW and pulsed waveforms: chirps, Barker codes, staggered or
jittered PRIs, bursts and impairments. It can also play back recordings as canned waveforms. The
generator writes its waveforms to files, streams them into the live view (loopback), or transmits them
with a USRP B200 / B205mini. See [Signal generator](#signal-generator).

## Roadmap 

| Version | Features | | 
|---|---|---| 
| 1.0 | Receive via UHD and RTL-SDR v3 | ✅ | 
| 1.1 | Signal Generator Support via UHD | ☑️ | 
| 1.2 | Support for Signal Hound SDRs | ⭕️ | 
| 1.3 | Support for EPIQ SDRs | ⭕️ | 

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

The repository includes Dear ImGui 1.91.9 and ImPlot 0.16 in `third_party/`, so the build needs no
network access. The binary contains the UI font.

### Optional: device support

Live streaming needs librtlsdr (for RTL-SDR), UHD (for USRP), or both. CMake finds them
automatically and reports the support that it enabled (`wvt: RTL-SDR support enabled ...`).
Without these libraries, wvt still builds. It then has the file viewer, recording and a built-in
signal simulator.

| | RTL-SDR | UHD (USRP B200/B205) |
|---|---|---|
| Debian/Ubuntu | `sudo apt install librtlsdr-dev` | `sudo apt install libuhd-dev uhd-host` |
| Fedora | `sudo dnf install rtl-sdr-devel` | `sudo dnf install uhd-devel` |
| Arch | `sudo pacman -S rtl-sdr` | `sudo pacman -S libuhd` |
| macOS | `brew install librtlsdr` | `brew install uhd` |

To make a backend necessary (CMake fails if the library is missing) or to leave it out, use:
`cmake -S . -B build -DWVT_WITH_RTLSDR=ON -DWVT_WITH_UHD=OFF`. Each option takes `AUTO`, `ON` or `OFF`.

wvt uses the UHD C API, so you do not need Boost headers to build it.

### Device setup

**RTL-SDR v3 on Linux**
- Stop the DVB-T driver of the kernel from claiming the dongle:
  `echo 'blacklist dvb_usb_rtl28xxu' | sudo tee /etc/modprobe.d/blacklist-rtlsdr.conf`.
  Then replug the dongle, or run `sudo modprobe -r dvb_usb_rtl28xxu`.
- Allow access without root. On most distributions, the `rtl-sdr` package installs a udev rule.
  If `rtl_test` works only as root, add your user to the `plugdev` group and replug the dongle.

**USRP B200 / B205mini**
- Download the FPGA and firmware images one time: `sudo uhd_images_downloader`.
  On macOS with Homebrew, run `uhd_images_downloader` without sudo.
- On Linux, install the UHD udev rule, so that UHD works without root. On Debian/Ubuntu, the
  `uhd-host` package installs this rule.
- Use a USB 3 port for rates above approximately 8 MS/s. To check that the computer finds the
  device, run `uhd_find_devices`.

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

signal generator (-c sets the center frequency, -r the sample rate):
      --siggen         open the signal generator window
      --gen cw|pulsed|playback   waveform mode (also opens the window)
      --offset HZ  --amp DBFS  --drift HZ_PER_S
      --pri S | --prf HZ   --pw S   --edge rect|linear|cos|gauss  --rise S  --fall S
      --stagger S,S,..  --jitter PCT  --seed N  --burst N[:GAP]  --delay S
      --lfm up|down|tri:BW  --barker N
      --play FILE  --play-rate HZ  --play-segment START:LEN  --play-gap S  --play-once
      --snr DB  --dc I:Q  --iq GAIN_DB:PHASE_DEG  --linewidth HZ
      --gen-out PREFIX write a file and exit (container via --record-format)
      --gen-sample cf32|ci16|ci8|cu8   --gen-duration T | --gen-pulses N   --gen-annotate
      --tx             TRANSMIT with a USRP until Ctrl+C (or --gen-duration)
      --tx-gain DB  --tx-args ARGS
```

wvt gets the format, sample rate and center frequency from these sources, in this order:
1. **SigMF** `.sigmf-meta` / `.sigmf-data` files
2. **gqrx** filenames (`gqrx_YYYYMMDD_HHMMSS_<fc>_<rate>_fc.raw`)
3. hints in the filename such as `2.4MSps` or `915MHz`
4. the file extension (`.cu8 .cs8 .cs16 .cf32 .cfile .raw …`)

Command-line options override all detected values. While the file is open, you can also change
each value in the UI.

The window uses GLX/OpenGL when the X server supports it. If not, wvt uses its built-in software
rasterizer. Thus wvt also works over `ssh -X`, in Xvfb, and in VNC sessions without GL.

## Live devices

Select **Source → Device** in the toolbar, or start wvt with `--device`. Select the device, set
the center frequency, sample rate and gain, and press **Start**. Each wvt window streams from one
device only.

```sh
wvt --device rtlsdr -c 100.1M -r 2.4M --gain 30          # RTL-SDR v3, FM broadcast band
wvt --device rtlsdr -c 7.1M -r 2.4M --direct-sampling Q  # RTL-SDR v3, HF
wvt --device uhd:type=b200 -c 915M -r 20M --gain 40      # USRP B200 / B205mini
wvt --device sim -c 100M -r 2.4M                         # built-in simulator, no hardware
```

| | RTL-SDR v3 | USRP B200 / B205mini |
|---|---|---|
| Tuning | 24 MHz - 1.766 GHz. HF below 24 MHz with direct sampling (Q) | 70 MHz - 6 GHz |
| Sample rate | 225-300 kS/s or 0.9-3.2 MS/s (2.4 MS/s or less is reliable) | up to 56 MS/s (USB 3) |
| Gain | 0-49.6 dB in tuner steps, or AGC | 0-76 dB, or AGC |
| Extras | bias-tee, PPM correction | antenna RX2 / TX/RX, analog bandwidth |
| Samples | cu8 | ci16 (sc16) |

- You can change **frequency and gain** while the device streams. A change to the rate or to a
  device option restarts the stream.
- The **waterfall** scrolls with the newest row at the top. Its vertical axis shows seconds ago.
  *Rows/s* sets the scroll speed, and *History* sets how many rows wvt keeps. *Avg* `auto` averages
  all samples received since the previous row, up to 20 MS/s. The status bar shows the fraction of
  the samples that the FFT sees. Lower *Avg* to see short bursts more sharply.
- The **spectrum** shows a running average and a peak hold. *Spec avg* sets the time constant of
  the average, in rows. *reset* restarts the peak hold.
- The **status bar** shows the device, the actual rate and frequency, and all overflows. Devices
  may adjust the rate and frequency that you set. An overflow is a group of samples that the device
  produced but the computer did not receive in time.
- The time-domain plots (I/Q, AM, FM, PM) are not available in live mode. To analyze a capture,
  record it. Then use **Open recording** to open it in file mode.

### Recording

The second toolbar row records the live stream. **Record to** is a path prefix. wvt adds the date,
time, frequency and rate to it, for example
`~/wvt-recordings/wvt_20261016_142233_915.000MHz_2.4MSps.sigmf-data`. wvt also creates missing folders.

- **SigMF** (default) writes `.sigmf-data` and `.sigmf-meta`. The metadata holds the sample format,
  rate, device, start time and frequency. If you retune during a recording, wvt starts a new capture
  segment at that sample. Each gap (dropped samples) gets an annotation that tells where the gap is
  and how many samples it lost. wvt writes the metadata again during the recording. Thus the
  metadata is usable even if the wvt process is killed.
- **Raw** writes bare samples (`.cu8`, `.cs16` or `.cf32`). The file name holds the rate and
  frequency, where the metadata detection of wvt finds them. A raw file cannot record a frequency
  change, so wvt locks the tuning during a raw recording.
- **Samples**: *native* saves exactly the samples that the device delivers: cu8 for RTL-SDR and
  ci16 for USRP. This format is the smallest and is lossless. *cf32* converts the samples to
  complex float32 for tools that read only float.
- wvt locks the sample rate during a recording, because one file has one rate. **Limit** stops the
  recording after a set time.
- The storage must be fast enough: 2.4 MS/s cu8 is 4.8 MB/s, but a B205 at 56 MS/s ci16 is 224 MB/s.
  The **buffer** value shows how far the writer is behind. If the buffer fills, wvt drops samples
  and annotates the gap.
- **Open recording** stops the device and opens the last recording in file mode.

`--no-gui` records without a window, for example on a headless machine:

```sh
wvt --device uhd:type=b200 -c 915M -r 10M --record ~/caps/ism --record-duration 30 --no-gui
```

## Signal generator

Open it with the **Signal gen** toolbar button or **Ctrl+G**, or start wvt with `--siggen` or `--gen`.
The window floats over the main window. It works in File mode and in Device mode.

**Waveform**

| | |
|---|---|
| Carrier | sample rate, center frequency, offset from center, peak amplitude (dBFS), start phase |
| CW | a steady tone, with an optional drift (Hz/s). Retuning keeps the phase continuous |
| Pulsed | PRI or PRF, pulse width or duty cycle (the width includes the edges) |
| Edges | rectangular, linear, raised cosine or Gaussian, with separate rise and fall times |
| PRI pattern | fixed, stagger (a list of PRIs used in turn), or jitter (+-%, repeatable with a seed) |
| Bursts | N pulses, then extra dead time. A start delay before the first pulse |
| In-pulse modulation | LFM up / down / triangle chirp with a set bandwidth, or Barker 2-13 phase codes |
| Playback | a SigMF or raw IQ recording as a canned waveform: one segment, looped with a gap or played once |
| Impairments | white noise (SNR), DC offset, IQ gain/phase imbalance, carrier phase noise (linewidth) |

The pulses are slices of one continuous carrier, so they are phase-coherent. wvt checks the
settings while you type. Settings with problems show in red, and wvt does not apply them. Examples
of problems are a pulse wider than the PRI, a Barker chip shorter than one sample, and an offset
outside the band. The **preview** shows one pulse (or the pulse train) in time, and the averaged
spectrum.

**Playback** plays a recording. The recording can be a file from the generator, a wvt recording,
or any SigMF or raw IQ capture. **Load** reads the format, rate and frequency from the SigMF
metadata or the file name. Then it sets the generator to the same values, so the recording plays
at its original rate and frequency. Shortcut buttons load the last file from the generator, or the
file that is open in the viewer. For the file in the viewer, wvt uses the format settings of the
viewer. **Viewer's markers** plays only the part between the time markers of the viewer.

- The 0 Hz point of the recording goes to center + offset. The peak of the recording plays at the
  set amplitude. **Keep its level** sets the amplitude to the recorded peak, so the recording plays
  without scaling.
- If the recording has a different rate, wvt resamples it (windowed sinc, approximately 80 dB
  stopband, up to 64x in each direction). When wvt lowers the rate, a filter removes all content
  outside the new band. This content does not alias.
- wvt can play up to 32 Mi samples (2^25) at a time, at either rate. Impairments apply on top of
  the recording.
- The length of a file output can be a number of plays. With **Loop** off, the file contains the
  start delay and one play. Changes apply immediately. A different recording starts again from
  its beginning.

**Outputs** (select one with *Output* at the top of the window):

- **File** writes SigMF or raw files in cf32, ci16, ci8 or cu8, with names like recordings. The
  length is a duration. In pulsed mode, the length can also be an exact number of PRIs. The SigMF
  metadata includes all generator settings (`wvt:siggen`). If you select it, the metadata also has
  one annotation for each pulse. **Open in viewer** opens the result.
- **Loopback** streams the waveform into the live spectrum and waterfall, in Device mode with the
  device *Signal generator loopback*. The live view shows the waveform as a receiver tuned to the
  center frequency shows it. Changes apply while you make them. The main toolbar can record the
  loopback stream.
- **USRP transmit** transmits the waveform from the TX/RX port of a B200 / B205mini, tuned to the
  center frequency. You can change the frequency, the gain and all waveform settings while the
  USRP transmits.

Transmit safety:
- wvt transmits only after you do these steps:
  1. Tick **Arm transmitter**.
  2. Press **Transmit...**.
  3. In the dialog, press **Transmit**. The dialog shows the frequency, rate and gain.

  The arm setting resets after each transmission.
- TX gain starts at 0 dB. While wvt transmits, the window title, the toolbar (**TX ON**) and a
  large **STOP TRANSMITTING** button show it. If you close the generator window or quit wvt, the
  transmission stops.
- Transmit only into a dummy load or an attenuated cable, or where you have a license. To monitor
  your own signal on a receiver, put a minimum of 30 dB of attenuation between the ports.
- On one B2xx, wvt can receive and transmit at the same time (full duplex). Then both directions
  use the same sample rate, because the B2xx derives them from one clock.
- Between pulses the USRP transmits zeros. Thus a small LO leakage stays at the center frequency.
  A non-zero offset keeps the signal away from this leakage.

```sh
wvt --gen pulsed --pri 1m --pw 20u --lfm up:2M -r 10M -c 915M                     # open the window
wvt --gen pulsed --pri 1m --pw 20u --barker 13 -r 2M --gen-out ~/caps/b13 --gen-pulses 100 --gen-annotate
wvt --device siggen --gen cw --offset 250k -c 915M -r 2M                          # loopback view
wvt --gen cw --offset 100k -c 915M -r 1M --tx --tx-gain 10 --gen-duration 5         # transmit 5 s
wvt --play ~/caps/b13_*.sigmf-meta --play-gap 5m --gen-out ~/caps/b13x10 --gen-pulses 10  # 10 plays
wvt --play capture.cs16 --play-rate 2.4M -r 10M -c 915M --play-segment 1.5:0.2 --tx    # replay a burst
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
- When the pointer is on a plot, all other plots show a crosshair at the same time or frequency.
- Time markers (T1/T2) show in every plot. You can drag them in any plot. Press `Z` to zoom to the span between them.

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

- **Waterfall:** windowed FFTs with fftshift, in dBFS. The FFT step adapts so that the view fits in a maximum of *Rows* rows. wvt max-pools adjacent bins to *Cols* columns, so narrow signals stay visible. The spectrum plot shows the average and the peak hold of the rows in the view.
- **Channel** (optional): mixes the chosen offset down to 0 Hz, applies a windowed-sinc FIR low-pass (Blackman), then decimates. Auto settings: decimation = ⌊Fs / 2·BW⌋ and taps ≈ 12·Fs / BW. The mixer phase is a function of the absolute sample number, so the results do not move when you pan.
- **AM:** |x|, as linear, dB, or normalized m(t) = |x| / mean − 1. Modulation index m = (max − min) / (max + min).
- **FM:** instantaneous frequency arg(x[n]·x*[n−1])·Fs / 2π, either absolute (offset from the capture center) or relative to the channel center. The readout shows mean, peak deviation and rms deviation.
- **PM:** phase, either wrapped, unwrapped, or unwrapped with the best-fit line removed (this removes any remaining carrier offset). Radians or degrees.
- **Smoothing:** a moving average for each of AM, FM and PM.
- **Squelch:** blanks FM and PM wherever smoothed power is below the threshold.
- **Large files:** wvt memory-maps files. wvt demodulates views up to *Max pts* samples (and approximately 32 M input samples) at full resolution. Wider views switch to **SPARSE** mode, which demodulates a short block for each screen pixel. All plots show exact min/max envelopes for each pixel.
- **Background work:** each computation runs on its own worker thread. A new request replaces the old request. Thus a setting change shows its effect immediately, and the UI never blocks.

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
| `src/engine.*` | Worker threads, and the waterfall, channelizer/demodulator and overview computations |
| `src/dsp.*` | FFT, windows, FIR design, parallel-for, smoothing |
| `src/iq_file.*` | mmap reader for all sample formats, plus SigMF and filename detection |
| `src/ui_util.*` | SI-unit inputs and formatters, scrollbar widget, style |
| `src/app_live.cpp` | Live mode UI: device and recording toolbar, live spectrum and scrolling waterfall |
| `src/live.*` | Live engine: owns the device, sample ring, recorder and spectrum thread |
| `src/sample_ring.h` | Ring buffer between the device thread, the spectrum thread and the recorder |
| `src/recorder.*` | SigMF / raw recorder |
| `src/sdr_device.*` | Device interface, driver list, sample-rate checks |
| `src/sdr_rtlsdr.cpp` | RTL-SDR backend (librtlsdr) |
| `src/sdr_uhd.cpp` | USRP backend (UHD C API) |
| `src/sdr_sim.cpp` | Simulated device with synthetic signals |
| `src/siggen.*` | Signal generator DSP: CW / pulsed / playback waveforms, resampler, validation, parameter bus |
| `src/siggen_out.*` | Generator outputs: file job, transmit engine, preview |
| `src/sdr_siggen.cpp` | Loopback device that streams the generator into the live view |
| `src/sdr_tx.h` | Transmitter interface (`sdr_uhd.cpp` implements it for UHD) |
| `src/app_siggen.cpp` | Signal generator window |
| `tools/iqgen.cpp` | Synthetic test-signal generator |
| `tools/siggen_check.cpp` | Self-test for the generator (`build/siggen_check`, no hardware needed) |
