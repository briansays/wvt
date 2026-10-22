# wvt — IQ waveform visualization tool

An X11 desktop tool to examine raw IQ recordings from software-defined radios.
The left side shows a spectrum and a waterfall. The right side shows I/Q, AM, FM and PM plots
that share one time axis. When you pan or zoom one plot, all the other plots follow.

<img width="2125" height="1407" alt="image" src="https://github.com/user-attachments/assets/71d188af-cc07-4a0c-93ef-36d082b5829b" />

- **Live devices:** wvt streams from an RTL-SDR v3, a USRP (B200 family, N3xx, X3xx, X4xx) or a Signal Hound BB60, and records the
  stream to SigMF or raw IQ files.
- **Signal generator:** CW, pulsed, swept, playback and chain waveforms, to a file, to the live
  view, or transmitted with a USRP.
- **Audio:** AM, NFM, WFM, USB, LSB and CW demodulation to the speakers or to a WAV file.
- **Region export:** a time range, and as an option a frequency band, to a new file.
- **Plot snapshots:** PNG files of the plots, as they show on the screen.
- **GPU processing:** CUDA on NVIDIA GPUs (Linux) and Metal on Apple GPUs (macOS).

## Quick start

Install a C++20 compiler, CMake 3.16 or later, and the X11 and OpenGL development packages
(on macOS, [XQuartz](https://www.xquartz.org)). Then type:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/iqgen out.sigmf-data --fmt ci16      # make a test recording
./build/wvt out.sigmf-meta --chan 300k:150k
```

Or use the build script. It builds in `build.<arch>` (for example `build.arm64`), and its options
select the architecture and turn features off:

```sh
scripts/build.sh --help
scripts/build.sh --no-cuda --test            # build, then run the self-checks
```

## Documentation

The documentation is in [`docs/`](docs/index.md). To make the HTML pages with Sphinx:

```sh
pip install -r docs/requirements.txt
sphinx-build -W -b html docs build/docs      # then open build/docs/index.html
```

| Page | Contents |
|---|---|
| [Getting started](docs/getting-started.md) | Packages, build, first run |
| [Build reference](docs/build.md) | All CMake options and what each option enables or disables, build targets |
| [The file viewer](docs/viewer.md) | File formats, metadata, screen layout, linked views |
| [Load a segment](docs/segments.md) | Work on a part of a large file |
| [Export a region](docs/export.md) | Write a time range or a band to a new file |
| [Plot snapshots](docs/snapshots.md) | Save the plots as PNG files |
| [Live devices](docs/live.md) | RTL-SDR and USRP setup, the live view, recording |
| [Signal generator](docs/siggen.md) | Waveforms, sweeps, playback, chains, outputs, transmit safety |
| [Audio](docs/audio.md) | Demodulation, playback and WAV files |
| [GPU processing](docs/gpu.md) | CUDA and Metal, run-time selection, fallback |
| [Logging](docs/logging.md) | The log file and the console output |
| [Mouse and keyboard](docs/controls.md) | All controls |
| [Command-line reference](docs/cli.md) | All options and environment variables |
| [Signal processing](docs/signal-processing.md) | How wvt calculates each plot |
| [Self-checks](docs/self-checks.md) | Test signals, self-check programs, CI |
| [Source layout](docs/source-layout.md) | The source files and their functions |
| [Design records](docs/design/index.md) | The plan and decisions for each feature |

## License

wvt is released under the [MIT License](LICENSE).

The bundled third-party code in [`third_party/`](third_party) keeps its own licenses:
[Dear ImGui](third_party/imgui/LICENSE.txt) (MIT), [ImPlot](third_party/implot/LICENSE) (MIT),
[miniaudio](third_party/miniaudio/LICENSE) (public domain or MIT-0) and
[stb_image_write](third_party/stb/stb_image_write.h) (public domain or MIT).
