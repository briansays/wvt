# Live devices

wvt streams from an RTL-SDR v3, a USRP B200 / B205mini or a Signal Hound BB60. It shows the
spectrum and a scrolling
waterfall in real time, and it records the stream to SigMF or raw IQ files. wvt opens these files
directly.

## Install the device libraries

Live streaming needs librtlsdr (for an RTL-SDR), UHD (for a USRP), or both. Install them before
you configure the build. CMake finds them automatically.

| System | RTL-SDR | UHD (USRP B200 / B205) |
|---|---|---|
| Debian / Ubuntu | `sudo apt install librtlsdr-dev` | `sudo apt install libuhd-dev uhd-host` |
| Fedora | `sudo dnf install rtl-sdr-devel` | `sudo dnf install uhd-devel` |
| Arch | `sudo pacman -S rtl-sdr` | `sudo pacman -S libuhd` |
| macOS | `brew install librtlsdr` | `brew install uhd` |

The Signal Hound BB60 needs the Signal Hound SDK. Refer to [Signal Hound BB60](#signal-hound-bb60).

Without these libraries, wvt still builds. It then has the file viewer, recording from the
simulator, and the signal generator. For the CMake options, refer to
[Build reference](build.md#wvt_with_rtlsdr).

## Prepare the device

### RTL-SDR v3 on Linux

1. Stop the DVB-T driver of the kernel from using the dongle:

   ```sh
   echo 'blacklist dvb_usb_rtl28xxu' | sudo tee /etc/modprobe.d/blacklist-rtlsdr.conf
   ```

2. Disconnect and connect the dongle again, or type `sudo modprobe -r dvb_usb_rtl28xxu`.
3. Make sure that `rtl_test` works without root. On most distributions, the `rtl-sdr` package
   installs a udev rule. If `rtl_test` works only as root, add your user to the `plugdev` group.
   Then connect the dongle again.

### USRP B200 / B205mini

1. Download the FPGA and firmware images one time: `sudo uhd_images_downloader`. On macOS with
   Homebrew, type the command without `sudo`.
2. On Linux, install the UHD udev rule, so that UHD works without root. On Debian and Ubuntu, the
   `uhd-host` package installs this rule.
3. Connect the device to a USB 3 port. Rates above approximately 8 MS/s need USB 3.
4. To make sure that the computer finds the device, type `uhd_find_devices`.

### Signal Hound BB60

This applies to the BB60D, BB60C and BB60A.

1. Download the Signal Hound SDK from the Signal Hound web site, and extract it.
2. Configure the build with the SDK folder:

   ```sh
   cmake -S . -B build -DSIGNALHOUND_SDK_DIR=$HOME/signal_hound_sdk
   ```

   CMake shows `wvt: Signal Hound BB60 support enabled (bb_api 5.0.x, ...)`. It copies the library
   into `build/bb_api/`, and wvt finds it there.
3. On Linux, install the udev rule of the SDK (`lib/linux_x64/sh_usb.rules` or
   `lib/aarch64/sh_usb.rules`) in `/etc/udev/rules.d/`, so that the BB60 works without root. Then
   connect the BB60 again.
4. Connect the BB60 to a USB 3 port.
5. Close Spike and other programs that use the BB60. One program at a time can use it.

The SDK has libraries for these computers:

| Computer | Library | Notes |
|---|---|---|
| Linux x86-64 | `lib/linux_x64/<distribution>/libbb_api.so.5.x` | CMake uses the newest version 5 library. It also copies `libftd2xx.so` |
| Linux ARM64 | `lib/aarch64/libbb_api.so.5.x` | For example a Raspberry Pi 5 or a Jetson |
| macOS ARM (Apple silicon) | `lib/macos_arm/libbb_api.5.x.dylib` | Needs libusb from the ARM Homebrew (`/opt/homebrew`): `brew install libusb`. The rates go down to 312.5 kS/s only (decimation up to 128) |

:::{note}
The macOS library is for ARM only. A wvt build for x86-64 (for example with the Intel Homebrew in
`/usr/local`) cannot use it. On Apple silicon, build with the ARM Homebrew in `/opt/homebrew`.
Then all the backends (RTL-SDR, UHD, BB60, Metal) build together:

```sh
brew install cmake pkgconf libusb librtlsdr uhd      # the ARM Homebrew
PATH=/opt/homebrew/bin:$PATH cmake -S . -B build -DSIGNALHOUND_SDK_DIR=$HOME/signal_hound_sdk
PATH=/opt/homebrew/bin:$PATH cmake --build build -j
```

If CMake shows `this build is x86_64`, the CMake in the `PATH` is the Intel one.
:::

## Start a stream

1. In the **Source** drop-down on the right of the menu bar, select **Device**.
2. Select the device.
3. Set the center frequency, the sample rate and the gain.
4. Click **Start**.

Each wvt window streams from one device only. You can also start wvt with `--device`:

```sh
wvt --device rtlsdr -c 100.1M -r 2.4M --gain 30          # RTL-SDR v3, FM broadcast band
wvt --device rtlsdr -c 7.1M -r 2.4M --direct-sampling Q  # RTL-SDR v3, HF
wvt --device uhd:type=b200 -c 915M -r 20M --gain 40      # USRP B200 / B205mini
wvt --device bb -c 2.45G -r 20M --ref-level -30          # Signal Hound BB60 (the first one)
wvt --device sim -c 100M -r 2.4M                         # built-in simulator, no hardware
```

To see the devices that wvt finds, type `wvt --list-devices`.

## Device limits

| | RTL-SDR v3 | USRP B200 / B205mini | Signal Hound BB60 |
|---|---|---|---|
| Tuning | 24 MHz to 1.766 GHz. HF below 24 MHz with direct sampling (Q) | 70 MHz to 6 GHz | 9 kHz to 6.4 GHz |
| Sample rate | 225 kS/s to 300 kS/s, or 0.9 MS/s to 3.2 MS/s. 2.4 MS/s or less is reliable | Up to 56 MS/s (USB 3) | 40 MS/s divided by a power of 2: 40, 20, 10 ... 4.883 kS/s |
| Gain | 0 dB to 49.6 dB in tuner steps, or AGC | 0 dB to 76 dB, or AGC | A reference level, −130 dBm to +20 dBm |
| Options (⚙) | Bias-tee, direct sampling, PPM correction | Antenna RX2 or TX/RX, analog bandwidth | Attenuator, amplifier, I/Q bandwidth, external reference |
| Samples | `cu8` | `ci16` (sc16) | `ci16` (sc16) |

### BB60 settings

Ref level
: The strongest input that you expect, in dBm. The BB60 sets its attenuator and amplifier from
  it. If the level is too low, the ADC overloads, and the status bar shows *ADC overload: raise the
  reference level*. If the level is too high, the noise floor goes up. The BB60 has no AGC. A
  change applies when you release the slider, because the BB60 restarts its stream.

Sample rate
: Select a rate from the list. The BB60 makes only 40 MS/s divided by a power of 2. Above 10 MS/s,
  the API filters the samples on the computer, which needs a fast CPU.

Atten and Amp
: The attenuator (0, 10, 20 or 30 dB) and the amplifier (0 to 3; BB60D: 0, 5, 15 or 20 dB). *Auto*
  sets them from the reference level. To set them by hand, set both. Then wvt does not use the
  reference level.

Bandwidth
: The I/Q filter bandwidth. 0 sets the widest filter for the rate: 27 MHz at 40 MS/s, 17.8 MHz at
  20 MS/s, 8 MHz at 10 MS/s, and less at lower rates. A wider value is limited, and the status bar
  tells you.

Ext ref
: Lock to a 10 MHz reference on port 1.

A retune or a new reference level restarts the stream of the BB60. The log (DEBUG) shows how long
each restart takes. wvt marks the retune between the last old sample and the first new one. Thus a SigMF recording
starts its new capture segment at the correct sample.

## The live view

- You can change the **frequency** and the **gain** while the device streams. A change to the rate
  or to a device option starts the stream again.
- The **waterfall** scrolls with the newest row at the top. Its vertical axis shows the seconds
  before now.
- The **FFT**, **Window** and **dB** range are on the settings row. The other settings are behind
  its ⚙ button:
  - *Rows/s* sets the scroll speed.
  - *History* sets the number of rows that wvt keeps.
  - *Avg* `auto` averages all samples since the previous row, up to 20 MS/s (100 MS/s on the GPU).
    The status bar shows the fraction of the samples that the FFT sees. To see short bursts more
    clearly, set a lower *Avg*.
- The **spectrum** shows a running average and a peak hold. *Spec avg* sets the time constant of
  the average, in rows. *Reset*, next to *Peak hold* in the ⚙ options, starts the peak hold again.
- The **status bar** shows the device, the actual rate and frequency, and all overflows. The device
  can change the rate and the frequency that you set. An overflow is a group of samples that the
  device sent but the computer did not receive in time.
- The time plots (I/Q, AM, FM, PM) are not available in live mode. To analyze a signal, record it.
  Then use **Open recording** to open it in file mode.

## Recording

**Record**, on the device row, records the live stream. Its ⚙ button sets where and how.

**Record to** is a path prefix. wvt adds the date, the time, the frequency and the rate, for
example `~/wvt-recordings/wvt_20261016_142233_915.000MHz_2.4MSps.sigmf-data`. wvt makes the
folders that do not exist.

### File types

**SigMF** (the default) writes `.sigmf-data` and `.sigmf-meta`.

- The metadata has the sample format, the rate, the device, the start time and the frequency.
- If you tune to a new frequency during a recording, wvt starts a new capture segment at that
  sample.
- Each gap (dropped samples) gets an annotation. The annotation tells where the gap is and how
  many samples it lost.
- wvt writes the metadata again during the recording. Thus the metadata is usable if the wvt
  process stops unexpectedly.

**Raw** writes only the samples (`.cu8`, `.cs16` or `.cf32`).

- The file name has the rate and the frequency, where wvt can find them.
- A raw file cannot record a frequency change. Thus wvt locks the tuning during a raw recording.

### Samples

- *native* saves the samples that the device sends: `cu8` for an RTL-SDR and `ci16` for a USRP.
  This format is the smallest, and it has no loss.
- *cf32* changes the samples to complex 32-bit float, for tools that read only float.

### Limits

- One file has one rate. Thus wvt locks the sample rate during a recording.
- **Limit** stops the recording after a set time.
- The storage must be sufficiently fast. For example, 2.4 MS/s `cu8` is 4.8 MB/s, but a B205 at
  56 MS/s `ci16` is 224 MB/s.
- The **buffer** value shows how far the writer is behind. If the buffer is full, wvt drops samples
  and adds an annotation for the gap.
- **Open recording** stops the device and opens the last recording in file mode.

### Record without a window

`--no-gui` records without a window, for example on a computer without a display. Press Ctrl+C to
stop.

```sh
wvt --device uhd:type=b200 -c 915M -r 10M --record ~/caps/ism --record-duration 30 --no-gui
```

| Option | Function |
|---|---|
| `--record PREFIX` | Record to `PREFIX_<date>_<time>_<fc>_<rate>.<ext>` |
| `--record-format sigmf\|raw` | The file type. The default is `sigmf` |
| `--record-sample native\|cf32` | The sample format. The default is `native` |
| `--record-duration T` | Stop after `T` seconds |
| `--no-gui` | Record without a window |

## How it works

The device thread writes the samples into a ring buffer, in the native format of the device. Two
types of reader use this buffer:

- The spectrum thread copies the newest samples. If the device thread writes over the samples
  during the copy, the spectrum thread skips them.
- The recorder reads all samples. If the recorder is behind, the device thread does not write over
  the samples that the recorder did not read. It drops the new block and records the gap.

Thus a slow disk does not stop the spectrum, and each loss in a recording has an annotation.

The BB60 backend (`src/sdr_bb60.cpp`) uses `bb_api` from the Signal Hound SDK in I/Q streaming
mode with 16-bit samples. Its thread reads blocks of approximately 20 ms. For tests without the
SDK or a device, `-DWVT_BB_SIM=ON` builds the backend against a simulated BB60D
(`tools/bb_sim/`). The simulator makes a tone at 100.2 MHz and noise, and it reports an overload
when the tone is above the reference level. The self-check `build/bb_check` uses it. For the
design, refer to [the BB60 design record](design/bb60-integration-plan.md).
