# Plan: Audio demodulation and playback

Branch: `7-audio-demod` (from `main` at `b50b762`)
Status: **plan only; no implementation yet.** Open questions are in section 6.

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
| Demodulation | `computeDemod()` (`src/engine.cpp`) — batch per view, cancellable, reduced for display | No demodulator that keeps filter / phase state across blocks, so no continuous audio |
| Live samples | `SampleRing` (`src/sample_ring.h`) supports extra lossy readers with their own cursor | `LiveEngine` does not expose `ring_`, the format, or the rate |
| Tuning | `demod_.chanEnable/chanOffset/chanBw` and the drag overlay, file mode only | Live mode is spectrum-only and has no tuner overlay |
| Tool windows | `drawSigGenWindow()` + `drawSigGenButton()` | Pattern to copy for a floating window and toolbar button |
| Audio output | None — the build links only X11, GL, and threads | Needs an output backend |

## 3. Design decisions

| Topic | Decision | Reason |
|---|---|---|
| Audio output | Vendor **miniaudio** (single header, public domain / MIT-0) in `third_party/` | CoreAudio on macOS; ALSA / PulseAudio / PipeWire on Linux, loaded at runtime, so no new build dependency. Matches the vendored ImGui / ImPlot approach |
| Build switch | `WVT_WITH_AUDIO` = `AUTO` / `ON` / `OFF`, defines `WVT_HAVE_AUDIO` | Same pattern as `WVT_WITH_RTLSDR` / `WVT_WITH_UHD`. With it off, WAV export still works |
| DSP | New stateful, block-based `AudioDemod` (`src/audio_dsp.*`) | Keeps `computeDemod` and the viewer unchanged |
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
- [ ] Vendor `third_party/miniaudio/miniaudio.h`; one `src/audio_out.cpp` with `MINIAUDIO_IMPLEMENTATION`.
- [ ] `src/audio_out.h`: `AudioOut` — open / close, device list, 48 kHz stereo float, pull from the
      SPSC ring, underrun counter, latency.
- [ ] CMake: `WVT_WITH_AUDIO`; link CoreAudio / AudioToolbox / CoreFoundation on macOS, `dl` / `m` on Linux.
- [ ] `--list-audio-devices`.
- [ ] Confirm early that audio plays from the XQuartz-linked binary on macOS.

### Phase 2 — Streaming DSP (`src/audio_dsp.{h,cpp}`)
- [ ] `Nco`, `HalfbandDecim`, `FirDecim` (with saved history), `Resampler` (variable ratio),
      `DcBlock`, `Agc`, `Deemphasis`.
- [ ] `AudioDemod::process(const cf* in, int64_t n, std::vector<float>& out)`, configured by
      `AudioParams` (mode, offset, bw, squelch dB, AGC, de-emphasis, volume, BFO).
- [ ] Reuse `designLowpass()` and `getFFT()` from `dsp.h`.

### Phase 3 — Audio engine (`src/audio_engine.{h,cpp}`)
- [ ] `AudioEngine` with one small source interface and two implementations:
  - `FileAudioSource`: `IqFile` + `FormatParams`, sample cursor, range [s0, s1], loop, seek,
    pause; reads ahead in ~20 ms blocks.
  - `LiveAudioSource`: lossy `SampleRing::read()` with its own cursor, starting ~100 ms behind
    `written()`; resyncs when lapped.
- [ ] Drift PI loop (live mode only; files have no drift).
- [ ] Stats for the UI: playhead, level (dBFS), underruns, resyncs, latency, squelch state.
- [ ] `LiveEngine`: add `ring()`, `format()`, `rate()`, `gen()` accessors.

### Phase 4 — UI (`src/app_audio.cpp`, members in `src/app.h`)
- [ ] Toolbar button "Audio" (`*` while playing) and a shortcut (check that the key is free;
      Ctrl+A conflicts with select-all in text fields).
- [ ] Window contents:
  - **Source:** File or Live, from the app mode.
  - **Tuning:** follow channel, offset, bandwidth.
  - **Demod:** mode, AGC, de-emphasis, BFO, squelch.
  - **Output:** device combo, volume, mute, level meter.
  - **Transport (file):** play / pause / stop, range (cursor / view / markers / whole file),
    loop, position slider.
  - **Status line:** latency, underruns, resyncs.
- [ ] File mode: playhead line on the time plots and waterfall; modifier-click on a time plot
      seeks; optional "follow playhead" pans the view.
- [ ] Live mode: channel overlay on the live spectrum and waterfall (drag line, shaded
      bandwidth) and click-to-tune, using the same `demod_.chanOffset`.
- [ ] Stop audio when the source changes (open file, enter / leave live mode, device stop).

### Phase 5 — WAV export and CLI
- [ ] "Save WAV…" for the selected file range, faster than real time; "Record audio" toggle in live mode.
- [ ] `--audio MODE`, `--audio-out FILE.wav` (demodulate the file or `--view` range and exit,
      like `--gen-out`), `--audio-device NAME`, `--volume DB`.

### Phase 6 — Tests and docs
- [ ] `tools/audio_check.cpp` (like `siggen_check`): synthesize AM / FM / SSB tones at a known
      offset, run `AudioDemod`, check tone frequency, SNR, and level; check resampler output
      length against the ratio; check that the drift loop converges for a ±200 ppm rate error.
- [ ] Manual checks: `wvt --device sim` (built-in AM and FM stations) and iqgen files.
- [ ] README: Audio section, build option, CLI flags, source-file table, roadmap row.

## 5. Risks

| Risk | Mitigation |
|---|---|
| macOS audio from an XQuartz-linked process | CoreAudio works from any process; verify in Phase 1 |
| No audio device (`ssh -X`, Xvfb, headless) | Pane shows "no output device; WAV export only" and keeps working |
| High rates (USRP at 20+ MS/s) | Measure the half-band cascade; show "DSP behind" in the status line if it cannot keep up |
| Live latency vs. robustness | Default ~150 ms target, adjustable |

## 6. Open questions

1. Is it OK to vendor miniaudio, or should we write native ALSA + CoreAudio backends instead
   (more code to maintain)?
2. Floating window (like the Signal Generator) or a docked section in the right column under
   the AM / FM / PM plots?
3. Is the mode list right? WFM stereo in this branch or later?
4. WAV export and headless `--audio-out` in the first version, or afterwards?
