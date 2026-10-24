# Command-line reference

```text
wvt [options] [file]
```

`wvt --help` shows the same list. Values in Hz and seconds accept SI suffixes, for example `2.4M`,
`250k`, `20u` or `5m`. A unit can follow the value, for example `2.4 MHz` or `10 MS/s`. wvt
does not accept other text after the value (`1.5.2` or `12abc`). Whole-number options (for
example `--seed`, `--gpu-device` or the `N` in `--chain-add PATH:N`) must be whole numbers in
their range. For more information about an option, refer to the page in the last column.

## File and display

| Option | Function | Page |
|---|---|---|
| `-f`, `--format FMT` | `cf32`, `cf64`, `ci32`, `ci16`, `ci8` or `cu8`. Add `_be` for big-endian | [Viewer](viewer.md) |
| `-r`, `--rate HZ` | The sample rate | [Viewer](viewer.md) |
| `-c`, `--fc HZ` | The center frequency | [Viewer](viewer.md) |
| `-H`, `--header BYTES` | The number of bytes to skip at the start of the file | [Viewer](viewer.md) |
| `--swap-iq` | Swap I and Q | [Viewer](viewer.md) |
| `--chan OFF:BW` | Start with the channel filter on, for example `--chan 300k:150k` | [Viewer](viewer.md) |
| `--segment T0:T1` | Load only this part of the file, in seconds. `-1` is the start or the end of the file | [Segments](segments.md) |
| `--view T0:T1` | Start with this time range, for example `--view 0.5:0.52` | [Viewer](viewer.md) |
| `--software` | Use the software renderer | [Getting started](getting-started.md) |
| `--scale S` | The UI scale. Useful on a high-resolution display | |
| `--geometry WxH` | The initial window size (default 1600x950) | |
| `--screenshot F` | Show one frame, write it to `F` as a PPM file, then stop | |
| `--session-out F` | Open the file with the other options, write a file session to `F`, then stop | [File sessions](sessions.md) |
| `--gpu auto\|on\|off` | GPU processing | [GPU](gpu.md) |
| `--gpu-device N` | Use GPU `N` | [GPU](gpu.md) |
| `-h`, `--help` | Show the options, then stop | |

## Logging

| Option | Function | Page |
|---|---|---|
| `-v`, `--verbose [LEVEL]` | Show log messages on the console: `debug`, `info`, `warn` or `error` (default `info`) | [Logging](logging.md) |
| `--log-dir DIR` | The folder for the log file | [Logging](logging.md) |
| `--no-log-file` | Do not write a log file | [Logging](logging.md) |

## Live devices and recording

| Option | Function | Page |
|---|---|---|
| `--device DEV` | `rtlsdr[:INDEX\|SERIAL]`, `uhd[:ARGS]` (for example `uhd:addr=192.168.40.2`), `bb[:SERIAL]`, `skiq[:SERIAL\|#N]`, `sim` or `siggen` | [Live devices](live.md) |
| `--gain DB\|auto` | The receiver gain (default 30 dB) | [Live devices](live.md) |
| `--ref-level DBM` | BB60: the strongest expected input (default −20 dBm) | [Live devices](live.md#bb60-settings) |
| `--atten auto\|0\|10\|20\|30` | BB60: the attenuator (default `auto`) | [Live devices](live.md#bb60-settings) |
| `--bb-amp auto\|0-3` | BB60: the amplifier setting (default `auto`). The generator option `--amp` sets the amplitude | [Live devices](live.md#bb60-settings) |
| `--ext-ref` | BB60: lock to a 10 MHz reference on port 1. Sidekiq: use the external reference clock | [Live devices](live.md#bb60-settings) |
| `--antenna NAME` | The USRP receive antenna, for example `RX2` or `TX/RX`, or the Sidekiq RF port, for example `"J3 (RX A2)"` or `J1` | [Live devices](live.md#usrp-options) |
| `--usrp-channel N` | The USRP channel for receive and transmit (default 0) | [Live devices](live.md#usrp-options) |
| `--ref SRC` | The USRP clock and time reference: `internal`, `external` or `gpsdo` | [Live devices](live.md#usrp-options) |
| `--bandwidth HZ` | The USRP analog bandwidth, the BB60 I/Q bandwidth, or the Sidekiq RF bandwidth (default: automatic) | [Live devices](live.md) |
| `--bias-tee` | RTL-SDR: supply power to an LNA or an active antenna through the coax | [Live devices](live.md) |
| `--direct-sampling I\|Q` | RTL-SDR: HF direct sampling (v3: `Q`) | [Live devices](live.md) |
| `--ppm N` | RTL-SDR frequency correction | [Live devices](live.md) |
| `--list-devices` | Show the devices and the GPUs, then stop | [Live devices](live.md) |
| `--record PREFIX` | Record to `PREFIX_<date>_<time>_<fc>_<rate>.<ext>` | [Live devices](live.md#recording) |
| `--record-format sigmf\|raw` | The file type (default `sigmf`) | [Live devices](live.md#recording) |
| `--record-sample native\|cf32` | The sample format (default `native`) | [Live devices](live.md#recording) |
| `--record-duration T` | Stop after `T` seconds | [Live devices](live.md#recording) |
| `--no-gui` | Record without a window. Ctrl+C stops | [Live devices](live.md#recording) |

## Signal generator

For the full list, refer to [Signal generator](siggen.md#command-line).

| Option | Function |
|---|---|
| `--siggen` | Open the signal generator window |
| `--gen cw\|pulsed\|playback` | The mode. This option also opens the window |
| `--sweep`, `--level-sweep` and related options | Sweeps |
| `--pri`, `--prf`, `--pw` and related options | Pulses |
| `--play` and related options | Playback |
| `--chain`, `--chain-add`, `--chain-loops` | Chains |
| `--gen-out PREFIX` | Write to a file, then stop |
| `--tx` | Transmit with a USRP. wvt asks before it transmits |
| `--tx-dry-run`, `--tx-confirm` | Only show the plan, or transmit and do not ask |
| `--tx-max-gain`, `--tx-allow` | Transmit limits |

## Region export

| Option | Function | Page |
|---|---|---|
| `--extract-out PATH` | Write the region to `PATH`, then stop | [Export](export.md#command-line) |
| `--extract T0:T1` | The time range | [Export](export.md#command-line) |
| `--extract-band LO:HI` | Only this band | [Export](export.md#command-line) |
| `--extract-format cf32\|ci16` | The output format for a band | [Export](export.md#command-line) |
| `--extract-overwrite` | Replace output files that exist | [Export](export.md#command-line) |
| `--export-window` | Open the export window | [Export](export.md#command-line) |

## Audio

| Option | Function | Page |
|---|---|---|
| `--audio am\|nfm\|wfm\|usb\|lsb\|cw` | Open the audio window and play | [Audio](audio.md#command-line) |
| `--audio-out FILE.wav` | Demodulate to a WAV file, then stop | [Audio](audio.md#command-line) |
| `--audio-device NAME` | The sound output | [Audio](audio.md#command-line) |
| `--list-audio-devices` | Show the sound outputs, then stop | [Audio](audio.md#command-line) |
| `--volume DB` | The playback volume (default −6 dB) | [Audio](audio.md#command-line) |
| `--squelch DBFS` | Mute while the channel power is below this level | [Audio](audio.md#command-line) |

## Environment variables

| Variable | Function |
|---|---|
| `WVT_SCALE` | The UI scale, as `--scale` |
| `WVT_GPU` | `auto`, `on` or `off`, as `--gpu` |
| `WVT_LOG_DIR` | The folder for the log file, as `--log-dir` |
| `TMPDIR` | The folder for the log file, if `WVT_LOG_DIR` is not set |
| `WVT_TX_MAX_GAIN` | The maximum TX gain, as `--tx-max-gain` |
| `WVT_TX_ALLOW` | The bands where wvt can transmit, as `--tx-allow` |
| `WVT_GPU_FORCE_FAIL` | Set to `1` to make each GPU call fail. For tests of the CPU fallback |
| `DISPLAY` | The X server |
