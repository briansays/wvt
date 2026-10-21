# Source layout

## Application

| File | Function |
|---|---|
| `src/main.cpp` | Command-line parsing, ImGui and ImPlot setup, the event loop |
| `src/platform_x11.*` | The Xlib window, input, clipboard, and GLX or software presentation |
| `src/sw_renderer.*` | The software rasterizer for ImGui draw data |
| `src/app.*` | The UI: toolbar, linked plots, parameter panels |
| `src/app_menu.cpp` | The menu bar, the keyboard help and the About window |
| `src/ui_util.*` | SI-unit inputs and formatters, the scrollbar widget, forms, the gear button, the color bar, the style |
| `src/log.*` | The logger |
| `src/json.*` | A small JSON reader (chain files) |

## File viewer and signal processing

| File | Function |
|---|---|
| `src/engine.*` | Worker threads, and the waterfall, channel filter, demodulator and overview calculations |
| `src/dsp.*` | FFT (Accelerate vDSP on macOS), windows, FIR design, parallel-for, smoothing |
| `src/iq_file.*` | The memory-mapped reader for all sample formats, and SigMF and file name detection |
| `src/extract.*` | Region export: checks, filter design, the background job |
| `src/app_export.cpp` | The export window, the waterfall right-click menu, region selection |

## GPU

| File | Function |
|---|---|
| `src/gpu.h`, `src/gpu_common.cpp` | The GPU interface, the `--gpu` rules, the fallback to the CPU |
| `src/gpu_backend.h` | The backend interface |
| `src/gpu_cuda.cu` | The CUDA backend: device setup, waterfall and live spectrum kernels with cuFFT |
| `src/gpu_metal.mm` | The Metal backend: device setup, one-pass FFT kernels, MPSGraph FFT, channel filter |
| `src/gpu_stub.cpp` | The backend for builds without a GPU |

## Live devices

| File | Function |
|---|---|
| `src/app_live.cpp` | The live mode UI: device and recording toolbar, live spectrum, scrolling waterfall |
| `src/live.*` | The live engine: the device, the sample ring, the recorder and the spectrum thread |
| `src/sample_ring.h` | The ring buffer between the device thread, the spectrum thread and the recorder |
| `src/recorder.*` | The SigMF and raw recorder |
| `src/sdr_device.*` | The device interface, the driver list, sample-rate checks |
| `src/sdr_rtlsdr.cpp` | The RTL-SDR backend (librtlsdr) |
| `src/sdr_uhd.cpp` | The USRP backend (UHD C API) |
| `src/sdr_sim.cpp` | The simulated device |
| `src/sdr_siggen.cpp` | The loopback device that streams the generator into the live view |
| `src/sdr_tx.h` | The transmitter interface. `sdr_uhd.cpp` implements it for UHD |

## Signal generator

| File | Function |
|---|---|
| `src/siggen.*` | Generator DSP: CW, pulsed, playback and chain waveforms, waveform cache, resampler, checks, chain files, parameter bus |
| `src/siggen_out.*` | Generator outputs: file job, transmit engine, preview |
| `src/app_siggen.cpp` | The signal generator window |
| `src/app_chain.cpp` | The Chain section and its preview |

## Audio

| File | Function |
|---|---|
| `src/audio_dsp.*` | The streaming demodulator: channel filter, AM, FM, SSB, CW, resampler |
| `src/audio_engine.*` | Audio sessions (file or live), drift control, WAV recording and export |
| `src/audio_out.*` | Sound output (miniaudio) and the ring buffer to the audio callback |
| `src/wav_writer.*` | The WAV file writer |
| `src/app_audio.cpp` | The audio window |

## Tools

| File | Function |
|---|---|
| `tools/iqgen.cpp` | The synthetic test-signal generator |
| `tools/siggen_check.cpp` | The self-check for the signal generator |
| `tools/audio_check.cpp` | The self-check for the audio demodulator |
| `tools/log_check.cpp` | The self-check for the logger |
| `tools/segment_check.cpp` | The self-check for the time segment rules |
| `tools/extract_check.cpp` | The self-check for the region export |
| `tools/gpu_check.cpp` | The GPU self-check and benchmark |

## Other folders

| Folder | Contents |
|---|---|
| `third_party/` | Dear ImGui 1.91.9, ImPlot 0.16 and miniaudio 0.11.25 |
| `cmake/` | `embed.cmake`, which puts the UI font in the binary |
| `docs/` | These pages. `docs/design/` has the design records |
| `.github/workflows/` | The CI jobs |
