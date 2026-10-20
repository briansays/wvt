# Plan: Epiq Solutions Sidekiq as a wvt live device

Branch: none yet. Suggested: `wvt-skiq-dev`, from `wvt-gpu-dev`.
Status: **plan only**. There is no code yet.
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
different between SDK versions:

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
| Handle | `skiq_rx_hdl_A1` only | Single-channel view. Multi-channel is out of scope |
| Build switch | `WVT_WITH_SKIQ` = `AUTO` / `ON` / `OFF`. Find the SDK with `-DSIDEKIQ_SDK_DIR=...`, then env `SIDEKIQ_SDK_DIR`, then `~/sidekiq_sdk_current` | Same pattern as `WVT_WITH_BB`. Same search order as SoapySidekiq |
| Library lookup | Run `bin/sidekiq-config --cflags --libs-static`. Keep the static link flags in their order on an imported target | The SDK has no pkg-config file. The link order of the static libraries is important |
| Sample format | `SampleFormat::CI16`. Set `skiq_iq_order_iq` so the data is I then Q, as in a ci16 file | The ring, the recorder and SigMF already handle ci16 (USRP, BB60) |
| Scaling | Shift each sample left by `16 - resolution` bits in the receive thread. A full-scale signal then reads 0 dBFS on 12-bit and 16-bit cards | wvt decodes ci16 as value / 32768. Without the shift, a 12-bit card reads 24 dB low |
| Gain | `StreamConfig::gain` in dB. The backend maps dB to a gain index per part type (table in §2). `GainRange` shows the dB range of the card | The user sets dB, as for the other drivers |
| AGC | `StreamConfig::agc` → `skiq_rx_gain_auto` if the card supports it. Else the Auto box is not shown | Same control as RTL-SDR and USRP |
| Bandwidth | `StreamConfig::bandwidth`. 0 = 80 % of the sample rate | Same field as USRP and BB60 |
| RF port | `StreamConfig::antenna` holds the port name from `skiq_rf_port_string`. Empty = card default | Reuse the USRP antenna field and combo |
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
- [ ] Get the Sidekiq SDK (4.26.0 or later if possible) from the Epiq support portal.
      Record the SDK version and the card types that are available for tests.
- [ ] Install the kernel modules and udev rules from the SDK. Check with the SDK test
      application `rx_samples` that a card streams.
- [ ] Answer the items in §2, "To check against the SDK manual". Update this plan.

### Phase 1 — Build system (`CMakeLists.txt`)
- [ ] `WVT_WITH_SKIQ` (`AUTO` / `ON` / `OFF`) and `SIDEKIQ_SDK_DIR` cache variables.
- [ ] Find `bin/sidekiq-config` in the order from §3. Run it for `--cflags`,
      `--libs-static` and `--version`. Make an `INTERFACE IMPORTED` target `sidekiq` that
      keeps the link flags in their order.
- [ ] If found: add `src/sdr_skiq.cpp`, define `WVT_HAVE_SKIQ`, and print
      `wvt: Sidekiq support enabled (SDK x.y.z)`. If not found: `ON` is a fatal error.
      `AUTO` prints a status line.
- [ ] Skip on macOS with a status line. The SDK is for Linux only.
- [ ] If an older SDK has no `sidekiq-config`, add a second lookup: `sidekiq_api.h` and
      `libsidekiq__<arch>.a` with the support libraries that the SDK manual lists.

### Phase 2 — Device abstraction (`src/sdr_device.h`, `src/sdr_device.cpp`)
- [ ] Comments for the `skiq` use of `args`, `gain`, `bandwidth`, `antenna` and `extRef`.
- [ ] `sdrDrivers()`, `enumerateDevices()` with a "Sidekiq (none detected)" entry, and
      `openDevice()` dispatch.
- [ ] `checkSampleRate("skiq")`: positive and ≤ 250 MS/s. A warning above 61.44 MS/s
      about disk speed for recording.
- [ ] `GainRange`: add `std::vector<std::string> ports` if the RF port combo needs a list
      from the device. Else use a new `SdrDevice::rfPorts()` method. Choose the smaller change.

### Phase 3 — Backend (`src/sdr_skiq.cpp`, new)
- [ ] `enumerateSkiq()`: `skiq_get_cards`, then for each card the serial number and part
      type. The label is, for example, `Sidekiq X4 serial 8N3H (PCIe)`.
- [ ] Open: find the card by serial number or `#N`. Check `skiq_is_card_avail`. Call
      `skiq_init`. Read the part type, resolution, rate range and gain index range.
- [ ] Configure: I/Q order, reference clock, RF port, rate and bandwidth, LO frequency,
      gain mode and gain index. Read back the actual rate with
      `skiq_read_rx_sample_rate_and_bandwidth`.
- [ ] Receive thread:
  - Get the block size from `skiq_read_rx_block_size`.
  - Call `skiq_receive` with a timeout. Check the handle and the block length.
  - Shift each sample by `16 - resolution` bits.
  - Check the timestamps for gaps.
- [ ] Errors: count the receive errors other than `no_data` and `overrun`. After 10 in
      sequence, set `fatalError()` and stop the thread. This occurs, for example, when the
      user removes the card.
- [ ] `setCenterFreq`, `setGain` (dB → index, clamp), `gainRange()` per part type.
- [ ] Overload poll and `notes()`.
- [ ] `stop()`: `skiq_stop_rx_streaming`, join the thread, `skiq_exit`.
- [ ] Put the pure logic in functions without SDK types. These are the dB-to-index
      mapping, the timestamp gap check and the sample shift. Phase 6 tests them without
      hardware.

### Phase 4 — CLI (`src/main.cpp`)
- [ ] `--device skiq[:SERIAL|#N]`. Reuse `--gain`, `--bandwidth`, `--antenna` (RF port)
      and `--ext-ref`.
- [ ] Default rate 10 MS/s for `skiq` when `-r` is not given.
- [ ] Help text: `--device` line, and "BB60 / Sidekiq" for `--ext-ref`.

### Phase 5 — GUI (`src/app_live.cpp`)
- [ ] Driver name "Sidekiq". Default gain range until a card is open.
- [ ] Rate tooltip: the card's range after open, and the USB note.
- [ ] Gain slider in dB, Auto box only if the card supports AGC.
- [ ] RF port combo from the card's port list (as the USRP antenna combo).
- [ ] Ext ref checkbox (as BB60). Apply on change, with a stream restart if Phase 0 shows
      that it is necessary.

### Phase 6 — Tests and docs
- [ ] `tools/skiq_check.cpp` tests the pure logic from Phase 3 without the SDK. Every
      build makes it. It tests:
  - dB-to-index for each part type, with clamping.
  - Timestamp gaps: no gap, one lost block, and wrap-around of the 64-bit counter.
  - The shift for 12-bit and 16-bit cards.
- [ ] Write a manual hardware test list. It covers:
  - Each available card type, on PCIe and on USB.
  - Rates from the minimum to the maximum of each card.
  - Retune while streaming, gain steps, and the overload note.
  - A 60 s recording at the highest rate that the disk supports.
  - Removal of the card while streaming.
- [ ] `README.md`, in Simplified Technical English as the rest of the README:
  - A build section for the SDK.
  - Device setup: kernel modules, udev rules, and one process per card.
  - A device table column, the CLI options, the source file table and roadmap row 1.3.

### Phase 7 — Optional: transmit from the signal generator
- [ ] `openSkiqTx` for `SdrTx` (`src/sdr_tx.h`): `skiq_tx_block_allocate`,
      `skiq_transmit`, `skiq_write_tx_LO_freq`, `skiq_write_tx_attenuation`,
      `skiq_start_tx_streaming` and `skiq_stop_tx_streaming`.
- [ ] Transmit gain as attenuation (the Sidekiq controls TX power with attenuation). The
      `SdrTx` gain range shows it as negative dB, or the UI shows "Attenuation".
- [ ] `openTx` dispatch, `--tx-args skiq[:SERIAL]`, and the signal generator window
      (`src/app_siggen.cpp` now accepts only `uhd`).
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
