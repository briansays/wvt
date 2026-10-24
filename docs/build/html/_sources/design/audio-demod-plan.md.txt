# Plan: Audio demodulation and playback

Branch: `7-audio-demod` (from `main` at `b50b762`)
Status: **implemented in the branch (uncommitted).** Decisions are in section 6, test results and
remaining work in section 7.

## 1. Goal

Add an **Audio** window that demodulates one channel and plays it to the speakers:

- **File mode:** play a recording from a cursor, the current view, or the marked time range,
  with a playhead that follows on the plots.
- **Live mode:** play the device stream as it arrives (RTL-SDR, USRP, `sim`, or siggen
  loopback). Playback runs alongside recording.

Out of scope for now: FM stereo / RDS, more than one channel at a time, network audio output.

## 2. Starting point

| Area | What exists | Gap |
|---|---|---|
| Demodulation | `computeDemod()` (`src/dsp/engine.cpp`) — batch per view, cancellable, reduced for display | No demodulator that keeps filter / phase state across blocks, so no continuous audio |
| Live samples | `SampleRing` (`src/common/sample_ring.h`) supports extra lossy readers with their own cursor | `LiveEngine` does not expose `ring_`, the format, or the rate |
| Tuning | `demod_.chanEnable/chanOffset/chanBw` and the drag overlay, file mode only | Live mode is spectrum-only and has no tuner overlay |
| Tool windows | `drawSigGenWindow()` + `drawSigGenButton()` | Pattern to copy for a floating window and toolbar button |
| Audio output | None — the build links only X11, GL, and threads | Needs an output backend |

## 3. Design decisions

| Topic | Decision | Reason |
|---|---|---|
| Audio output | Vendor **miniaudio** (single header, public domain / MIT-0) in `third_party/` | CoreAudio on macOS; ALSA / PulseAudio / PipeWire on Linux, loaded at runtime, so no new build dependency. Matches the vendored ImGui / ImPlot approach |
| Build switch | `WVT_WITH_AUDIO` = `AUTO` / `ON` / `OFF`, defines `WVT_HAVE_AUDIO` | Same pattern as `WVT_WITH_RTLSDR` / `WVT_WITH_UHD`. With it off, WAV export still works |
| DSP | New stateful, block-based `AudioDemod` (`src/audio/audio_dsp.*`) | Keeps `computeDemod` and the viewer unchanged |
| Channelizer | NCO shift → cascade of half-band decimators → final FIR to the IF rate | A single direct FIR (as in `computeDemod`) is too costly in real time at 10–20 MS/s |
| Modes | AM, NFM, WFM (mono, 50 / 75 µs de-emphasis), USB, LSB, CW (BFO) | Covers the `sim` stations and common bands |
| Resampling | Polyphase / Farrow resampler with a variable ratio, to 48 kHz | Device and file rates are arbitrary |
| Clock drift (live) | PI loop on the audio ring fill level trims the resampler ratio (about ±500 ppm) | SDR and sound card clocks differ; without this, audio underruns or builds up delay |
| Threading | One DSP thread per audio session fills an SPSC float ring. The miniaudio callback only copies out (no locks, no allocation) and plays zeros on underrun | Keeps the audio callback real-time safe |
| Tuning | **"Follow viewer channel"** (default on) uses the existing channel offset / bandwidth; off gives the pane its own settings | One tuner instead of two that disagree |
| Window | Floating ImGui window "Audio", toolbar button and shortcut | Same as the Signal Generator |
| Live retune / gap | When `LiveEngine` `gen_` changes or the reader is lapped, reset the demod state and resync near `written()` | Avoids glitches and stale audio |

## 4. Work breakdown

### Phase 1 — Build and output backend
- [x] Vendor `third_party/miniaudio/miniaudio.h`; one `src/audio/audio_out.cpp` with `MINIAUDIO_IMPLEMENTATION`.
- [x] `src/audio/audio_out.h`: `AudioOut` — open / close, device list, 48 kHz stereo float, pull from the
      SPSC ring, underrun counter, latency.
- [x] CMake: `WVT_WITH_AUDIO`; link CoreAudio / AudioToolbox / CoreFoundation on macOS, `dl` / `m` on Linux.
- [x] `--list-audio-devices`.
- [x] Confirm early that audio plays from the XQuartz-linked binary on macOS.

### Phase 2 — Streaming DSP (`src/audio_dsp.{h,cpp}`)
- [x] `Nco`, `HalfbandDecim`, `FirDecim` (with saved history), `Resampler` (variable ratio),
      `DcBlock`, `Agc`, `Deemphasis`.
- [x] `AudioDemod::process(const cf* in, int64_t n, std::vector<float>& out)`, configured by
      `AudioParams` (mode, offset, bw, squelch dB, AGC, de-emphasis, volume, BFO).
- [x] Reuse `designLowpass()` and `getFFT()` from `dsp.h`.

### Phase 3 — Audio engine (`src/audio_engine.{h,cpp}`)
- [x] `AudioEngine` with one small source interface and two implementations:
  - `FileAudioSource`: `IqFile` + `FormatParams`, sample cursor, range [s0, s1], loop, seek,
    pause; reads ahead in ~20 ms blocks.
  - `LiveAudioSource`: lossy `SampleRing::read()` with its own cursor, starting ~100 ms behind
    `written()`; resyncs when lapped.
- [x] Drift PI loop (live mode only; files have no drift).
- [x] Stats for the UI: playhead, level (dBFS), underruns, resyncs, latency, squelch state.
- [x] `LiveEngine`: add `ring()`, `format()`, `rate()`, `gen()` accessors.

### Phase 4 — UI (`src/app/app_audio.cpp`, members in `src/app/app.h`)
- [x] Toolbar button "Audio" (`*` while playing) and **Ctrl+D** (Ctrl+A would conflict with
      select-all in text fields).
- [x] Window contents:
  - **Source:** File or Live, from the app mode.
  - **Tuning:** follow channel, offset, bandwidth.
  - **Demod:** mode, AGC, de-emphasis, BFO, squelch.
  - **Output:** device combo, volume, mute, level meter.
  - **Transport (file):** play / pause / stop, range (cursor / view / markers / whole file),
    loop, position slider.
  - **Status line:** latency, underruns, resyncs.
- [x] File mode: playhead line on the time plots and waterfall; modifier-click on a time plot
      seeks; optional "follow playhead" pans the view.
- [x] Live mode: channel overlay on the live spectrum and waterfall (drag line, shaded
      bandwidth) and click-to-tune, using the same `demod_.chanOffset`.
- [x] Stop audio when the source changes (open file, enter / leave live mode, device stop).

### Phase 5 — WAV export and CLI
- [x] "Save WAV…" for the selected file range, faster than real time; "Record audio" toggle in live mode.
- [x] `--audio MODE`, `--audio-out FILE.wav` (demodulate the file or `--view` range and exit,
      like `--gen-out`), `--audio-device NAME`, `--volume DB`.

### Phase 6 — Tests and docs
- [x] `tools/audio_check.cpp` (like `siggen_check`): synthesize AM / FM / SSB tones at a known
      offset, run `AudioDemod`, check tone frequency, SNR, and level; check resampler output
      length against the ratio; check that the drift loop converges for a ±200 ppm rate error.
- [x] Manual checks: `wvt --device sim` (built-in AM and FM stations) and iqgen files.
- [x] README: Audio section, build option, CLI flags, source-file table.
- [ ] Roadmap row (left for you: the table numbers versions, and 1.2 / 1.3 are not done yet).

## 5. Risks

| Risk | Mitigation |
|---|---|
| macOS audio from an XQuartz-linked process | CoreAudio works from any process; verify in Phase 1 |
| No audio device (`ssh -X`, Xvfb, headless) | Pane shows "no output device; WAV export only" and keeps working |
| High rates (USRP at 20+ MS/s) | Measure the half-band cascade; show "DSP behind" in the status line if it cannot keep up |
| Live latency vs. robustness | ~150 ms target (100 ms for files); not adjustable yet |

## 6. Decisions (formerly open questions)

| Question | Decision |
|---|---|
| Output backend | Vendor **miniaudio** |
| Window | **Floating** window, like the Signal Generator |
| Modes | AM, NFM, WFM (mono), USB, LSB, CW, as listed. WFM stereo is out of scope for this branch |
| WAV export | **In this version**: "Save WAV…", live "Record audio", and headless `--audio-out` |

## 7. Results and remaining work

Implemented as planned, with these details:

- Files: `src/audio/audio_dsp.*`, `src/audio/audio_engine.*`, `src/audio/audio_out.*`, `src/io/wav_writer.*`,
  `src/audio/miniaudio_impl.c`, `src/app/app_audio.cpp`, `tools/audio_check.cpp`, `third_party/miniaudio/`
  (0.11.25, header + license). Small changes in `app.*`, `app_live.cpp`, `live.h`, `main.cpp`,
  `recorder.*` (exports `makeDirs`), `CMakeLists.txt`, `README.md`.
- miniaudio on macOS uses `MA_NO_RUNTIME_LINKING` and links the CoreAudio frameworks; on Linux it
  loads the backends at run time (links only `dl`, `m`, threads). The `MA_NO_*` defines are on the
  `miniaudio` target as PUBLIC, so all files see the same configuration.
- For every mode, `offset` is the passband center, so the cyan / orange overlay is exactly the
  passband. USB puts the carrier at the lower edge, LSB at the upper edge.
- "Follow playhead" only pages views of 0.25 s or more; shorter views paged faster than the plots
  could be computed.
- Headless `--audio-out` with `--device` records live audio without opening a sound device
  (`AudioOutput::None`).
- `App::busy()` does not include audio; `App::audioBusy()` keeps the main loop running. This keeps
  `--screenshot` working while audio plays.

Verified:

- `build/audio_check`: all modes at 250 kS/s, 2.4 MS/s and 10 MS/s with blocks of random size. Tone
  within 1 Hz, SNR 37-70 dB, output length within 0.15 %, ratio trim exact (+500 ppm), squelch, and
  the WAV header. DSP speed at 10 MS/s: 10-15x real time (x86_64 build under Rosetta).
- Headless export from an `iqgen` file (WFM 1 kHz at 53 dB SNR; AM 400 Hz) and headless live
  recording from `--device sim` (WFM 50 dB, AM 400 Hz, 0 resyncs).
- 40 s live run against the MacBook speakers (muted): latency steady at 173-180 ms, drift trim
  -90 to -290 ppm, 0 underruns, 0 resyncs, DSP load 11-13 % for WFM at 2.4 MS/s.
- Screenshots of file playback (playhead, level meter, follow) and live playback (tuner overlay).
- Build with `-DWVT_WITH_AUDIO=OFF`: compiles, reports "built without audio output", WAV export works.

Checked by hand: playback with the simulator and with a handheld radio.

Not yet verified by hand:

- [ ] Listening test of each mode.
- [ ] Shift+click to play, dragging the playhead and the position slider, Pause / Resume, Loop.
- [ ] Live "Record audio" button, device combo, audio across a retune and a stream restart.
- [ ] Real RTL-SDR / USRP streams (only the simulator was tested).
- [ ] A Linux build (only macOS was built here).

Possible follow-ups: adjustable live latency, FM stereo, audio spectrum / scope in the window.
