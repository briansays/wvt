# Source layout

## Folders

The source files are in folders in `src/`, one folder for each function:

| Folder | Contents |
|---|---|
| `src/app/` | `main.cpp`, the command line, the window and the renderer, and all the UI windows (`app_*.cpp`) |
| `src/common/` | Code that all the parts use: the logger, JSON, the sample ring, SI numbers and UI helpers, the build information |
| `src/dsp/` | Signal processing for the file viewer: FFT, filters, the waterfall and demodulator engine, region export |
| `src/io/` | File input and output: the IQ file reader, the recorder, the WAV writer, the PNG snapshots |
| `src/device/` | Live devices: the device interface, the backends and the live engine |
| `src/gpu/` | The GPU interface and the CUDA, Metal and stub backends |
| `src/siggen/` | The signal generator and its outputs |
| `src/audio/` | The audio demodulator, the audio sessions and the sound output |

An include names the folder, relative to `src/`, for example `#include "common/log.h"`. `src/` is
the only include folder for the project files. `src/common/` does not include files from other
folders.

## Application

| File | Function |
|---|---|
| `src/app/main.cpp` | The table of command-line options, ImGui and ImPlot setup, the event loop |
| `src/app/cli.*` | The table-driven command-line parser: it runs the option handlers and makes `--help` from the same table |
| `src/app/platform_x11.*` | The Xlib window, input, clipboard, and GLX or software presentation |
| `src/app/sw_renderer.*` | The software rasterizer for ImGui draw data |
| `src/app/app.*` | The UI: toolbar, linked plots, parameter panels |
| `src/app/app_menu.cpp` | The menu bar, the keyboard help and the About window |
| `src/common/ui_util.*` | SI-unit inputs and formatters, the scrollbar widget, forms, the gear button, the color bar, the style |
| `src/common/log.*` | The logger |
| `src/common/build_info.h` | The git commit and version tag. `cmake/build_info.cmake` writes the definitions at each build |
| `src/common/json.*` | The JSON module: a small reader (chain files, SigMF metadata) and the writer functions `jsonQuote` and `jsonNumber` |
| `src/io/snapshot.*`, `src/app/app_snapshot.cpp` | Plot snapshots: names, crop, PNG writer, the snapshot window and the capture |

## File viewer and signal processing

| File | Function |
|---|---|
| `src/dsp/engine.*` | Worker threads, and the waterfall, channel filter, demodulator and overview calculations |
| `src/dsp/dsp.*` | FFT (Accelerate vDSP on macOS), windows, FIR design, parallel-for, smoothing |
| `src/io/iq_file.*` | The memory-mapped reader for all sample formats, and SigMF and file name detection |
| `src/dsp/extract.*` | Region export: checks, filter design, the background job |
| `src/app/app_export.cpp` | The export window, the waterfall right-click menu, region selection |

## GPU

| File | Function |
|---|---|
| `src/gpu/gpu.h`, `src/gpu/gpu_common.cpp` | The GPU interface, the `--gpu` rules, the fallback to the CPU |
| `src/gpu/gpu_backend.h` | The backend interface |
| `src/gpu/gpu_cuda.cu` | The CUDA backend: device setup, waterfall and live spectrum kernels with cuFFT |
| `src/gpu/gpu_metal.mm` | The Metal backend: device setup, one-pass FFT kernels, MPSGraph FFT, channel filter |
| `src/gpu/gpu_stub.cpp` | The backend for builds without a GPU |

## Live devices

| File | Function |
|---|---|
| `src/app/app_live.cpp` | The live mode UI: device and recording toolbar, live spectrum, scrolling waterfall |
| `src/device/live.*` | The live engine: the device, the sample ring, the recorder and the spectrum thread |
| `src/common/sample_ring.h` | The ring buffer between the device thread, the spectrum thread and the recorder |
| `src/io/recorder.*` | The SigMF and raw recorder |
| `src/device/sdr_device.*` | The device interface, the driver list, sample-rate checks |
| `src/device/sdr_rtlsdr.cpp` | The RTL-SDR backend (librtlsdr) |
| `src/device/sdr_uhd.cpp` | The USRP backend (UHD C API): all USRP models, one channel at a time |
| `src/device/sdr_bb60.cpp` | The Signal Hound BB60 backend (`bb_api`) |
| `src/device/sdr_sim.cpp` | The simulated device |
| `src/device/sdr_siggen.cpp` | The loopback device that streams the generator into the live view |
| `src/device/sdr_tx.h` | The transmitter interface. `sdr_uhd.cpp` implements it for UHD |

## Signal generator

| File | Function |
|---|---|
| `src/siggen/siggen.*` | Generator DSP: CW, pulsed, playback and chain waveforms, waveform cache, resampler, checks, chain files, parameter bus |
| `src/siggen/siggen_out.*` | Generator outputs: file job, transmit engine, preview |
| `src/app/app_siggen.cpp` | The signal generator window |
| `src/app/app_chain.cpp` | The Chain section and its preview |

## Audio

| File | Function |
|---|---|
| `src/audio/audio_dsp.*` | The streaming demodulator: channel filter, AM, FM, SSB, CW, resampler |
| `src/audio/audio_engine.*` | Audio sessions (file or live), drift control, WAV recording and export |
| `src/audio/audio_out.*` | Sound output (miniaudio) and the ring buffer to the audio callback |
| `src/io/wav_writer.*` | The WAV file writer |
| `src/app/app_audio.cpp` | The audio window |

## Tools

| File | Function |
|---|---|
| `tools/iqgen.cpp` | The synthetic test-signal generator |
| `tools/siggen_check.cpp` | The self-check for the signal generator |
| `tools/audio_check.cpp` | The self-check for the audio demodulator |
| `tools/log_check.cpp` | The self-check for the logger |
| `tools/segment_check.cpp` | The self-check for the time segment rules |
| `tools/extract_check.cpp` | The self-check for the region export |
| `tools/snapshot_check.cpp` | The self-check for the plot snapshots |
| `tools/device_check.cpp` | The self-check for the device list and the sample rate rules |
| `tools/bb_check.cpp` | The self-check for the BB60 backend |
| `tools/bb_sim/` | A simulated BB60D behind the `bb_api` calls that wvt uses |
| `tools/gpu_check.cpp` | The GPU self-check and benchmark |
| `tools/run-clang-tidy.sh` | Runs clang-tidy on the wvt sources, as CI does (`.clang-tidy`, `tools/.clang-tidy`) |
| `tools/fuzz/` | The libFuzzer targets for the parsers, and their seed inputs in `corpus/` |

## Other folders

| Folder | Contents |
|---|---|
| `third_party/` | Dear ImGui 1.91.9, ImPlot 0.16, miniaudio 0.11.25 and stb_image_write 1.16 |
| `cmake/` | `embed.cmake`, which puts the UI font in the binary |
| `docs/` | These pages. `docs/design/` has the design records |
| `.github/workflows/` | The CI jobs |
