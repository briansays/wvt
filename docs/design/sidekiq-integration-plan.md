# Plan: Epiq Solutions Sidekiq as a wvt live device

Issue: #4. Branch: `4-sidekiq` (from `main`).
Status: **receive implemented on `4-sidekiq` (2026-10-23)** against SDK 4.26.0, tested with a
simulated card, and built and linked against the real SDK on Linux x86-64 and ARM64 (§9).
**Not yet run against a card.** Transmit (Phase 7) is not done. Sections 1 to 8 are the plan;
§9 records the implementation and its differences from the plan.
Roadmap: version 1.3, "Support for EPIQ SDRs".

## 1. Goal

Add Epiq Solutions Sidekiq cards to the live device list, next to RTL-SDR, USRP B2xx and
BB60. The user can do these tasks:

- Select a Sidekiq card in the Device list, or with `--device skiq`.
- Tune the card, and set the sample rate, bandwidth, gain and RF port.
- View the spectrum and waterfall.
- Record SigMF or raw IQ.

A later phase adds transmit from the signal generator (§5, Phase 7).

Out of scope:

- Multi-channel receive (A1 + A2 together) and phase-coherent modes.
- 1PPS and GPS timing, and timed start.
- FPGA bitstream changes and user FPGA applications.
- Running wvt on the embedded Sidekiq products (Z2, Z3u, Z4, Matchstiq). wvt is an X11
  desktop program. These products run their own embedded Linux.

## 2. API choice

**Use libsidekiq directly.** It is the Epiq SDK library, with the header `sidekiq_api.h`.

| Option | Result |
|---|---|
| **libsidekiq (chosen)** | Same pattern as `bb_api` for the BB60: a vendor SDK, linked through CMake. Full control of block size, timestamps, overload flags and gain index. No new dependency other than the SDK |
| SoapySidekiq through SoapySDR | Adds SoapySDR as a dependency only for one device family. The gain mapping and timestamp checks are in the module, not in wvt. Useful as a reference for the API calls |
| gr-sidekiq | Needs GNU Radio. Not applicable |

The SDK is not open source. Epiq gives it to customers through its support portal. wvt cannot
include it in the repository or download it in CMake. The user must install it, as with the
Signal Hound SDK.

### Verified API facts

These facts come from SoapySidekiq (github.com/epiqsolutions/SoapySidekiq). That module
calls libsidekiq and needs SDK 4.26.0 or later.

| Topic | API |
|---|---|
| Find cards | `skiq_get_cards(skiq_xport_type_auto, &count, cards)` without `skiq_init` |
| Open | `skiq_init(skiq_xport_type_auto, skiq_xport_init_level_full, &card, 1)`, then `skiq_exit()` |
| Card is in use | `skiq_is_card_avail(card, &pid)` gives the process that holds the card |
| Identity | `skiq_read_serial_string`, `skiq_read_parameters` → `card_param.part_type`, `skiq_part_string` |
| Sample rate | `skiq_write_rx_sample_rate_and_bandwidth(card, hdl, rate, bw)`, `skiq_read_min_sample_rate`, `skiq_read_max_sample_rate` |
| Tuning | `skiq_write_rx_LO_freq(card, hdl, hz)` |
| Gain | `skiq_write_rx_gain_mode` (`skiq_rx_gain_auto` / `skiq_rx_gain_manual`), `skiq_read_rx_gain_index_range`, `skiq_write_rx_gain(card, hdl, index)` |
| I/Q order | `skiq_write_iq_order_mode(card, skiq_iq_order_iq)`. The default order is Q then I |
| Resolution | `skiq_read_rx_iq_resolution(card, &bits)`. Each card type can be different |
| Blocking receive | `skiq_set_rx_transfer_timeout(card, 100000)` (µs) makes `skiq_receive` block |
| Receive | `skiq_receive(card, &hdl, &block, &len)` → `skiq_rx_status_success`, `skiq_rx_status_no_data` or `skiq_rx_status_error_overrun` |
| Block layout | `SKIQ_RX_HEADER_SIZE_IN_BYTES` header, then interleaved `int16` I/Q in `block->data`. The header has `rf_timestamp` and `sys_timestamp` |
| Lost samples | `rf_timestamp` of a block must equal the previous one plus the samples per block |
| Block size | `skiq_read_rx_block_size(card, skiq_rx_stream_mode_balanced)` |
| Start / stop | `skiq_start_rx_streaming(card, hdl)`, `skiq_stop_rx_streaming(card, hdl)` |
| Other | `skiq_write_rx_rf_port_for_hdl`, `skiq_rf_port_string`, `skiq_write_ref_clock_select`, `skiq_read_rx_overload_state`, `skiq_read_rx_cal_offset`, `skiq_register_logging` |

Gain index mapping in SoapySidekiq (one step = one index):

| Part types | dB per step | dB = 0 at index |
|---|---|---|
| mPCIe, M.2, M.2-2280 (Stretch), Z2, Z3u | 1 | 0 |
| X2, X4, X40 | 0.5 | 195 |
| NV100, NVM2, Z4 | 0.5 | 187 |

wvt always clamps the index to the range from `skiq_read_rx_gain_index_range`.

### To check against the SDK manual

The SDK manual is in the SDK download. These items are not in SoapySidekiq, or they can be
different between SDK versions. §9.1 gives the answers from SDK 4.26.0:

- Does `skiq_set_rx_transfer_timeout` work on the USB transport? If not, the receive thread
  must poll and sleep for a short time.
- Can wvt call `skiq_write_rx_LO_freq` and `skiq_write_rx_gain` while the card streams?
- Does `skiq_write_ref_clock_select` apply at once, or only after a new `skiq_init`?
- Does each card type support `skiq_rx_gain_auto`?
- Does an SDK older than 4.26 supply `bin/sidekiq-config`? If not, CMake needs a second way to
  find the library (§3, Build).
- Do 12-bit cards put the sample in the low 12 bits (sign-extended) or in the high 12 bits?
  The scaling in §3 depends on this. A test with a known input level gives the answer.

## 3. Design decisions

| Topic | Decision | Reason |
|---|---|---|
| Driver id | `"skiq"`. Device args = serial number, or `#N` for card number N | Short. The serial number stays the same when the PCIe order changes |
| Target cards | Cards attached to an x86-64 Linux host: M.2, M.2-2280 (Stretch), NV100, X2, X4. mPCIe if available | wvt runs on a desktop. The embedded products are out of scope |
| Transport | `skiq_xport_type_auto`. PCIe gives full rate. USB (mPCIe and M.2) gives a low rate. Show the transport in the device description | One code path. The user sees why a rate is not possible |
| Handle | One handle at a time: the handle of the chosen RF port, A1 by default (issue #85). A2 and B2 use `skiq_chan_mode_dual` | Single-channel view. Multi-channel is out of scope |
| Build switch | `WVT_WITH_SKIQ` = `AUTO` / `ON` / `OFF`. Find the SDK with `-DSIDEKIQ_SDK_DIR=...`, then env `SIDEKIQ_SDK_DIR`, then `~/sidekiq_sdk_current` | Same pattern as `WVT_WITH_BB`. Same search order as SoapySidekiq |
| Library lookup | Run `bin/sidekiq-config --cflags --libs-static`. Keep the static link flags in their order on an imported target | The SDK has no pkg-config file. The link order of the static libraries is important |
| Sample format | `SampleFormat::CI16`. Set `skiq_iq_order_iq` so the data is I then Q, as in a ci16 file | The ring, the recorder and SigMF already handle ci16 (USRP, BB60) |
| Scaling | Shift each sample left by `16 - resolution` bits in the receive thread. A full-scale signal then reads 0 dBFS on 12-bit and 16-bit cards | wvt decodes ci16 as value / 32768. Without the shift, a 12-bit card reads 24 dB low |
| Gain | `StreamConfig::gain` in dB. The backend maps dB to a gain index per part type (table in §2). `GainRange` shows the dB range of the card | The user sets dB, as for the other drivers |
| AGC | `StreamConfig::agc` → `skiq_rx_gain_auto` if the card supports it. Else the Auto box is not shown | Same control as RTL-SDR and USRP |
| Bandwidth | `StreamConfig::bandwidth`. 0 = 80 % of the sample rate | Same field as USRP and BB60 |
| RF port | `StreamConfig::antenna` holds the port and its handle, `"J3 (RX A2)"` or `"J1 (TRX A1)"`, from `skiq_read_rx_rf_ports_avail_for_hdl` for every handle (issue #85). A TRX port sets `skiq_rf_port_config_trx` and receive operation. Empty = A1 on the card default | Reuse the USRP antenna field and combo |
| External reference | `StreamConfig::extRef` → `skiq_write_ref_clock_select(external)` | Same flag as the BB60 |
| Receive thread | One thread. `skiq_receive` with a 100 ms timeout. Copy and shift each block into a buffer of about 20 ms, then call `data_` | Same block size as the BB60 backend. Fewer ring writes |
| Lost samples | Check `rf_timestamp` continuity. A gap of N samples calls `gap_(N, "sample loss")`. `skiq_rx_status_error_overrun` calls `gap_(-1, "overrun")` | The ring and the recorder already count gaps |
| Overload | Read the overload state about 2 times per second. Show "ADC overload" in `notes()` for 1 s | Same as the BB60 backend |
| Retune | Write the LO frequency while the card streams, if the SDK allows it (§2). Else stop, write, start, and purge | The UI already starts a new capture segment on retune |
| SDK messages | `skiq_register_logging` writes SDK messages to stderr with a `wvt: skiq:` prefix. Errors also go to `notes()` | The SDK writes to stdout by default. That mixes with `--list-devices` output |
| Card in use | Before `skiq_init`, call `skiq_is_card_avail`. If another process holds the card, give its PID in the error | A clear message instead of a timeout |

## 4. Rates and limits

The backend reads the limits from the card after `skiq_init`. It does not use a fixed table.
`checkSampleRate("skiq")` only checks that the rate is positive and not more than 250 MS/s.
After open, the backend compares the rate with `skiq_read_min_sample_rate` and
`skiq_read_max_sample_rate`, and gives an error that names the card's range.

Expected limits (to check on hardware):

| Card | RF chip | Resolution | Typical maximum rate |
|---|---|---|---|
| M.2, mPCIe, M.2-2280 | AD9361 / AD9364 | 12 bits | 61.44 MS/s on PCIe, much less on USB |
| X2 | ADRV9009 | 16 bits | 245.76 MS/s |
| X4 | 2 × ADRV9009 | 16 bits | 245.76 MS/s per channel |
| NV100 | ADRV9002 | 16 bits | about 61.44 MS/s |

Rates above 20 MS/s need the GPU build for "FFT sees 100%" in live mode (see
`docs/design/cuda-gpu-plan.md`). Rates above 100 MS/s are more than the GPU path analyzes. The
recorder must write at 4 bytes × rate. At 245.76 MS/s, that is about 1 GB/s. The README
must tell the user to record to a fast NVMe disk, or to use a lower rate.

## 5. Work breakdown

### Phase 0 — SDK and hardware access
- [x] Get the Sidekiq SDK (4.26.0 or later if possible) from the Epiq support portal.
      Record the SDK version and the card types that are available for tests.
- [ ] Install the kernel modules and udev rules from the SDK. Check with the SDK test
      application `rx_samples` that a card streams.
- [x] Answer the items in §2, "To check against the SDK manual". Update this plan (§9.1).

### Phase 1 — Build system (`CMakeLists.txt`)
- [x] `WVT_WITH_SKIQ` (`AUTO` / `ON` / `OFF`) and `SIDEKIQ_SDK_DIR` cache variables.
- [x] Find `bin/sidekiq-config` in the order from §3. Run it for `--cflags`,
      `--libs-static` and `--version`. Make an `INTERFACE IMPORTED` target `sidekiq` that
      keeps the link flags in their order.
- [x] If found: add `src/device/sdr_skiq.cpp`, define `WVT_HAVE_SKIQ`, and print
      `wvt: Sidekiq support enabled (SDK x.y.z)`. If not found: `ON` is a fatal error.
      `AUTO` prints a status line.
- [x] Skip on macOS with a status line. The SDK is for Linux only.
- [x] If an older SDK has no `sidekiq-config`, add a second lookup: `sidekiq_api.h` and
      `libsidekiq__<arch>.a` with the support libraries that the SDK manual lists.

### Phase 2 — Device abstraction (`src/device/sdr_device.h`, `src/device/sdr_device.cpp`)
- [x] Comments for the `skiq` use of `args`, `gain`, `bandwidth`, `antenna` and `extRef`.
- [x] `sdrDrivers()`, `enumerateDevices()` with a "Sidekiq (none detected)" entry, and
      `openDevice()` dispatch.
- [x] `checkSampleRate("skiq")`: positive and ≤ 250 MS/s. A warning above 61.44 MS/s
      about disk speed for recording.
- [x] `GainRange`: add `std::vector<std::string> ports` if the RF port combo needs a list
      from the device. Else use a new `SdrDevice::rfPorts()` method. Choose the smaller change.

### Phase 3 — Backend (`src/device/sdr_skiq.cpp`, new)
- [x] `enumerateSkiq()`: `skiq_get_cards`, then for each card the serial number and part
      type. The label is, for example, `Sidekiq X4 serial 8N3H (PCIe)`.
- [x] Open: find the card by serial number or `#N`. Check `skiq_is_card_avail`. Call
      `skiq_init`. Read the part type, resolution, rate range and gain index range.
- [x] Configure: I/Q order, reference clock, RF port, rate and bandwidth, LO frequency,
      gain mode and gain index. Read back the actual rate with
      `skiq_read_rx_sample_rate_and_bandwidth`.
- [x] Receive thread:
  - Get the block size from `skiq_read_rx_block_size`.
  - Call `skiq_receive` with a timeout. Check the handle and the block length.
  - Shift each sample by `16 - resolution` bits.
  - Check the timestamps for gaps.
- [x] Errors: count the receive errors other than `no_data` and `overrun`. After 10 in
      sequence, set `fatalError()` and stop the thread. This occurs, for example, when the
      user removes the card.
- [x] `setCenterFreq`, `setGain` (dB → index, clamp), `gainRange()` per part type.
- [x] Overload poll and `notes()`.
- [x] `stop()`: `skiq_stop_rx_streaming`, join the thread, `skiq_exit`.
- [x] Put the pure logic in functions without SDK types. These are the dB-to-index
      mapping, the timestamp gap check and the sample shift. Phase 6 tests them without
      hardware.

### Phase 4 — CLI (`src/app/main.cpp`)
- [x] `--device skiq[:SERIAL|#N]`. Reuse `--gain`, `--bandwidth`, `--antenna` (RF port)
      and `--ext-ref`.
- [x] Default rate 10 MS/s for `skiq` when `-r` is not given.
- [x] Help text: `--device` line, and "BB60 / Sidekiq" for `--ext-ref`.

### Phase 5 — GUI (`src/app/app_live.cpp`)
- [x] Driver name "Sidekiq". Default gain range until a card is open.
- [x] Rate tooltip: the card's range after open, and the USB note.
- [x] Gain slider in dB, Auto box only if the card supports AGC.
- [x] RF port combo from the card's port list (as the USRP antenna combo).
- [x] Ext ref checkbox (as BB60). Apply on change, with a stream restart if Phase 0 shows
      that it is necessary.

### Phase 6 — Tests and docs
- [x] `tools/skiq_check.cpp` tests the pure logic from Phase 3 without the SDK. Every
      build makes it. It tests:
  - dB-to-index for each part type, with clamping.
  - Timestamp gaps: no gap, one lost block, and wrap-around of the 64-bit counter.
  - The shift for 12-bit and 16-bit cards.
- [x] Write a manual hardware test list. It covers:
  - Each available card type, on PCIe and on USB.
  - Rates from the minimum to the maximum of each card.
  - Retune while streaming, gain steps, and the overload note.
  - A 60 s recording at the highest rate that the disk supports.
  - Removal of the card while streaming.
- [x] `README.md`, in Simplified Technical English as the rest of the README:
  - A build section for the SDK.
  - Device setup: kernel modules, udev rules, and one process per card.
  - A device table column, the CLI options, the source file table and roadmap row 1.3.

### Phase 7 — Optional: transmit from the signal generator
- [ ] `openSkiqTx` for `SdrTx` (`src/device/sdr_tx.h`): `skiq_tx_block_allocate`,
      `skiq_transmit`, `skiq_write_tx_LO_freq`, `skiq_write_tx_attenuation`,
      `skiq_start_tx_streaming` and `skiq_stop_tx_streaming`.
- [ ] Transmit gain as attenuation (the Sidekiq controls TX power with attenuation). The
      `SdrTx` gain range shows it as negative dB, or the UI shows "Attenuation".
- [ ] `openTx` dispatch, `--tx-args skiq[:SERIAL]`, and the signal generator window
      (`src/app/app_siggen.cpp` now accepts only `uhd`).
- [ ] Underrun count from `skiq_read_tx_num_underruns` into `SdrTx::Stats`.

## 6. Risks

| Risk | Handling |
|---|---|
| No SDK on the development computer (macOS) | The backend compiles only on Linux with the SDK. Keep the pure logic separate and test it on every build (Phase 6) |
| The API is different between SDK versions | Set a minimum SDK version in CMake (4.26.0 if Phase 0 shows that it works). Print the SDK version in the status line and in `description()` |
| `libsidekiq` is static and has its own support libraries | Use the order from `sidekiq-config --libs-static` without change |
| Only one process can open a card | Check `skiq_is_card_avail` and name the other process |
| 12-bit scaling is wrong | Phase 0 test with a known input. `skiq_check` tests the shift |
| High rates (X2 / X4) are more than the host can record or analyze | Warnings in `checkSampleRate` and in the README. The "FFT sees" readout already shows the analyzed part |
| The SDK writes to stdout | `skiq_register_logging` before `skiq_init` |
| `skiq_exit` is global for the process | Only one Sidekiq device object at a time. `openDevice` refuses a second open with a clear message |

## 7. Acceptance criteria

1. A build without the SDK, and every macOS build, has no change in behavior.
2. `wvt --list-devices` shows each attached card with its type, serial number and transport.
3. `wvt --device skiq -c 915M -r 10M` shows a live spectrum. A signal generator tone at a
   known level reads within 3 dB of the expected dBFS on a 12-bit and a 16-bit card.
4. Retune and gain changes take effect in less than 200 ms with no fatal error.
5. A 60 s SigMF recording at 10 MS/s has no sample loss. wvt opens it with the same rate,
   frequency and format as the recording.
6. When the user removes a card (or the transport fails), the status bar shows the error and
   wvt continues to run.
7. `skiq_check` passes.

## 8. Open questions

- Which Sidekiq cards and SDK version are available for development and tests?
- Are the cards on PCIe, USB, or both?
- Is transmit (Phase 7) part of version 1.3, or later?
- Is a calibrated dBm readout wanted? `skiq_read_rx_cal_offset` gives the offset from dBFS
  to dBm for the current gain. This needs a new "dBm" mode in the spectrum axis.

## 9. Implementation record (2026-10-23)

The receive path (Phases 1 to 6) is on `4-sidekiq`. It was written against the SDK 4.26.0 headers
and manual (`sidekiq_api.h`, the HTML SDK manual, the `sidekiq_probe` example). No card was
available.

### 9.1 Answers from the SDK manual (4.26.0)

| Question | Answer | What wvt does |
|---|---|---|
| Does `skiq_set_rx_transfer_timeout` work on USB? | No. *Developing apps*: "not all transports support blocking receive"; `skiq_get_rx_transfer_timeout` gives `RX_TRANSFER_WAIT_NOT_SUPPORTED` on USB and custom transports | A 25 ms timeout (the manual's advice). If it fails or is not supported, the thread polls: it sleeps 0.5 ms after `no_data` |
| Retune and gain change while streaming? | Yes. `skiq_init` makes three mutexes per card, so the calls are thread-safe. On AD9361 cards (mPCIe, M.2, M.2-2280, Z2, Z3u) a retune or rate change stops the sample clock, and the RF timestamp slips (*Timestamp Slips within AD9361 Products*) | Write the LO while the card streams. No gap check for 0.5 s after a retune, so the slip is not counted as a loss |
| `skiq_write_ref_clock_select` at once or after `skiq_init`? | At once, but it starts a full RF initialization, so the RF settings must come after it. M.2 and mPCIe: not supported at run time (only the stored setting, with the `ref_clock` test app) | Write it first in `configure()`. On the M.2 and mPCIe, the error tells the user about `ref_clock` |
| `skiq_rx_gain_auto` on each card? | The API does not say which cards have it | Try `skiq_write_rx_gain_mode(auto)` after open. `GainRange::agc` = the result |
| `bin/sidekiq-config` in older SDKs? | 4.26.0 has it (`doc/sidekiq-config.md`). Older SDKs are unknown | Use it if present. Else link `lib/libsidekiq__<cpu>.a` with the flags of the 4.26.0 `share/sidekiq/sidekiq-config/<cpu>.conf`, and read the version from `LIBSIDEKIQ_VERSION_*` |
| 12-bit samples: low or high bits? | Low bits, sign-extended to 16 bits (`skiq_iq_order_t` in `sidekiq_types.h`) | Shift left by `16 - resolution` (×16 on 12-bit cards), as planned |

### 9.2 Differences from the plan

| Topic | Plan | Implementation | Reason |
|---|---|---|---|
| Gain mapping | dB = 0 at a fixed index per part type (195 on X2/X4, 187 on NV100), from SoapySidekiq | dB above the lowest index from `skiq_read_rx_gain_index_range`, with the step per part type (1 dB AD9361, 0.5 dB other cards) | The API documents the step but not an absolute 0 dB index. This needs no offset table that can be wrong |
| Overload | Poll `skiq_read_rx_overload_state` 2 times per second | Also the `overload` bit in each block header | The header bit is per block. The manual: USB blocks have the header word set to 0, so the poll stays for USB |
| RF ports | `GainRange::ports` or `SdrDevice::rfPorts()` | `DeviceCaps::antennas` (already there for the USRP) from `skiq_rx_param_t::fixed_rf_ports` | No new interface. TRX ports need `skiq_write_rf_port_config` and an Rx/Tx operation: out of scope |
| Device list | `skiq_get_cards`, serial and part type | `skiq_get_cards`, `skiq_is_card_avail`, then `skiq_init(basic)` for the free cards to read part, serial and transport, then `skiq_exit` (as `sidekiq_probe`) | `skiq_read_parameters` needs an initialized card. A card in use is listed as `#N` with the PID of the owner. While wvt has a card open, the list shows that card only |
| SDK messages | To stderr with a `wvt: skiq:` prefix | To the wvt log, component `skiq` (errors at ERROR, warnings at WARN, the rest at DEBUG) | The log already goes to the console and the log file |
| Overrun | `gap_(-1, "overrun")` | The next block's timestamp gives the exact count: `gap_(N, "Sidekiq overrun ...")` | Exact counts in the SigMF annotations |
| `syslog.h` | — | `sidekiq_types.h` includes `<syslog.h>`, whose `LOG_DEBUG` and `LOG_INFO` clash with the wvt log macros. `sdr_skiq.cpp` keeps the SDK levels and `#undef`s the two names before the wvt headers | Found when the backend was compiled against the real header |
| CMake search | `SIDEKIQ_SDK_DIR`, env, `~/sidekiq_sdk_current` | Also the newest `third_party/sidekiq_sdk/sidekiq_sdk_v*` (git-ignored) | The SDK for this work was extracted there |
| Minimum SDK | 4.26.0 if possible | 4.14.0 (`skiq_write_ref_clock_select`). Only 4.26.0 is tested | Older SDKs can work; CMake prints the version |
| README | Build section, device setup, table, roadmap | In `docs/live.md`, `docs/build.md`, `docs/cli.md`, `docs/self-checks.md`, `docs/source-layout.md`. The README lists the device | The README now points to the docs pages; it has no roadmap table |

### 9.3 Tests that ran

- `skiq_check` in every build: the gain mapping and its clamps, the lost-sample count (with the
  64-bit wrap and a timestamp that goes back), the 12-bit shift, the Q/I swap (also in place) and
  the rate rule.
- `skiq_check` with `-DWVT_SKIQ_SIM=ON` (macOS arm64): the device list, open by first card,
  serial and `#N`; the errors (unknown serial, card number, rate and LO range, port name, external
  reference on an M.2, card in use with its PID, no card); a second open refused; real-time
  streaming; the tone offset after a retune; the tone level at −30 dBFS on a 12-bit M.2 and a
  16-bit X4; no false gap from the simulated AD9361 timestamp slip (the test fails without the
  0.5 s pause); overload note on and off; AGC; restart after stop; USB with polled receive and
  polled overload; an overrun counted as exactly 5090 samples; a card removal as a fatal error.
- A 3 s headless SigMF recording from the simulated card at 5 MS/s: 60 MB, no gaps, `ci16_le`,
  the card in `core:hw`. A GUI screenshot under Xvfb shows the tone at −30 dBFS.
- High rates with the simulated X4 (`WVT_SKIQ_SIM_PART=x4`, macOS arm64, Apple M5, internal SSD),
  headless SigMF: 5 s each at 122.88, 200 and 245.76 MS/s, and 20 s at 245.76 MS/s (19.66 GB,
  20.3 s wall time, 0 gaps, the tone at +200 kHz and −30 dBFS at the start, middle and end of
  the file; about 60 % of one CPU core with the simulator). The simulator copies its signal from
  a table, and it reports an overrun when the host falls more than 64 MiB behind (an assumed DMA
  buffer size). `skiq_check` stalls the host for 0.5 s at 100 MS/s and checks that wvt counts the
  loss. This tests wvt's path from `skiq_receive` to the disk, not the PCIe and DMA of a real X4.
- A build with the real SDK 4.26.0 in Ubuntu 24.04 containers, x86-64 and ARM64, with
  `-DWVT_WITH_SKIQ=ON -DWVT_WERROR=ON`: CMake found the SDK in `third_party/`, `sidekiq-config`
  gave the static link flags, and `wvt` links and runs. With no card, `--list-devices` shows
  *Sidekiq (none detected)*, and `--device skiq` gives *no Sidekiq card found*.
- A macOS build without the SDK: `Sidekiq support disabled (the Sidekiq SDK is for Linux only)`,
  no change in behavior.

### 9.4 Hardware test list (still to do)

For each card type that is available (M.2 and X4 first), on PCIe, and on USB for the M.2:

1. `sidekiq_probe` sees the card. `wvt --list-devices` shows it with its part, serial number
   and transport.
2. `wvt --device skiq -c 915M -r 10M`: a signal generator tone at a known level reads within 3 dB
   of the expected dBFS (acceptance criterion 3). Do this on a 12-bit and a 16-bit card: this
   checks the shift.
3. Rates from the minimum to the maximum of the card. The error for a rate out of range names the
   card's range. On USB, find the highest rate without gaps and add it to `docs/live.md`.
4. Retune while streaming, many times: no gap is reported, the tone moves, and the change takes
   less than 200 ms (the log at DEBUG shows the tune time). On an X4, check if the timestamp
   slips; if not, the 0.5 s pause can be limited to AD9361 cards.
5. Gain steps across the range. The dB per step matches the slider. AGC on the cards that have it.
6. An input above 0 dBm (with care): the *RF overload* note shows, also on USB.
7. RF port and external reference (X4: with a 10 MHz source; M.2: the error message).
8. A 60 s SigMF recording at 10 MS/s: no sample loss; wvt opens it with the same rate, frequency
   and format (acceptance criterion 5). Then the highest rate that the disk supports.
9. A second program (`rx_samples`) holds the card: wvt names its PID.
10. Remove the card or the USB cable while streaming: the status bar shows the error, wvt keeps
    running (acceptance criterion 6).

### 9.5 Remaining work

- The hardware tests in §9.4.
- Phase 7 (transmit), if version 1.3 needs it (§8).
- TRX RF ports (`skiq_write_rf_port_config`), if a user needs them.
- A calibrated dBm readout from `skiq_read_rx_cal_offset` (§8).
