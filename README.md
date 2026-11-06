# wvt — IQ waveform visualization tool

An X11 desktop tool to examine raw IQ recordings from software-defined radios.
The left side shows a spectrum and a waterfall. The right side shows I/Q, AM, FM and PM plots
that share one time axis. When you pan or zoom one plot, all the other plots follow.

*File View:*
<img width="1628" height="1000" alt="CleanShot 2026-10-25 at 12 33 26 PM" src="https://github.com/user-attachments/assets/516071cb-e20a-4f28-9f93-a0f25db03535" />

*Live View:* 
<img width="1628" height="1000" alt="CleanShot 2026-10-25 at 12 32 03 PM" src="https://github.com/user-attachments/assets/1b6723c9-adea-4075-b649-0d37eaf3a7da" />

- **Live devices:** wvt streams from an RTL-SDR v3, a USRP (B200 family, N3xx, X3xx, X4xx), a Signal Hound BB60 or an Epiq
  Sidekiq (M.2, mPCIe, X2, X4, NV100), and records the
  stream to SigMF or raw IQ files. It can send the stream over ZMQ to another wvt or to other
  programs such as GNU Radio, and receive samples over ZMQ from them.
- **Signal generator:** CW, pulsed, swept, playback and chain waveforms, to a file, to the live
  view, or transmitted with a USRP.
- **Audio:** AM, NFM, WFM, USB, LSB and CW demodulation to the speakers or to a WAV file.
- **Region export:** a time range, and as an option a frequency band, to a new file.
- **Plot snapshots:** PNG files of the plots, as they show on the screen.
- **Point measurements:** two markers on the plots, and the time, frequency and value between them.
- **File sessions:** save the recording, the view, the plot settings and the channel, and open them again later.
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

The documentation is the [wvt User Manual](docs/MANUAL.md). To make the HTML pages with Sphinx:

```sh
pip install -r docs/requirements.txt
sphinx-build -W -b html docs build/docs      # then open build/docs/index.html
```

## License

wvt is released under the [MIT License](LICENSE).

The bundled third-party code in [`third_party/`](third_party) keeps its own licenses:
[Dear ImGui](third_party/imgui/LICENSE.txt) (MIT), [ImPlot](third_party/implot/LICENSE) (MIT),
[miniaudio](third_party/miniaudio/LICENSE) (public domain or MIT-0),
[stb_image_write](third_party/stb/stb_image_write.h) (public domain or MIT) and
[Native File Dialog](third_party/nativefiledialog/LICENSE) (zlib).
