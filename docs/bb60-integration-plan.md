# Plan: Signal Hound BB60 as a wvt live device

Branch: `wvt-sh-dev` (commit `72f9d8a`, compared against `main`)
Status: implementation in the branch; **not yet run against hardware**. This document
reconstructs the plan from the branch, and records review findings and remaining work.

## 1. Goal

Add the Signal Hound BB60D (and BB60C / BB60A) to the hardware device options, next to
RTL-SDR and USRP B2xx. The user can select it in the Device list or with `--device bb`, tune
it, set its reference level, view the spectrum and waterfall, and record SigMF or raw IQ.

Out of scope: BB60 sweep mode, triggers / GPS time stamps, the SM200/SM435 or other Signal
Hound devices, transmit.

## 2. Design decisions

| Topic | Decision | Reason |
|---|---|---|
| API | `bb_api` from the Signal Hound SDK, linked as an imported shared library | No distro package or pkg-config file exists |
| Build switch | `WVT_WITH_BB` = `AUTO` / `ON` / `OFF`, `SIGNALHOUND_SDK_DIR` hint | Same pattern as `WVT_WITH_RTLSDR` / `WVT_WITH_UHD` |
| Driver id | `"bb"`, device args = serial number | Short, matches `rtlsdr` / `uhd` naming |
| Sample format | `bbDataType16sc` → `SampleFormat::CI16` | Half the bandwidth of cf32; recorder and ring already handle ci16 (USRP) |
| Sample rate | 40 MS/s / 2^n, n = 0..13 (40 MS/s .. 4.883 kS/s), shown as a combo, requests snapped on a log scale | The BB60 only decimates by powers of 2 |
| "Gain" | Reused `StreamConfig::gain` as **reference level in dBm** (-130 .. +20); `GainRange` gets `label` / `unit` | The BB60 has no gain control; ref level picks atten/amp |
| Manual RF | `atten` (-1 auto, 0-3 → 0/10/20/30 dB) and `rfGain` (-1 auto, 0-3) | Expose `bbConfigureGainAtten` |
| Bandwidth | `StreamConfig::bandwidth` reused as the I/Q filter bandwidth; 0 = widest for the rate | Same field the USRP uses |
| External ref | `extRef` → `bbConfigureIO` port 1 = 10 MHz in | Lab use |
| Retune / ref-level change | Stop thread → `bbAbort` → configure → `bbInitiate` → restart thread with purge | The BB60 applies settings only at `bbInitiate` |
| Retune gap | Not reported as a gap; the recorder starts a new SigMF capture segment and the purge drops old samples | Matches how retunes already work for the other drivers |

## 3. Work breakdown (as implemented)

### Phase 1 — Build system (`CMakeLists.txt`)
- [x] `WVT_WITH_BB`, `SIGNALHOUND_SDK_DIR` cache variables.
- [x] `find_path(bb_api.h)` with SDK sub-folder suffixes; `find_library(bb_api)`.
- [x] Fallback glob for the versioned `libbb_api.so.5.0.x`, choosing x86-64 vs aarch64 from
      `CMAKE_SYSTEM_PROCESSOR`, skipping symlinks.
- [x] Imported target `bb_api`, adds `src/sdr_bb60.cpp`, defines `WVT_HAVE_BB`.
- [x] `ON` fails hard when not found; `AUTO` prints a status line.

### Phase 2 — Device abstraction (`src/sdr_device.h/.cpp`)
- [x] `StreamConfig`: `atten`, `rfGain`, `extRef`; comments for `bb` use of `args`, `gain`, `bandwidth`.
- [x] `GainRange`: `label`, `unit`.
- [x] `sdrDrivers()`, `enumerateDevices()` (with a "none detected" placeholder), `openDevice()` dispatch.
- [x] `checkSampleRate("bb")`: range check + warning above 10 MS/s (host-side filtering).
- [x] New `supportedRates(driver)` and `snapSampleRate(driver, rate)`.

### Phase 3 — Backend (`src/sdr_bb60.cpp`, new)
- [x] Open by first device or by serial; read device type and serial for the description.
- [x] Configure: ext ref (BB60D vs BB60C port constants), 16-bit I/Q, center, ref level,
      gain/atten, decimation + bandwidth (clamped to the per-decimation maximum).
- [x] Streaming thread: `bbGetIQUnpacked` in ~20 ms blocks (1 k – 256 k samples), purge on
      (re)start, sample-loss → `gap_(-1, …)`, `bbADCOverflow` → "ADC overload" note for 1 s,
      `bbDeviceConnectionErr` → fatal error.
- [x] `reconfigure()` restarts the stream for retune / ref level and rolls back on failure.
- [x] Reads back the actual rate with `bbQueryIQParameters`.
- [x] `enumerateBb()` via `bbGetSerialNumberList2`.

### Phase 4 — CLI (`src/main.cpp`)
- [x] `--device bb[:SERIAL]`, `--ref-level DBM`, `--atten auto|0|10|20|30`, `--amp auto|0-3`, `--ext-ref`.
- [x] Default rate 10 MS/s and ref level -20 dBm for `bb` when not given.
- [x] Help text updates.

### Phase 5 — GUI (`src/app_live.cpp`)
- [x] Driver name "BB60", default gain range as ref level.
- [x] Rate combo for drivers with discrete rates.
- [x] Gain slider uses `GainRange::label/unit`; for BB60 the value applies on slider release
      (each change restarts the stream).
- [x] Reset gain to a meaningful default when switching to / from `bb`.
- [x] BB60 toolbar row: Atten, Amp, Bandwidth, Ext ref (each restarts the stream).

### Phase 6 — Docs (`README.md`)
- [x] Build instructions for the SDK, udev / USB 3 / Spike notes, device table column,
      CLI options, ref-level behavior, source-file table.

## 4. Review findings (to fix or verify)

Ordered by expected impact.

1. **Platform support is not stated.** The Signal Hound SDK ships `bb_api` for Windows and
   Linux (x86-64, aarch64) only. wvt builds on Linux and macOS (XQuartz), so on macOS BB60
   support is always off, and the CMake glob only searches `.so*`. README should say
   "Linux only" in the BB60 build section and the device table.
2. **Device re-scan while a BB60 streams.** `bbGetSerialNumberList2` may not list a device that
   is already open. Then `App::scanDevices()` sees `found == false` and calls
   `selectDevice(devices_.front())`, which changes `devCfg_` while the BB60 still streams.
   Verify on hardware; if it happens, keep the open device in the list (add it from
   `liveStats_` when streaming) or skip re-selection while running.
3. **Max I/Q bandwidth table is approximate.** `maxBandwidth()` uses 0.8 × rate for decimation
   ≥ 4. Check against the SDK manual's table for `bbConfigureIQ` (some decimations allow less
   than 80%). If the API rejects the value, open fails with a generic error; clamp to the
   documented table instead.
4. **Retune marker timing relies on the first block taking ~20 ms.** `LiveEngine::setCenterFreq`
   sets `freshFrom_` and calls `rec_.noteRetune()` *after* `dev_->setCenterFreq()` returns, but
   the BB60 thread already restarted inside that call. The comment in `reconfigure()` assumes
   ~20 ms, but the block size is clamped to 262 144 samples, so the first block takes only
   ~6.5 ms at 40 MS/s and ~13 ms at 20 MS/s (plus `bbInitiate` latency). At low rates it is
   safe (1024 samples ≈ 210 ms at 4.88 kS/s). A more robust option: let the backend mark the boundary itself (e.g. a callback, or start
   the thread only after the engine records the marker).
5. **UI stall on retune.** `setCenterFreq` / `setGain` run on the UI thread and do join +
   `bbAbort` + `bbInitiate`. Measure; if > ~100 ms, move reconfigure to the device thread or
   debounce frequency edits.
6. **External reference on BB60A.** The code uses the BB60C port-1 constant for any non-D
   device. Confirm the BB60A accepts it, or disable *Ext ref* for the BB60A.
7. **`setGain` compares `refLevel_` without `cfgM_`.** Harmless today (UI thread only), but
   take the lock for consistency with `reconfigure()`.
8. **Style:** in `src/main.cpp` the `--ext-ref` branch shares a line with `--antenna`
   (`... dev.extRef = true; else if (a == "--antenna") ...`). Split it to match the
   surrounding one-option-per-branch layout.
9. **`--gain auto` with `--device bb`** sets `agc = true`, which the BB60 ignores. Either
   reject it with a message ("BB60: use --ref-level") or document it.

## 5. Remaining work

### 5.1 Code
- [ ] Fix items 1, 7, 8, 9 above (small).
- [ ] Decide and fix items 2–6 after the hardware test (5.3).
- [ ] Optional: compile-only stub of `bb_api.h` + empty library under `tools/` or a test folder,
      so CI and macOS can build `sdr_bb60.cpp` without the SDK.

### 5.2 Build verification
- [ ] Linux x86-64: build with `-DSIGNALHOUND_SDK_DIR=…` straight from the SDK folder
      (versioned `.so` only) and with the library installed in `/usr/local/lib`.
- [ ] Linux aarch64 (e.g. Raspberry Pi 5 / Jetson): confirm the glob picks the ARM library.
- [ ] `WVT_WITH_BB=ON` without the SDK: CMake fails with the expected message.
- [ ] `WVT_WITH_BB=AUTO` without the SDK (incl. macOS): builds, `--device bb` gives
      "built without bb support".

### 5.3 Hardware test matrix (BB60D, then BB60C if available)
| # | Test | Pass criterion |
|---|---|---|
| 1 | `wvt --list-devices` | BB60 listed with type and serial |
| 2 | `--device bb` and `--device bb:SERIAL` | Opens; title shows "Signal Hound BB60D serial …" |
| 3 | Every rate in the combo, 40 MS/s → 4.883 kS/s | Streams; status rate matches; no sustained loss ≤ 10 MS/s |
| 4 | 40 MS/s and 20 MS/s | Note CPU load and loss events; warning shown |
| 5 | Known CW from a generator at several ref levels | Correct frequency; "ADC overload" appears when ref level is too low |
| 6 | Atten / Amp manual vs auto | Noise floor and level change as expected |
| 7 | Bandwidth 0 and explicit values, incl. above max | Clamped with a note; no open failure |
| 8 | Retune while streaming and while recording SigMF | New capture segment per retune; no stale samples in the waterfall |
| 9 | Raw recording | Tuning locked; file opens in wvt with right rate / fc |
| 10 | `--record … --no-gui --record-duration 30` | File complete, length ≈ 30 s × rate |
| 11 | Ext ref with and without a 10 MHz source | Locks / clear error |
| 12 | Unplug while streaming | "BB60 stopped streaming (unplugged?)", no crash |
| 13 | Spike open at the same time | Clear "in use" error |
| 14 | Device re-scan while streaming (finding 2) | Selection does not change |
| 15 | Switch driver BB60 ↔ RTL-SDR / sim | Gain / ref level reset to sensible defaults |

### 5.4 Merge
- [ ] Update README with the hardware-test result (tested models, known limits).
- [ ] Squash or tidy commits; open a PR from `wvt-sh-dev` to `main`.

## 6. Files touched

| File | Change |
|---|---|
| `CMakeLists.txt` | `WVT_WITH_BB`, SDK discovery, imported target |
| `src/sdr_bb60.cpp` | New backend (≈ 320 lines) |
| `src/sdr_device.h/.cpp` | Config fields, `GainRange` label/unit, rate list + snap, dispatch |
| `src/main.cpp` | CLI options and defaults |
| `src/app_live.cpp` | Rate combo, ref-level slider, BB60 option row |
| `README.md` | Build, setup, usage, device table |
