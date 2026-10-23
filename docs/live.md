# Live devices

wvt streams from an RTL-SDR v3, a USRP, a Signal Hound BB60 or an Epiq Solutions Sidekiq. The USRP
can be the USB B200 family, or a network USRP such as the N310, X310 or X410. It shows the spectrum and a scrolling
waterfall in real time, and it records the stream to SigMF or raw IQ files. wvt opens these files
directly.

## Install the device libraries

Live streaming needs librtlsdr (for an RTL-SDR), UHD (for a USRP), or both. Install them before
you configure the build. CMake finds them automatically.

| System | RTL-SDR | UHD (USRP) |
|---|---|---|
| Debian / Ubuntu | `sudo apt install librtlsdr-dev` | `sudo apt install libuhd-dev uhd-host` |
| Fedora | `sudo dnf install rtl-sdr-devel` | `sudo dnf install uhd-devel` |
| Arch | `sudo pacman -S rtl-sdr` | `sudo pacman -S libuhd` |
| macOS | `brew install librtlsdr` | `brew install uhd` |

The Signal Hound BB60 needs the Signal Hound SDK. Refer to [Signal Hound BB60](#signal-hound-bb60).
The Epiq Sidekiq needs the Sidekiq SDK, on Linux only. Refer to [Epiq Sidekiq](#epiq-sidekiq).

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

### USRP B200 family (USB)

This applies to the B200, B210, B200mini and B205mini.

1. Download the FPGA and firmware images one time: `sudo uhd_images_downloader`. On macOS with
   Homebrew, type the command without `sudo`.
2. On Linux, install the UHD udev rule, so that UHD works without root. On Debian and Ubuntu, the
   `uhd-host` package installs this rule.
3. Connect the device to a USB 3 port. Rates above approximately 8 MS/s need USB 3.
4. To make sure that the computer finds the device, type `uhd_find_devices`.

### Network USRPs (N3xx, X3xx, X4xx)

This applies to the N300, N310, N320, N321, X300, X310 and X410, and to the other USRPs that
connect through Ethernet.

1. Make sure that the FPGA image on the device agrees with the UHD version on the computer. If
   `uhd_usrp_probe` shows an image error, update the image as the Ettus manual tells you.
2. Give the network interface of the computer an address on the subnet of the USRP. For example,
   an X310 has `192.168.10.2` on SFP port 0 (1 GbE) and `192.168.40.2` on SFP port 1 (10 GbE).
3. For rates above approximately 25 MS/s, use a 10 GbE (or faster) link. Set the MTU of the
   interface to 9000.
4. On Linux, make the network buffers of the kernel larger, as UHD tells you when it starts.
5. To make sure that the computer finds the device, type `uhd_find_devices` or
   `uhd_find_devices --args addr=192.168.40.2`.

A network USRP makes its sample rates from a fixed master clock divided by a whole number. For
example, an X310 at 200 MHz gives 200, 100, 66.67, 50 MS/s and so on. If you set a different rate,
the device uses the nearest rate that it can make, and the status bar tells you. To use a
different master clock, add `master_clock_rate=` to the device arguments, for example
`addr=192.168.40.2,master_clock_rate=184.32e6`.

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

### Epiq Sidekiq

This applies to the Sidekiq cards in a Linux computer: M.2, M.2-2280 (Stretch), mPCIe, X2, X4 and
NV100. wvt receives on one channel (handle A1). The embedded products (Z2, Z3u, Z4, Matchstiq) run
their own Linux, and wvt does not run on them.

1. Get the Sidekiq SDK (version 4.14.0 or later; wvt is tested with 4.26.0) from the Epiq
   Solutions support portal. Extract it. The SDK is not open source: do not add it to the
   repository.
2. Install the kernel modules and the udev rules of the SDK, as the SDK manual tells you
   (*Installation procedure*). Make sure that the SDK program `sidekiq_probe` finds the card.
3. Configure the build with the SDK folder:

   ```sh
   cmake -S . -B build -DSIDEKIQ_SDK_DIR=$HOME/sidekiq_sdk_v4.26.0
   ```

   CMake shows `wvt: Sidekiq support enabled (SDK 4.26.0, ...)`. Without `SIDEKIQ_SDK_DIR`, CMake
   looks in `$SIDEKIQ_SDK_DIR`, then in `~/sidekiq_sdk_current`, then in
   `third_party/sidekiq_sdk/sidekiq_sdk_v*` (the newest). CMake gets the link flags from the
   `bin/sidekiq-config` program of the SDK, and links `libsidekiq` statically. wvt loads the
   support libraries of the SDK (libusb, glib, tirpc) from the SDK folder at run time, so keep
   the SDK in its folder after the build.
4. Close the other programs that use the card. One program at a time can use a card. If a card is
   in use, the device list and the error message give the process ID of the program that has it.

The SDK has libraries for Linux x86-64 (`lib/libsidekiq__x86_64.gcc.a`) and Linux ARM64
(`lib/libsidekiq__aarch64.a`). CMake selects the library for the CPU of the build. On macOS, CMake
shows `Sidekiq support disabled (the Sidekiq SDK is for Linux only)`.

:::{note}
At high rates, a recording writes 4 bytes per sample: 1 GB/s at 245.76 MS/s on an X2 or X4.
Record to a fast NVMe disk, or use a lower rate. The spectrum analyzes all the samples up to
20 MS/s, or up to approximately 100 MS/s with the GPU. The *FFT sees* readout shows the part that
it analyzes.
:::

## Start a stream

1. In the **Source** drop-down on the right of the menu bar, select **Device**.
2. Select the device. wvt looks for devices in the background, on USB and on the network. Until
   the search is done, the list shows *Scanning...*. To search again, click **Rescan**.
3. For a USRP that is not in the list, type its device arguments in **Args** and press Enter. For
   example, type `addr=192.168.40.2`.
4. Set the center frequency, the sample rate and the gain.
5. Click **Start**.

Each wvt window streams from one device only. You can also start wvt with `--device`:

```sh
wvt --device rtlsdr -c 100.1M -r 2.4M --gain 30          # RTL-SDR v3, FM broadcast band
wvt --device rtlsdr -c 7.1M -r 2.4M --direct-sampling Q  # RTL-SDR v3, HF
wvt --device uhd -c 915M -r 20M --gain 40                # the first USRP found (for example a B205mini)
wvt --device uhd:addr=192.168.40.2 -c 2.4G -r 50M --usrp-channel 1 --antenna RX2   # X310, second daughterboard
wvt --device uhd:addr=192.168.10.2 -c 3.5G -r 122.88M --ref external                # X410 on a 10 MHz / PPS reference
wvt --device bb -c 2.45G -r 20M --ref-level -30          # Signal Hound BB60 (the first one)
wvt --device skiq -c 915M -r 10M --gain 40               # Epiq Sidekiq (the first free card)
wvt --device skiq:8N3H0 -c 2.4G -r 50M --antenna J1      # Sidekiq with serial number 8N3H0, RF port J1
wvt --device sim -c 100M -r 2.4M                         # built-in simulator, no hardware
```

To see the devices that wvt finds, type `wvt --list-devices`.

## Device limits

| | RTL-SDR v3 | USRP | Signal Hound BB60 | Epiq Sidekiq |
|---|---|---|---|---|
| Tuning | 24 MHz to 1.766 GHz. HF below 24 MHz with direct sampling (Q) | The range of the device and its daughterboard | 9 kHz to 6.4 GHz | The range of the card, for example 70 MHz to 6 GHz on the M.2 |
| Sample rate | 225 kS/s to 300 kS/s, or 0.9 MS/s to 3.2 MS/s. 2.4 MS/s or less is reliable | The range of the device, up to 500 MS/s | 40 MS/s divided by a power of 2: 40, 20, 10 ... 4.883 kS/s | The range of the card: up to 61.44 MS/s (M.2, mPCIe, NV100) or 245.76 MS/s (X2, X4) on PCIe. Much less on USB |
| Gain | 0 dB to 49.6 dB in tuner steps, or AGC | The range of the device. AGC on the devices that have it | A reference level, −130 dBm to +20 dBm | dB above the lowest gain index: 0 dB to 76 dB in 1 dB steps (M.2, mPCIe), 0.5 dB steps on the other cards. AGC on the cards that have it |
| Options (⚙) | Bias-tee, direct sampling, PPM correction | Antenna, channel, reference, analog bandwidth | Attenuator, amplifier, I/Q bandwidth, external reference | RF port, RF bandwidth, external reference |
| Samples | `cu8` | `ci16` (sc16) | `ci16` (sc16) | `ci16` (sc16) |

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

### Sidekiq settings

Gain
: The gain in dB above the lowest gain index of the card. The M.2 and mPCIe (AD9361) have 1 dB
  steps. The X2, X4 and NV100 have 0.5 dB steps. The *AGC* box shows only if the card has automatic
  gain. If the RF input is more than 0 dBm, the status bar shows *RF overload*.

Sample rate
: Type a rate in the range of the card. The tooltip shows the range after the card opens. A USB
  card (mPCIe or M.2 on USB) streams much less than on PCIe. The device list shows the transport,
  for example *Sidekiq M.2 serial 8N3H0 (USB)*.

RF port
: The receive port. The list shows the fixed receive ports of the card after it opens. *Default*
  keeps the card's default port.

Bandwidth
: The RF bandwidth. 0 sets 80 % of the sample rate.

Ext ref
: Use the external reference clock input. The M.2 and mPCIe cannot change the reference while
  wvt runs. Set their reference with the `ref_clock` program of the SDK.

A retune and a gain change apply while the card streams. 12-bit cards (M.2, mPCIe) have the same
dBFS scale as 16-bit cards: a full-scale signal shows 0 dBFS.

wvt reads the USRP limits from the device when it opens, so it does not need a table of models.
These are typical values:

| USRP | Link | Tuning | Maximum rate | Receive channels |
|---|---|---|---|---|
| B200, B210, B200mini, B205mini | USB 3 | 70 MHz to 6 GHz | 56 MS/s (61.44 MS/s) | 1 (B210: 2) |
| N310 | 1 GbE, 10 GbE | 10 MHz to 6 GHz | 153.6 MS/s | 4 |
| X310 | 1 GbE, 10 GbE | Depends on the daughterboards (UBX: 10 MHz to 6 GHz) | 200 MS/s | 2 (one for each daughterboard slot) |
| X410 | 10 GbE, 100 GbE | 1 MHz to 7.2 GHz | 491.52 MS/s (500 MS/s) | 4 |

The real limit is often the link and the host. A 1 GbE link carries approximately 25 MS/s. Above
approximately 100 MS/s, the computer must also analyze and record the samples fast enough.

### USRP options

The ⚙ button next to **AGC** has these options. A change starts the stream again.

Antenna
: The receive port of the channel, for example `RX2` or `TX/RX`. The list comes from the device
  after it opens. *Default* uses the default port of the device.

Channel
: The receive channel. The X310 has one for each daughterboard slot, and the N310 and X410 have
  four. wvt streams one channel at a time. From the command line, use `--usrp-channel N`.

Reference
: The clock and time reference: *internal*, *external* (the 10 MHz and PPS inputs) or *gpsdo*.
  The reference applies to the full device, so the transmitter uses it too. From the command line,
  use `--ref SRC`.

Bandwidth
: The analog filter bandwidth. 0 sets it automatically. Not all daughterboards have this filter.

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
folders that do not exist. wvt does not replace a file: if the data file or the `.sigmf-meta`
file exists, the recording does not start.

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
- The storage must be sufficiently fast. For example, 2.4 MS/s `cu8` is 4.8 MB/s, a B205 at
  56 MS/s `ci16` is 224 MB/s, and an X310 at 200 MS/s is 800 MB/s.
- The **buffer** value shows how far the writer is behind. If the buffer is full, wvt drops samples
  and adds an annotation for the gap.
- **Open recording** stops the device and opens the last recording in file mode.

### Record without a window

`--no-gui` records without a window, for example on a computer without a display. Press Ctrl+C to
stop.

```sh
wvt --device uhd -c 915M -r 10M --record ~/caps/ism --record-duration 30 --no-gui
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

Thus a slow disk does not stop the spectrum, and each loss in a recording has an annotation. The
buffer holds approximately 2 s of samples, from 16 MB to 1 GB.

The USRP backend uses the C API of UHD for all USRPs. When a USRP opens, wvt asks UHD for the
model, the number of channels, the antennas, the clock sources, and the ranges of the frequency,
rate, gain and bandwidth of the channel. The window uses these values for its lists and tooltips.
The receiver and the transmitter share one open device. The search for devices runs on a
background thread, because a network search takes approximately half a second. The self-check
`build/device_check` tests the device list entries and the rate rules.

For the design, refer to [the USRP models design record](design/usrp-models-plan.md).

The BB60 backend (`src/device/sdr_bb60.cpp`) uses `bb_api` from the Signal Hound SDK in I/Q streaming
mode with 16-bit samples. Its thread reads blocks of approximately 20 ms. For tests without the
SDK or a device, `-DWVT_BB_SIM=ON` builds the backend against a simulated BB60D
(`tools/bb_sim/`). The simulator makes a tone at 100.2 MHz and noise, and it reports an overload
when the tone is above the reference level. The self-check `build/bb_check` uses it. For the
design, refer to [the BB60 design record](design/bb60-integration-plan.md).

The Sidekiq backend (`src/device/sdr_skiq.cpp`) uses `libsidekiq` from the Sidekiq SDK. It
receives on handle A1 with 16-bit samples in I/Q order. It shifts the samples of a 12-bit card by
4 bits, so that full scale is 0 dBFS. Its thread collects the blocks from `skiq_receive` into
deliveries of approximately 20 ms. The RF timestamp of each block counts samples, so a jump is a
loss of that many samples. An overrun shows as a gap with the exact count. The RF timestamp of an
AD9361 card jumps when it retunes, so the gap check pauses for 0.5 s after a retune. The SDK
messages go to the wvt log (component `skiq`). `skiq_exit` stops the library for the whole
process, so wvt opens one Sidekiq at a time. For tests without the SDK or a card,
`-DWVT_SKIQ_SIM=ON` builds the backend against a simulated card (`tools/skiq_sim/`): an M.2, or an
X4 with `WVT_SKIQ_SIM_PART=x4`. It keeps up with 245.76 MS/s, and it reports an overrun when the
host falls more than 64 MiB behind. The self-check `build/skiq_check` uses it. For the design, refer
to [the Sidekiq design record](design/sidekiq-integration-plan.md).
