# Plan: run-time configuration file (issue #88)

Status: stages 1 and 2 of 4 implemented (2026-10-25) on `88-config-file`. wvt reads the file for
the folders, the formats, the FFT and live values, the GPU, the UI scale and size, and the log
folder. The SDR keys and the transmit limits are parsed but not used yet.

## 1. Goal

From the issue:

- One file, read at start, that sets the defaults of wvt.
- It sets the recording, snapshot and signal generator folders, the default SDR, a configuration
  for each SDR (center frequency, port, sample rate, gain, AGC and so on), the FFT parameters and
  the window, and the default file format.
- Each line is `variable = value`. Text after `#` is a comment.

## 2. What wvt does now

The defaults are in the code. Some can be changed with options or environment variables, but
nothing is kept between runs except the ImGui window layout (`imgui.ini`).

| Setting | Now | Where |
|---|---|---|
| Recording prefix | `~/wvt-recordings/wvt` | `RecordConfig::prefix` (`src/io/recorder.h`) |
| Snapshot folder | `~/wvt-recordings/` | `App::snapDir_` (`src/app/app.h`) |
| Signal generator file prefix | `~/wvt-recordings/siggen` | `App::App` (`src/app/app.cpp`) |
| Audio WAV prefix | `~/wvt-recordings/audio` | `App::App` |
| Device | The first device that a scan finds | `App::scanDevices` (`src/app/app_live.cpp`) |
| Rate, gain after a device change | 10 MS/s (USRP, BB60, Sidekiq) or 2.4 MS/s; 30 dB (BB60: -20 dBm) | `App::selectDevice` |
| File waterfall FFT | N 1024, Blackman-Harris, 50 % overlap | `App::nfft_`, `window_`, `overlapPct_` |
| Live spectrum | N 2048, Blackman-Harris, avg auto, 30 rows/s, 2048 columns | `LiveParams` (`src/device/live.h`) |
| Recording format | SigMF, native samples | `RecordConfig`, `--record-format`, `--record-sample` |
| Generator file format | cf32 | `--gen-sample` |
| GPU, UI scale, log folder, TX limits | Options and `WVT_GPU`, `WVT_SCALE`, `WVT_LOG_DIR`, `WVT_TX_MAX_GAIN`, `WVT_TX_ALLOW` | `src/app/main.cpp` |

## 3. Decisions

The rows marked **agreed** differ from the issue text; the owner agreed to them on 2026-10-25.

| Question | Decision | Why |
|---|---|---|
| File location | `--config FILE`, else `$WVT_CONFIG`, else `$XDG_CONFIG_HOME/wvt/wvt.conf`, else `~/.config/wvt/wvt.conf`. The same path on macOS and Linux | One documented place. `~/Library/Application Support` is the macOS custom, but wvt is a terminal-started X11 program on both |
| No file | Not an error. The built-in defaults apply | A new install must start without a file |
| `--no-config` | Read no file | For tests and for a known state |
| Order (low to high) | Built-in default, config file, environment variable, command-line option, change in the GUI | The more specific and the more recent wins. `WVT_GPU=off wvt` keeps its meaning |
| Key names (**agreed**) | Lowercase, dotted groups: `recording.folder`, `fft.size`, `sdr.0.rate`. No `wvt_` prefix | The file is already wvt's. Groups sort and read well. The issue's `0-wvt_sdr_type` becomes `sdr.0.type` |
| `wvt_sdr_count` (**agreed**) | Not used. The profiles are the `sdr.N.*` indexes that are in the file | A count can disagree with the lines. A missing index is not an error |
| Default SDR | `sdr.default = b205` (a type), or `sdr.default = 1` (a profile index) | The issue gives a type. An index is needed when there are two of one type (two X310s) |
| Profile match | A scanned device uses the first profile whose `type` matches its model and, if set, whose `serial` (or `args`) matches | Per-type defaults as the issue asks, with a per-unit override |
| Types | `b200`, `b205`, `b210`, `x310`, `n310`, `x410`, `usrp` (any USRP), `bb60`, `mpcie`, `m2`, `m2-2280`, `z3u`, `x2`, `x4`, `nv100`, `sidekiq` (any card), `rtlsdr`, `sim` | The models that wvt supports now. Matched without case against the model that the scan reports (`B205mini` matches `b205`) |
| Units (**agreed**) | A number is in base units (Hz, S/s, s, dB), written plainly or in scientific notation: `61.44e6` (61.44 MS/s), `915e6`, `2.4e6`, `-20`. Letter suffixes (`61.44M`) are not accepted in the file; the warning shows the scientific form (`use 61.44e6`) | The issue writes `sample_rate_sps = 61.44`, which is 61.44 S/s if read literally. One unambiguous form; `M` (mega) and `m` (milli) cannot be mixed up |
| Paths (**agreed**) | `~` expands. After expansion the path must be absolute; a relative path is a warning and the default stays | The issue says absolute but gives `~/wvt/...`. Accept both forms that the issue shows |
| Folder vs prefix | The file sets folders (`recording.folder`); the file name start stays a separate key (`recording.name = wvt`) | The issue asks for folders. wvt uses prefixes inside |
| Comments | `#` at the start of a line, or after a space or tab. Values can be in double quotes | `skiq:#0` and URLs contain `#` |
| Bad key or value | Warning with file and line, for example `wvt.conf:12: unknown key 'fft.sise' (did you mean 'fft.size'?)`. The rest of the file still applies | A typo must not stop a field session. `--config FILE` with a missing file is an error, because it was asked for |
| Duplicates | The last line wins, with a warning | The same as most config formats |
| Device limits | Checked when the device opens (the same checks as the GUI and the CLI). A rate the device cannot do is snapped, with a warning | wvt knows the device range only after the scan |
| Transmit limits (**agreed**) | `tx.max_gain_db` and `tx.allow` only make the limits stricter: the strictest of file, environment and option applies | A safety limit must not be loosened by an option that someone forgot about |
| Sessions | A file session (docs/sessions.md) still restores its own values over the config file | A session is a saved state, more specific than defaults |
| Writing the file | `wvt --write-config FILE` writes a complete file with all keys, their defaults and comments, then exits. It does not overwrite without `--force` | The example in §5 is that output. Users start from it |
| Showing what applies | `wvt --show-config` prints each key, its value and where it came from (default, file:line, env, option), then exits | Fast to debug "why is my rate wrong" |
| GUI | Not in this issue. A later **File → Save settings as defaults** can write the same format | Keep the first step small |

## 4. Implementation notes

- `src/app/config.{h,cpp}`: the parser and the key table (in `src/app/`, because `src/common/`
  does not include other folders, and the table uses `WindowType` and `SampleFormat`). One table entry per key: name, type
  (path, frequency, number, whole number, choice, bool, text), default, help line, and where the
  value goes. The table drives the parse, `--write-config`, `--show-config` and the "did you mean"
  hint, as `CliParser` does for options (issue #54).
- Result: a `WvtConfig` struct (folders, `RecordConfig` defaults, `LiveParams`, the file FFT
  values, a vector of `SdrProfile`, the default SDR, GPU, scale, log folder, TX limits), filled
  before the option parse in `main()`. The options then change the struct.
- `App` takes the struct in its constructor and uses it in place of the literals in `App::App`,
  `app.h` and `selectDevice`. `selectDevice` applies the matching profile when the driver or the
  device changes. `scanDevices` picks the `sdr.default` device before the first real device.
- `DeviceInfo` gets a `product` field (`B205mini`, `X310`, `BB60C`, `X4`, ...) so that a profile can
  match before the device opens. The UHD and Sidekiq scans already know it; the BB60 and RTL-SDR
  scans have a fixed model.
- `--no-gui --record` with no `--device` uses `sdr.default`.
- The log folder key is read before `logInit`, so the config file is parsed first, and its
  warnings are kept and logged after `logInit`.
- `tools/config_check.cpp`: comments, quotes, `#` in values, spaces, scientific notation and the
  rejected suffixes, `~`, bad and unknown
  keys with line numbers, duplicates, profile matching, the order (file < env < option), and that
  `--write-config` output reads back to the defaults.
- Docs: a new user page `docs/config.md`; `docs/cli.md` gets `--config`, `--no-config`,
  `--write-config`, `--show-config`; the record index gets a row.

Work order:

1. Parser, key table, `config_check`. No behavior change.
2. Folders, formats, FFT and live values, GPU, scale, log folder.
3. SDR profiles and the default SDR (needs `DeviceInfo::product`).
4. `--write-config`, `--show-config`, TX limits, docs.

## 5. Example file

The output of `wvt --write-config ~/.config/wvt/wvt.conf`, with the SDR part filled in for a
B205mini, an X310, a BB60 and a Sidekiq X4. The commented-out lines show the defaults.

```ini
# wvt configuration file (issue #88)
# One "key = value" per line. "#" starts a comment (at the line start or after a space).
# Numbers are in base units (Hz, S/s, s, dB), in scientific notation: 61.44e6 = 61.44 MS/s.
# "~" is the home folder.
# Order: built-in default < this file < environment variable < command-line option.

# ---- folders ----------------------------------------------------------------
recording.folder  = ~/wvt/recordings/
recording.name    = wvt                  # file names: <name>_<date>_<time>_<fc>_<rate>
snapshot.folder   = ~/wvt/snapshots/
siggen.folder     = ~/wvt/siggen/
siggen.name       = siggen
audio.folder      = ~/wvt/audio/

# ---- file formats -----------------------------------------------------------
recording.format  = sigmf                # sigmf | raw
recording.sample  = native               # native (cu8 RTL-SDR, ci16 USRP/BB60/Sidekiq) | cf32
recording.max_seconds = 0                # 0 = no limit
siggen.format     = sigmf                # sigmf | raw
siggen.sample     = cf32                 # cf32 | ci16 | ci8 | cu8
export.band_sample = cf32                # region export with a band filter: cf32 | ci16

# ---- file waterfall FFT -----------------------------------------------------
fft.size          = 1024                 # power of two, 64 to 65536
fft.window        = blackman-harris      # rect | hann | hamming | blackman | blackman-harris | flat-top
fft.overlap_pct   = 50                   # 0 to 95
display.colormap  = viridis              # an ImPlot colormap: viridis | plasma | hot | cool | jet | twilight | greys ...
# display.db_min  = auto
# display.db_max  = auto

# ---- live spectrum ----------------------------------------------------------
live.fft_size     = 2048
live.window       = blackman-harris
live.avg          = auto                 # auto (all new samples) | 1 to 64
live.rows_per_sec = 30                   # 1 to 100
live.max_columns  = 2048                 # 64 to 8192; more bins are max-pooled
live.history_rows = 600

# ---- general ----------------------------------------------------------------
gpu               = auto                 # auto | on | off   (env WVT_GPU)
# gpu.device      = 0
ui.scale          = 1                    # 0.5 to 4         (env WVT_SCALE)
ui.geometry       = 1600x950
# log.folder      = ~/wvt/logs/          # (env WVT_LOG_DIR)

# ---- transmit limits (only stricter than the environment and the options) ---
# tx.max_gain_db  = 60
# tx.allow        = 902e6:928e6, 2.4e9:2.5e9

# ---- SDRs -------------------------------------------------------------------
# sdr.default: a type (the first profile and the first device of that type)
# or a profile index. Empty: the first device that the scan finds.
sdr.default = b205

# Profile 0: any B205mini.
sdr.0.type          = b205
sdr.0.fc            = 100e6
sdr.0.rate          = 61.44e6
sdr.0.gain_db       = 0
sdr.0.agc           = off
sdr.0.port          = TX/RX              # USRP antenna: TX/RX | RX2
sdr.0.bandwidth     = auto               # auto | Hz
sdr.0.ref           = internal           # internal | external | gpsdo

# Profile 1: one X310, by serial, on its second channel.
sdr.1.type          = x310
sdr.1.serial        = 31F4A2C
sdr.1.args          = "addr=192.168.40.2"
sdr.1.channel       = 1
sdr.1.fc            = 2.45e9
sdr.1.rate          = 50e6
sdr.1.gain_db       = 20
sdr.1.agc           = off
sdr.1.port          = RX2
sdr.1.ref           = external

# Profile 2: Signal Hound BB60. It has a reference level in dBm instead of a gain.
sdr.2.type          = bb60
sdr.2.fc            = 1.09e9
sdr.2.rate          = 40e6
sdr.2.ref_level_dbm = -20
sdr.2.atten         = auto               # auto | 0 | 10 | 20 | 30
sdr.2.amp           = auto               # auto | 0 to 3
sdr.2.bandwidth     = 27e6
sdr.2.ext_ref       = off

# Profile 3: Sidekiq X4, card 0, RF port J1.
sdr.3.type          = x4
sdr.3.args          = "#0"               # "#" in quotes is not a comment
sdr.3.fc            = 3.5e9
sdr.3.rate          = 61.44e6
sdr.3.gain_db       = 10
sdr.3.agc           = off
sdr.3.port          = J1
sdr.3.bandwidth     = auto
sdr.3.ext_ref       = off

# Profile 4: RTL-SDR v3 for HF.
sdr.4.type            = rtlsdr
sdr.4.fc              = 7.1e6
sdr.4.rate            = 2.4e6
sdr.4.agc             = on
sdr.4.direct_sampling = q                # off | i | q
sdr.4.bias_tee        = off
sdr.4.ppm             = 0
```
