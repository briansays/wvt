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

An **audio** window demodulates one channel (AM, NFM, WFM, USB, LSB or CW) and plays it through the
speakers. It plays a recording or the live stream, and it writes WAV files. See [Audio](#audio).

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

The repository includes Dear ImGui 1.91.9, ImPlot 0.16 and miniaudio 0.11.25 in `third_party/`, so
the build needs no network access. The binary contains the UI font.

Audio playback uses miniaudio. On macOS it links CoreAudio. On Linux it needs no development
packages: at run time it loads PipeWire, PulseAudio or ALSA, whichever is available. To build
without playback, use `-DWVT_WITH_AUDIO=OFF`. WAV export still works without playback.

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

### Optional: GPU acceleration

wvt can compute the file waterfall, the live spectrum and the channel filter (of the
demodulator plots and the region export) on the GPU: with CUDA on an NVIDIA GPU under Linux, or
with Metal on a Mac. This lets wide views keep "FFT sees 100 %", and lets the live spectrum
analyze every sample at USRP rates. The results look the same as on the CPU.

**NVIDIA (CUDA), Linux**
- **Build:** the CUDA toolkit 12.0 or later (12.8 or later for RTX 50-series cards), with
  CMake 3.18 or later. CMake finds `nvcc` and reports
  `wvt: CUDA support enabled (CUDA 12.x, archs ..., static cuFFT)`.
- **Run:** an NVIDIA driver and a GPU with compute capability 7.5 or later (GeForce RTX 20xx
  and newer) and at least 1 GB of free memory. A CUDA build also runs on computers without
  a GPU or driver: it then uses the CPU, and the log says why.

**Apple GPU (Metal), macOS**
- **Build:** nothing to install: the Command Line Tools are enough (no Xcode). CMake reports
  `wvt: Metal support enabled`. The kernels compile when wvt first uses the GPU (about 0.2 s).
- **Run:** macOS 14 or later; older versions use the CPU. The GPU reads the memory-mapped file
  in place (unified memory). FFTs up to 4096 points run in one pass in the GPU's on-chip
  memory; larger ones use MPSGraph.
- On a Mac the CPU path uses Apple's Accelerate FFT (vDSP), so the CPU is also fast. On an M5
  the GPU gives about 1.6× for the waterfall at N ≤ 4096, 2.5–3.4× for a live row, and up
  to 6× for the channel filter with many taps. Small jobs stay on the CPU.

| CMake option | Meaning |
|---|---|
| `-DWVT_WITH_CUDA=AUTO\|ON\|OFF` | `AUTO` (default) builds the GPU code when the toolkit is found; `ON` fails without it |
| `-DWVT_CUDA_STATIC=ON\|OFF` | `ON` (default) links cuFFT into the binary (about 270 MB, needs only the driver); `OFF` makes a small binary that needs the CUDA libraries at run time |
| `-DCMAKE_CUDA_ARCHITECTURES=89` | Build for these GPUs only (smaller and faster to build). The default covers RTX 20xx–50xx, A100 and H100 |
| `-DWVT_WITH_METAL=AUTO\|ON\|OFF` | macOS: `AUTO` (default) builds the Metal backend; `OFF` builds without it |

At run time, `--gpu auto|on|off` (or `WVT_GPU`) chooses: `auto` uses a usable GPU, `on` stops
if there is none, `off` never touches the driver. `--list-devices` lists the GPUs, and
`--gpu-device N` picks one. In the window, the status bar shows `GPU <model>` or `CPU`, and
**Tools → Use the GPU** switches the GPU off and on.

If a GPU error occurs, that part of wvt goes back to the CPU for the rest of the session, and
the log and the status-bar tooltip say why. `build/gpu_check` compares the GPU results with
the CPU, and `build/gpu_check --bench` times them.

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
      --segment T0:T1  load only this part of the file, in seconds (-1 = file start / end)
      --view T0:T1     start with this time range, e.g. --view 0.5:0.52
      --software       force the software renderer
      --scale S        UI scale (or env WVT_SCALE), useful on HiDPI
      --geometry WxH   initial window size
      --screenshot F   render once, write a PPM to F, exit (for automation)
      --gpu auto|on|off  GPU processing, CUDA or Metal (default auto, or env WVT_GPU)
      --gpu-device N   use GPU N (see --list-devices)

live devices (-c and -r set the tuning):
      --device DEV     rtlsdr[:INDEX|SERIAL] | uhd[:ARGS] | sim
      --gain DB|auto   receiver gain (default 30 dB)
      --antenna NAME   USRP antenna (RX2 or TX/RX)
      --bandwidth HZ   USRP analog bandwidth (default: automatic)
      --bias-tee       RTL-SDR: power an LNA / active antenna over the coax
      --direct-sampling I|Q  RTL-SDR: HF direct sampling (v3: Q)
      --ppm N          RTL-SDR frequency correction
      --list-devices   print attached devices and GPUs, and exit

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
      --sweep FROM:TO  --sweep-shape up|down|tri  --sweep-step HZ:DWELL  --sweep-time S
      --level-sweep FROM:TO  --level-shape up|down|tri
      --pri S | --prf HZ   --pw S   --edge rect|linear|cos|gauss  --rise S  --fall S
      --stagger S,S,..  --jitter PCT  --seed N  --burst N[:GAP]  --delay S
      --lfm up|down|tri:BW  --barker N
      --play FILE  --play-rate HZ  --play-segment START:LEN  --play-gap S  --play-once
      --chain FILE.wvtchain.json  --chain-add PATH[@OFFSET][:N]  --chain-loops N
      --snr DB  --dc I:Q  --iq GAIN_DB:PHASE_DEG  --linewidth HZ
      --gen-out PREFIX write a file and exit (container via --record-format)
      --gen-sample cf32|ci16|ci8|cu8   --gen-duration T | --gen-pulses N   --gen-annotate
      --tx             TRANSMIT with a USRP until Ctrl+C (or --gen-duration)
      --tx-gain DB  --tx-args ARGS

audio (--chan OFF:BW sets the channel; without it: 0 Hz and the default width of the mode):
      --audio am|nfm|wfm|usb|lsb|cw   open the audio window and play
      --audio-out FILE.wav   demodulate to WAV and exit (the file or --segment, or the --view range;
                             with --device: the live stream until Ctrl+C or --record-duration)
      --audio-device NAME  --list-audio-devices  --volume DB  --squelch DBFS

export (the --segment limits it; --record-format sigmf|raw sets the file type):
      --extract-out PATH     write the region to PATH (without the extension) and exit
      --extract T0:T1        time range in seconds (default: the whole segment)
      --extract-band LO:HI   only this band (absolute Hz); without it the samples are copied
      --extract-format cf32|ci16   band output format (default cf32)
      --extract-overwrite    replace existing output files
      --export-window        open the export window (the --chan band, or the view)
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

### Large files: load a segment

On a large file, the first view of the whole file can be slow. The waterfall reads samples from
all of the file, and past its work budget it analyzes only part of them ("FFT sees" below 100 %).
To work on part of the file only, type a start and an end time, in seconds, in the **Segment**
boxes of the file row:

- Type the times before you open the file, or change them after. Press Enter to apply a change.
  wvt then shows the new segment and computes all the plots again.
- `-1` means the file start (in the first box) or the file end (in the second box). `-1` to `-1`
  loads the whole file. This is the default.
- The plots, readouts, bookmarks and markers show file time. A segment from 40 s to 50 s shows
  40–50 s, not 0–10 s. The view, the scrollbar, the overview strip and the audio ranges stay
  inside the segment.
- If the start is after the end of the file, or the end is before the start, wvt tells you why
  and keeps the previous segment. An end after the end of the file uses the end of the file.
- The segment stays when you open the next file. If it does not fit that file, wvt loads the
  whole file.

```sh
wvt big.sigmf-meta --segment 40:50                 # 40 s to 50 s
wvt big.sigmf-meta --segment 80:-1 --view 90:91    # 80 s to the end, start with a 1 s view at 90 s
wvt big.sigmf-meta --segment 40:50 --chan 300k:200k --audio wfm --audio-out seg.wav   # export 10 s
```

### Export a region to a new file

**File → Export region…** (or **Ctrl+E**) opens the export window. It writes a time range of
the file, and optionally a frequency band, to a new SigMF or raw file.

- **Fill from:** **View** (the view's time range, full band), **Channel** (the channel filter's
  band over the view), **Markers** (the time and frequency markers), or **Select…**, which lets
  you drag a box on the waterfall. You can also type or adjust each value: start and end in
  seconds or samples, the band as low / high or center / width.
- **Right-click the waterfall** (without dragging) for the same choices: *Select a region to
  export…*, *Export this view…*, *Export the channel…*. In select mode, drag a box with the left
  button (the view does not pan meanwhile); **Esc** cancels. Right-drag still zooms to a box.
- While the window is open, the region shows on the waterfall: drag the box to move it, or its
  edges and corners to resize it (a full-band region shows as two time lines). The time
  range is shaded on the I/Q, AM, FM and PM plots.
- **Full band:** the samples are copied unchanged: same format, rate and center frequency. A
  big-endian or I/Q-swapped file is written in the standard order.
- **A band:** wvt mixes the band center to 0 Hz, low-pass filters with the band as the
  passband (flat to the edges, 25 % transition band on each side, about 74 dB stopband), and
  decimates to `fs / D` with `D = ⌊fs / 1.25·BW⌋`. The window shows the output rate, center
  frequency, taps, decimation, size and file name before you export. The output is cf32, or
  ci16 at half the size (values above full scale are clipped and counted). With a GPU the
  filter runs there.
- The export stays inside the loaded segment. It runs in the background with a progress bar.
  **Cancel** deletes the partial file. An existing file is replaced only after you confirm.
  wvt checks the free disk space first.
- The SigMF metadata has the rate, the center frequency, `core:datetime` (the source's start
  time plus the start of the range, when the source has one), and a `wvt:extract` object with
  the source file, sample range, band and filter. The default name, such as
  `capture_12.5s_100.3MHz_200kSps`, carries the rate and frequency, so a raw export opens in
  wvt without options.

```sh
wvt cap.sigmf-meta --extract 10:20 --extract-out part                  # 10 s, copied unchanged
wvt cap.sigmf-meta --extract 10:20 --extract-band 100.2M:100.4M --extract-out fm   # a 200 kHz band
```

## Live devices

Select **Device** in the **Source** drop-down on the right of the menu bar, or start wvt with
`--device`. Select the device, set
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
| Extras (⚙ next to AGC) | bias-tee, direct sampling, PPM correction | antenna RX2 / TX/RX, analog bandwidth |
| Samples | cu8 | ci16 (sc16) |

- You can change **frequency and gain** while the device streams. A change to the rate or to a
  device option restarts the stream.
- The **waterfall** scrolls with the newest row at the top. Its vertical axis shows seconds ago.
  The **FFT**, **Window** and **dB** range are on the settings row; the rest is behind its ⚙ button.
  *Rows/s* sets the scroll speed, and *History* sets how many rows wvt keeps. *Avg* `auto` averages
  all samples received since the previous row, up to 20 MS/s (100 MS/s on the GPU). The status bar shows the fraction of
  the samples that the FFT sees. Lower *Avg* to see short bursts more sharply.
- The **spectrum** shows a running average and a peak hold. *Spec avg* sets the time constant of
  the average, in rows. *Reset* (next to *Peak hold* in the ⚙ options) restarts the peak hold.
- The **status bar** shows the device, the actual rate and frequency, and all overflows. Devices
  may adjust the rate and frequency that you set. An overflow is a group of samples that the device
  produced but the computer did not receive in time.
- The time-domain plots (I/Q, AM, FM, PM) are not available in live mode. To analyze a capture,
  record it. Then use **Open recording** to open it in file mode.

### Recording

**Record** (on the device row) records the live stream; its ⚙ button sets where and how. **Record to** is a path prefix. wvt adds the date,
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

Open it with **Tools → Signal generator** or **Ctrl+G**, or start wvt with `--siggen` or `--gen`.
The window floats over the main window. It works in File mode and in Device mode.

**Waveform**

| | |
|---|---|
| Carrier | sample rate, center frequency, offset from center, peak amplitude (dBFS), start phase |
| CW | a steady tone, with an optional drift (Hz/s). Retuning keeps the phase continuous |
| Sweeps | CW only. A frequency sweep (ramp up, ramp down, triangle, or stepped with a step and dwell) between two offsets, and a level sweep (ramp or triangle, linear in dB) between two dBFS levels. Both share one period and repeat until the output stops |
| Pulsed | PRI or PRF, pulse width or duty cycle (the width includes the edges) |
| Edges | rectangular, linear, raised cosine or Gaussian, with separate rise and fall times |
| PRI pattern | fixed, stagger (a list of PRIs used in turn), or jitter (+-%, repeatable with a seed) |
| Bursts | N pulses, then extra dead time. A start delay before the first pulse |
| In-pulse modulation | LFM up / down / triangle chirp with a set bandwidth, or Barker 2-13 phase codes |
| Playback | a SigMF or raw IQ recording as a canned waveform: one segment, looped with a gap or played once |
| Chain | several recordings played in turn, each with its own segment, offset, level, plays and gap; the chain loops forever or N times |
| Impairments | white noise (SNR), DC offset, IQ gain/phase imbalance, carrier phase noise (linewidth) |

The pulses are slices of one continuous carrier, so they are phase-coherent. wvt checks the
settings while you type. Settings with problems show in red, and wvt does not apply them. Examples
of problems are a pulse wider than the PRI, a Barker chip shorter than one sample, and an offset
outside the band. The **preview** shows one pulse (or the pulse train) in time, and the averaged
spectrum. For a sweep it shows the frequency and the level over one period, and a max-hold spectrum
of the swept band.

**Sweeps.** While the frequency sweeps, it replaces the Offset (and the Drift). While the level
sweeps, it replaces the Amplitude. The phase stays continuous through the wraps and the steps.
Changing a sweep setting while an output runs starts the period again. Noise (SNR) is set from the
peak of the level sweep.

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

**Chain** plays a list of canned signals in order: files from the generator, regions exported
from the viewer, or any SigMF or raw IQ recording. Add recordings by path, or with the **Last
generated**, **Viewer's file** and **Viewer's markers** shortcuts. Click a row to edit it.

- Each item has its own format, file rate, segment, **offset**, **level**, number of **plays** and
  **gap after** each play. Adding a recording reads its metadata. Its offset puts it at its
  recorded frequency, if that is inside the band. The first recording also sets the generator's
  rate and center frequency. The level is **as recorded** by default (the samples play
  unchanged); otherwise the recording's peak plays at the set dBFS.
- **Loop**: *Forever* plays the chain until the output stops; *Times* plays it N times. After the
  last pass the output is silent: a file ends there, the loopback says so, and a USRP stops
  transmitting by itself.
- While it plays, offset and level changes apply at once. Adding, removing or reordering items, or
  changing a segment, starts the chain again from the first item after the current one ends.
- All the items are loaded before the chain starts, up to 128 Mi samples in all (1 GiB as cf32),
  each up to 32 Mi samples. The noise (SNR) refers to the loudest item.
- **Chain file**: **Save** writes the chain, the loop count, the start delay and the generator's
  rate and center to a `.wvtchain.json` file. Recordings in the chain file's folder (or below it)
  are stored relative to it, so the folder can move. **Load** reads it back, and so does `--chain`.
- The preview shows one pass with each item shaded and numbered, and its average spectrum.

**Outputs** (select one with *Output* at the top of the window):

- **File** writes SigMF or raw files in cf32, ci16, ci8 or cu8, with names like recordings. The
  length is a duration. In pulsed mode, the length can also be an exact number of PRIs, with
  a sweep an exact number of sweep periods, and with a chain a number of passes. The SigMF
  metadata includes all generator settings (`wvt:siggen`). If you select it, the metadata also has
  one annotation for each pulse, play, sweep or chain item. **Open in viewer** opens the result.
- **Loopback** streams the waveform into the live spectrum and waterfall, in Device mode with the
  device *Signal generator loopback*. The live view shows the waveform as a receiver tuned to the
  center frequency shows it. Changes apply while you make them. The main window can record the
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
- TX gain starts at 0 dB. While wvt transmits, the window title, the menu bar (**TX ON**) and a
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
wvt --device siggen --sweep -400k:400k --sweep-shape tri --sweep-time 2 -c 915M -r 2M  # sweep, loopback
wvt --sweep -1M:1M --sweep-time 10m --level-sweep -40:-6 -r 5M -c 915M --gen-out ~/caps/sw --gen-pulses 20
wvt --sweep -200k:200k --sweep-step 25k:5m -r 1M -c 915M --tx --tx-gain 10        # stepped, transmit
wvt --gen cw --offset 100k -c 915M -r 1M --tx --tx-gain 10 --gen-duration 5         # transmit 5 s
wvt --play ~/caps/b13_*.sigmf-meta --play-gap 5m --gen-out ~/caps/b13x10 --gen-pulses 10  # 10 plays
wvt --play capture.cs16 --play-rate 2.4M -r 10M -c 915M --play-segment 1.5:0.2 --tx    # replay a burst
wvt --chain-add ~/caps/b13.sigmf-meta:3 --chain-add ~/caps/lfm.sigmf-meta@250k --chain-loops 5 --tx  # 5 passes
wvt --chain ~/caps/demo.wvtchain.json --gen-out ~/caps/demo --gen-pulses 10 --gen-annotate          # 10 passes
```

## Audio

Open the audio window with **Tools → Audio** or **Ctrl+D**, or start wvt with `--audio MODE`.
The window floats over the main window. It works in File mode and in Device mode.

**Channel.** With **Follow viewer channel** on (the default), the audio uses the channel of the
viewer: the cyan lines on the spectrum and waterfall. To tune, Ctrl+click the spectrum or the
waterfall, or drag the lines. Live mode also shows these lines. With *Follow* off, the audio channel
is the orange line, and you set its offset and bandwidth in the window. When you select a mode, wvt
sets the bandwidth to the default of that mode.

| Mode | Default width | Notes |
|---|---|---|
| AM | 10 kHz | envelope. *AGC* divides by the carrier level |
| NFM | 12.5 kHz | audio up to 5 kHz |
| WFM | 200 kHz | mono, audio up to 15 kHz, 75 µs de-emphasis (select 50 µs for Europe) |
| USB / LSB | 2.8 kHz | the passband is the shaded channel. The carrier is at its lower edge (USB) or upper edge (LSB) |
| CW | 500 Hz | a carrier at the channel center plays at the *BFO pitch* (200-1500 Hz) |

- **Gain** adds gain after the demodulator. **Squelch** mutes the audio while the channel power is
  below the threshold. The window shows the channel power, so you can set the threshold just above
  the noise.
- **Volume** and **Mute** change only the speaker level. They do not change WAV files.
- **Device** selects the sound output. The change applies on the next Play.

**File mode.** **Play** plays the **Range**: *from view start* to the end of the file, the *view*,
the part between the *time markers*, or the *whole file*. With a segment loaded, every range stays
inside the segment, and *whole file* becomes *whole segment*. A green playhead line moves across the
time plots and the waterfall. Drag it, or the position slider, to seek. **Shift+click** a time plot
or the waterfall to play from that point. *Loop* repeats the range. *Follow playhead* pages the view
forward when the playhead leaves it (only for views of 0.25 s or more).

**Device mode.** **Play** plays the live stream, with approximately 0.15 s of delay. The sound card
and the SDR run from different clocks. wvt measures the buffer level and trims the resampling ratio
(the window shows the correction in ppm), so the delay stays constant. If the demodulator falls
behind, it skips to the newest samples and counts a *resync*. A retune or a stream restart
continues the audio automatically.

**WAV files** are 48 kHz, 16-bit mono. The name is the *File prefix* plus the date, time,
frequency and mode, for example `~/wvt-recordings/audio_20261018_143000_100.3MHz_WFM.wav`.
- File mode: **Save WAV** demodulates the range faster than real time.
- Device mode: **Record audio** records the demodulated stream. It also works without a sound device.

```sh
wvt capture.sigmf-meta --chan 300k:200k --audio wfm                       # play a broadcast FM channel
wvt capture.sigmf-meta --chan -250k:10k --audio am --view 0.5:1.5 --audio-out am.wav   # export 1 s
wvt --device rtlsdr -c 100.4M -r 2.4M --audio wfm --chan -300k:200k       # listen live to 100.1 MHz
wvt --device sim -c 100M -r 2.4M --audio wfm --chan 360k:200k --audio-out fm.wav --record-duration 10
```

The demodulator shifts the channel to 0 Hz, then decimates with half-band filters to at least
twice the channel width, filters the channel (windowed-sinc FIR), and demodulates. A polyphase
resampler then sets the audio bandwidth and the 48 kHz output rate. All stages keep their state
between blocks, so the audio is continuous. `build/audio_check` tests each mode (no hardware needed).

## Screen layout

| Area | Contents |
|---|---|
| **Menu bar** | **File** (open, export region, quit), **View** (time and frequency markers, relative frequency, peak hold, the overview strip, which time plots show, show everything), **Tools** (signal generator, audio, use the GPU), **Help** (keyboard and mouse, about). On the right: **TX ON**, **REC**, a running audio or export, and the **Source** (File / Device) |
| **File row** | File path, **Format** (a drop-down with the sample format, I/Q swap, big endian and header bytes), sample rate, center frequency, segment (start / end time to load), file size |
| **Navigation row** | Start / back a page / back 10% / forward 10% / forward a page / end, zoom in/out/all, span presets and span, start time, and **Go to** (a sample index, *Add bookmark* and the bookmarks) |
| **Overview strip** | Power over the whole file (or the segment). The white box is the current view: drag it to move, drag its edges to resize, or click anywhere to jump there. Hidden from **View**, and by itself when the window is short |
| **Scrollbar** | Scrolls the view through the file (drag the thumb, click the track to move a page, or use the wheel) |
| **Left** | Waterfall settings (FFT size, window, dB range / auto; ⚙ for overlap, averaging, rows / cols, colormap and peak hold) above the spectrum of the current view, the waterfall, and its color bar |
| **Right** | Channel settings (on / off, offset, bandwidth, to peak; ⚙ for taps, decimation, squelch, max points and plot height), then scrollable I/Q, AM, FM and PM plots. Each plot's header has its show checkbox, a ⚙ button for its options (also on a right-click of the plot) and its measurement readout, so nothing is drawn over the traces |
| **Status bar** | Values under the cursor (t, sample #, f, dB, AM/FM/PM), marker ΔT, 1/ΔT and ΔF, `GPU <model>` or `CPU` (builds with CUDA or Metal), renderer, and a busy indicator |

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
| Export a region | Right-click the waterfall, or `Ctrl+E` |
| Plot options | Right-click a time plot (I/Q, AM, FM, PM), or its ⚙ button |
| Fit to data | Double-click |
| Tune the channel | Ctrl+click the spectrum or waterfall, or drag the cyan lines (center line moves it, edge lines change bandwidth) |
| Play audio from a point | Shift+click a time plot or the waterfall |
| Signal generator / audio / export window | `Ctrl+G` / `Ctrl+D` / `Ctrl+E` |
| Quit | `Ctrl+Q` |
| Pan the view | `←` `→` (10%), `PgUp` `PgDn` (one page) |
| Zoom the time axis | `+` / `-` |
| Jump to start / end | `Home` / `End` |
| Show everything | `A` |
| Toggle markers | `T` time markers, `F` frequency markers |
| Toggle channel filter | `C` |
| Add bookmark | `B` |

**Help → Keyboard and mouse** shows these in the window.

## Signal processing

- **Waterfall:** windowed FFTs with fftshift, in dBFS. The view fits in a maximum of *Rows* rows. When a row covers more time than one FFT, the row averages enough FFTs to cover its full time slice. Thus short pulses cannot fall between FFTs. *Avg* sets the minimum number of FFTs for each row. Above 256 M samples of FFT input (2 G on the GPU), wvt uses a sample of each slice. The settings row then shows the percentage of the view that the FFTs see. wvt max-pools adjacent bins to *Cols* columns, so narrow signals stay visible. The spectrum plot shows the average and the peak hold of the rows in the view.
- **Channel** (optional): mixes the chosen offset down to 0 Hz, applies a windowed-sinc FIR low-pass (Blackman), then decimates. Auto settings: decimation = ⌊Fs / 2·BW⌋ and taps ≈ 12·Fs / BW. With a GPU, large full-resolution views run the mixer and filter there. The mixer phase is a function of the absolute sample number, so the results do not move when you pan.
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
| `src/app_menu.cpp` | Menu bar, keyboard help and About windows |
| `src/engine.*` | Worker threads, and the waterfall, channelizer/demodulator and overview computations |
| `src/dsp.*` | FFT (Accelerate vDSP on macOS), windows, FIR design, parallel-for, smoothing |
| `src/gpu.h`, `src/gpu_common.cpp` | GPU interface, `--gpu` rules, fallback to the CPU |
| `src/gpu_cuda.cu` | CUDA backend: device setup, waterfall and live spectrum kernels with cuFFT |
| `src/gpu_metal.mm` | Metal backend (macOS): device setup, one-pass FFT kernels, MPSGraph FFT, channel filter |
| `src/gpu_stub.cpp`, `src/gpu_backend.h` | Backend for builds without a GPU backend; the backend interface |
| `src/iq_file.*` | mmap reader for all sample formats, plus SigMF and filename detection |
| `src/ui_util.*` | SI-unit inputs and formatters, scrollbar widget, forms, gear button, colour bar, style |
| `src/app_live.cpp` | Live mode UI: device and recording toolbar, live spectrum and scrolling waterfall |
| `src/live.*` | Live engine: owns the device, sample ring, recorder and spectrum thread |
| `src/sample_ring.h` | Ring buffer between the device thread, the spectrum thread and the recorder |
| `src/recorder.*` | SigMF / raw recorder |
| `src/sdr_device.*` | Device interface, driver list, sample-rate checks |
| `src/sdr_rtlsdr.cpp` | RTL-SDR backend (librtlsdr) |
| `src/sdr_uhd.cpp` | USRP backend (UHD C API) |
| `src/sdr_sim.cpp` | Simulated device with synthetic signals |
| `src/siggen.*` | Signal generator DSP: CW / pulsed / playback / chain waveforms, waveform cache, resampler, validation, chain files, parameter bus |
| `src/siggen_out.*` | Generator outputs: file job, transmit engine, preview |
| `src/sdr_siggen.cpp` | Loopback device that streams the generator into the live view |
| `src/sdr_tx.h` | Transmitter interface (`sdr_uhd.cpp` implements it for UHD) |
| `src/app_siggen.cpp` | Signal generator window |
| `src/app_chain.cpp` | Signal generator: the Chain section and its preview |
| `src/json.*` | Small JSON reader (chain files) |
| `src/audio_dsp.*` | Streaming audio demodulator: channelizer, AM / FM / SSB / CW, resampler |
| `src/audio_engine.*` | Audio sessions (file or live), drift control, WAV recording and export |
| `src/audio_out.*` | Sound output (miniaudio) and the ring buffer to the audio callback |
| `src/wav_writer.*` | WAV file writer |
| `src/app_audio.cpp` | Audio window |
| `tools/iqgen.cpp` | Synthetic test-signal generator |
| `tools/siggen_check.cpp` | Self-test for the generator (`build/siggen_check`, no hardware needed) |
| `tools/audio_check.cpp` | Self-test for the audio demodulator (`build/audio_check`, no hardware needed) |
| `tools/segment_check.cpp` | Self-test for the time segment rules (`build/segment_check`) |
| `src/extract.*` | Region export: checks, filter design, the background job (copy, or mix + filter + decimate) |
| `src/app_export.cpp` | Export window, waterfall right-click menu, region selection |
| `tools/extract_check.cpp` | Self-test for the region export (`build/extract_check`) |
| `tools/gpu_check.cpp` | GPU self-test (GPU against CPU) and benchmark (`build/gpu_check [--bench]`) |
