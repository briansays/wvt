# Plan: support other Ettus USRPs (issue #11)

Status: implemented (2026-10-20) on `11-usrp-models`. Not yet run on a network USRP. The user
pages are in [Live devices](../live.md) and [Signal generator](../siggen.md).

## 1. Goal

From the issue: expand the USRP support from the B200 / B205mini to other products, for example
the N310, X310 and X410.

## 2. Starting point

The USRP backend (`src/device/sdr_uhd.cpp`) used the UHD C API, so most calls already worked for any
USRP. These parts were written for the B2xx:

| Area | B2xx assumption |
|---|---|
| Device search | `uhd_usrp_find("type=b200")`: USB B2xx only. The default arguments were `type=b200` |
| Channel | Always channel 0 |
| Antennas | The UI offered `RX2` and `TX/RX`. The transmitter always used `TX/RX` |
| Rate check | A fixed limit of 61.44 MS/s, and a USB 3 warning above 8 MS/s |
| Full duplex | RX and TX must have the same rate (true only for the B2xx) |
| Messages | "B200/B205: 70 MHz - 6 GHz" in tune errors; USB 3 advice for overflows |
| Tooltips | B2xx rates, gains and bandwidths |
| Sample ring | 256 MB at most (0.3 s at 200 MS/s) |

## 3. Decisions

| Question | Decision | Why |
|---|---|---|
| Model table or device queries | Query the device: antennas, channels, clock sources, and the frequency, rate, gain and bandwidth ranges of the channel | No table to keep up to date. Daughterboards change the X310's limits |
| Device search | `uhd_usrp_find("")` (USB and network) on a background thread in the window. `--list-devices` stays synchronous | A network search takes about 0.5 s; the UI must not stop |
| List entries | Network devices by `addr=`, USB devices by `serial=` | An address opens without a new broadcast search |
| Default arguments | Empty: the first USRP UHD finds | `type=b200` excluded every other model |
| Channels | One channel at a time, selected by index (`--usrp-channel N`, a UI field) | Multi-channel streams need a different data path (one ring per channel) |
| Subdevice specification | Not exposed. The channel index uses the default specification | The default covers the normal set-ups. It can be added later |
| Reference | One setting for the clock and time source (`--ref internal\|external\|gpsdo`). A missing time source is a warning only | The reference belongs to the device, so RX and TX share it |
| Rate limit before open | 500 MS/s (the X410). A warning above 100 MS/s. The device coerces the rate when it opens | The model is not known before the device opens |
| Coerced rates | A note in the status bar. For network USRPs, the note names `master_clock_rate=` | Network USRPs divide a fixed master clock |
| Full duplex rate rule | Only on the B2xx (one clock for RX and TX) | Other USRPs allow a different rate in each direction |
| Transmit antenna | Empty means the first transmit antenna of the channel | `TX/RX` is not a port on the X410 (`TX/RX0`) |
| Option name | `--usrp-channel`, not `--channel` | `--chan` is the channel filter |
| UHD calls on two threads | A lock around `uhd_usrp_find` and `uhd_usrp_make` | The background search and an open can overlap |
| Sample ring | 2 s, from 16 MB to 1 GB | High rates need a larger buffer for the recorder |

## 4. Implementation notes

- `DeviceCaps` (`src/device/sdr_device.h`) carries what an open device supports. `SdrDevice::caps()`
  returns it; the USRP backend fills it after it opens. The window keeps the last value for its
  antenna and reference lists, the channel count, and the rate and bandwidth tooltips.
- `uhdDeviceInfo()` and `parseDeviceArgs()` are in `src/device/sdr_device.cpp`, without UHD, so
  `tools/device_check` tests them with search strings from each model family.
- `Usrp::usb` comes from the motherboard name (`B2…`). It selects the B2xx rate rule and the USB or
  network advice for overflows and underflows.
- The transmitter reports its antenna (`SdrTx::antenna()`), and the transmit status shows it.

## 5. Verification

| Check | State |
|---|---|
| Build without warnings, all self-checks, `device_check` | Done |
| No USRP attached: the search shows "USRP (none detected)", the open error names the USB and network causes | Done (macOS, UHD 4.11) |
| The USRP options popup: antenna, channel, reference, bandwidth | Done (Xvfb) |
| A B2xx still receives and transmits as before (default antenna RX2, TX/RX) | To do on hardware |
| X310 / N310 / X410: search by address, channel 1, external reference, coerced rate note, transmit antenna default | To do on hardware |
