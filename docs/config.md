# Configuration file

The configuration file sets the defaults of wvt: the folders, the file formats, the FFT values, the
GPU and a profile for each SDR. wvt reads the file when it starts.

## Make the file

1. Write a file with all the keys and their defaults:

   ```sh
   wvt --write-config ~/.config/wvt/wvt.conf
   ```

2. Open the file in a text editor and change the values that you need.
3. Start wvt. The log shows `read ~/.config/wvt/wvt.conf: N settings`.

wvt does not replace a file that exists. To replace it, add `--force`. To show the file on the
terminal, use `--write-config -`.

## Where wvt finds the file

wvt uses the first of these items:

1. The `--config FILE` option.
2. The `WVT_CONFIG` environment variable.
3. `$XDG_CONFIG_HOME/wvt/wvt.conf`.
4. `~/.config/wvt/wvt.conf`.

If you give the file with `--config` or `WVT_CONFIG` and the file does not exist, wvt stops. If the
file in item 3 or 4 does not exist, wvt uses the built-in defaults. To read no file, use
`--no-config`.

## The order of the settings

A setting can come from four sources. A later source wins over an earlier source:

1. The built-in default.
2. The configuration file.
3. An environment variable, for example `WVT_GPU`.
4. A command-line option, for example `--gpu`.

A change in the window applies after all of them. A [file session](sessions.md) also restores its
own values over the configuration file.

The transmit limits are different: the strictest limit applies. Refer to
[Transmit limits](siggen.md#transmit-limits).

To see each value and its source, use `--show-config`:

```text
$ WVT_GPU=off wvt --scale 1.5 --show-config
# configuration file: /home/user/.config/wvt/wvt.conf
fft.size              = 4096                            wvt.conf:14
gpu                   = off                             $WVT_GPU
ui.scale              = 1.5                             --scale
live.fft_size         = 2048                            default
```

## The format

- Each line has one `key = value`.
- `#` starts a comment at the start of a line, or after a space or a tab. `skiq#0` is a value.
- Put a value in double quotes if it has a `#` after a space, or spaces at its start or end. In
  quotes, write `\"` for a quote and `\\` for a backslash.
- Keys can be in upper case or lower case.
- Numbers are in base units: Hz, S/s, seconds and dB. Write large numbers in scientific notation,
  for example `61.44e6` for 61.44 MS/s. wvt does not accept SI suffixes in the file: for
  `61.44M`, the warning tells you to use `61.44e6`.
- A folder starts with `/` or `~/`.

If a line is not correct, wvt writes a warning with the file name and the line number, and it does
not use that line. wvt uses all the other lines. For example:

```text
wvt: warning: wvt.conf:12: unknown key 'fft.sise' (did you mean 'fft.size'?)
wvt: warning: wvt.conf:20: sdr.0.rate: '61.44M' is not a number; use 61.44e6 (base units, no suffix); the line is not used
```

If a key is in the file two times, wvt uses the last line, with a warning.

## Keys

### Folders

| Key | Default | Function |
|---|---|---|
| `recording.folder` | `~/wvt-recordings/` | The folder for recordings |
| `recording.name` | `wvt` | The start of a recording name: `<name>_<date>_<time>_<fc>_<rate>` |
| `snapshot.folder` | `~/wvt-recordings/` | The folder for [plot snapshots](snapshots.md) |
| `siggen.folder` | `~/wvt-recordings/` | The folder for signal generator files |
| `siggen.name` | `siggen` | The start of a signal generator file name |
| `audio.folder` | `~/wvt-recordings/` | The folder for audio WAV files |

### File formats

| Key | Default | Values |
|---|---|---|
| `recording.format` | `sigmf` | `sigmf`, `raw` |
| `recording.sample` | `native` | `native` (cu8 for RTL-SDR, ci16 for USRP, BB60 and Sidekiq), `cf32` |
| `recording.max_seconds` | `0` | The maximum length of a recording. `0` is no limit |
| `siggen.format` | `sigmf` | `sigmf`, `raw` |
| `siggen.sample` | `cf32` | `cf32`, `ci16`, `ci8`, `cu8` |
| `export.band_sample` | `cf32` | The format of a [region export](export.md) with a band filter: `cf32`, `ci16` |

### File waterfall

| Key | Default | Values |
|---|---|---|
| `fft.size` | `1024` | A power of two, 64 to 65536 |
| `fft.window` | `blackman-harris` | `rect`, `hann`, `hamming`, `blackman`, `blackman-harris`, `flat-top` |
| `fft.overlap_pct` | `50` | 0 to 95 |
| `display.colormap` | `viridis` | An ImPlot colormap, for example `viridis`, `plasma`, `hot`, `jet`, `greys` |
| `display.db_min`, `display.db_max` | `auto` | The dB range of the colors. A value turns off the automatic range |

### Live spectrum

| Key | Default | Values |
|---|---|---|
| `live.fft_size` | `2048` | A power of two, 64 to 65536 |
| `live.window` | `blackman-harris` | The same values as `fft.window` |
| `live.avg` | `auto` | `auto` (all the new samples) or 1 to 64 FFTs for each row |
| `live.rows_per_sec` | `30` | 1 to 100 |
| `live.max_columns` | `2048` | 64 to 8192 |
| `live.history_rows` | `600` | 64 to 4096 |

### General

| Key | Default | Function |
|---|---|---|
| `gpu` | `auto` | `auto`, `on` or `off`. Refer to [GPU processing](gpu.md) |
| `gpu.device` | The best GPU | The GPU number, as `--gpu-device` |
| `ui.scale` | `1` | The UI scale, 0.5 to 4 |
| `ui.geometry` | `1600x950` | The window size |
| `log.folder` | `$TMPDIR` or `/tmp` | The folder for the [log file](logging.md) |

### Transmit limits

| Key | Function |
|---|---|
| `tx.max_gain_db` | The maximum TX gain, in dB |
| `tx.allow` | The bands where wvt can transmit, for example `902e6:928e6, 2.4e9:2.5e9` |

These limits only add to the limits of `WVT_TX_MAX_GAIN`, `WVT_TX_ALLOW`, `--tx-max-gain` and
`--tx-allow`. An option cannot remove a limit of the file.

## SDR profiles

A profile sets the values of one SDR type or of one unit. A profile has the keys `sdr.N.<field>`,
where `N` is a number from 0 to 99.

| Field | Devices | Function |
|---|---|---|
| `type` | All | The SDR type. This field is necessary |
| `serial` | All | Only the unit with this serial number |
| `args` | All | Only the unit with these device arguments, for example `"addr=192.168.40.2"` |
| `fc` | All | The center frequency, in Hz |
| `rate` | All | The sample rate, in S/s |
| `gain_db` | USRP, Sidekiq, RTL-SDR | The gain, in dB |
| `ref_level_dbm` | BB60 | The reference level, in dBm |
| `agc` | USRP, Sidekiq, RTL-SDR | `on` or `off` |
| `port` | USRP, Sidekiq | The antenna (`TX/RX`, `RX2`) or the RF port (`J1`) |
| `bandwidth` | USRP, BB60, Sidekiq | The bandwidth in Hz, or `auto` |
| `ref` | USRP | `internal`, `external` or `gpsdo` |
| `channel` | USRP | The receive channel |
| `atten` | BB60 | `auto`, `0`, `10`, `20` or `30` |
| `amp` | BB60 | `auto` or 0 to 3 |
| `ext_ref` | BB60, Sidekiq | `on` or `off` |
| `direct_sampling` | RTL-SDR | `off`, `i` or `q` |
| `bias_tee` | RTL-SDR | `on` or `off` |
| `ppm` | RTL-SDR | The frequency correction |

If a field does not apply to the type, wvt writes a warning and does not use the field.

### Types

| Type | Devices |
|---|---|
| `b200`, `b205`, `b210`, `x310`, `n310`, `x410` | A USRP model. `b205` agrees with a B205mini |
| `usrp` | All USRPs |
| `bb60` | All Signal Hound BB60 devices |
| `mpcie`, `m2`, `m2-2280`, `z3u`, `x2`, `x4`, `nv100` | A Sidekiq part. The name must be the same: `x4` does not agree with an X40 |
| `sidekiq` | All Sidekiq cards |
| `rtlsdr` | All RTL-SDR devices |
| `sim` | The simulator |

### How wvt selects a profile

A device uses the first profile that agrees with it:

1. The type agrees with the model of the device.
2. If the profile has a `serial`, the serial number of the device is the same.
3. If the profile has `args`, the device arguments include them.

Put the profiles for one unit before the profiles for a type. `wvt --list-devices` shows
`[config sdr.N]` after each device that has a profile.

A Sidekiq card that has a serial number shows with its serial number, not with its card number.
For that card, use `serial`, not `args = "#0"`.

### The default SDR

`sdr.default` selects the device that wvt uses first:

- A type, for example `sdr.default = b205`: the first device of that type.
- A profile number, for example `sdr.default = 1`: the first device that profile 1 agrees with.

If the device is not connected, wvt writes a message in the log and uses the first device that it
finds.

### Where the profiles apply

- **In the window:** when wvt selects a device, it uses the profile of that device.
- **With `--device`:** the profile sets the device values. The options win over the profile. For
  example, `--device uhd -r 10e6` uses the profile but a rate of 10 MS/s.
- **With `--no-gui --record` and no `--device`:** wvt records from the `sdr.default` device.

## Example

```ini
recording.folder  = ~/wvt/recordings/
snapshot.folder   = ~/wvt/snapshots/
siggen.folder     = ~/wvt/siggen/
fft.size          = 2048
gpu               = auto

sdr.default = b205

# Any B205mini
sdr.0.type    = b205
sdr.0.fc      = 100e6
sdr.0.rate    = 61.44e6
sdr.0.gain_db = 0
sdr.0.port    = TX/RX

# One X310, on its second channel
sdr.1.type    = x310
sdr.1.args    = "addr=192.168.40.2"
sdr.1.channel = 1
sdr.1.fc      = 2.45e9
sdr.1.rate    = 50e6

# Signal Hound BB60
sdr.2.type          = bb60
sdr.2.rate          = 40e6
sdr.2.ref_level_dbm = -20
```

For a file with all the keys, use `wvt --write-config -`.
