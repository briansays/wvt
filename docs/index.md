# wvt: IQ waveform visualization tool

wvt is an X11 desktop tool for raw IQ recordings from software-defined radios (SDRs).

- The left side of the window shows a spectrum and a waterfall.
- The right side shows I/Q, AM, FM and PM plots. These plots share one time axis.
- When you pan or zoom one plot, all the other plots follow.

wvt has these other functions:

- **Live devices:** wvt streams from an RTL-SDR v3, a USRP (B200 family, N3xx, X3xx, X4xx) or a Signal Hound BB60. It shows the
  spectrum and the waterfall in real time, and it records the stream to a file.
- **Signal generator:** wvt makes CW, pulsed, swept and recorded waveforms. It writes them to a
  file, shows them in the live view, or transmits them with a USRP.
- **Audio:** wvt demodulates one channel and plays it through the speakers or writes it to a WAV
  file.
- **Region export:** wvt writes a time range, and as an option a frequency band, to a new file.
- **GPU processing:** wvt can do the signal processing on an NVIDIA GPU (CUDA) or an Apple GPU
  (Metal).

Start with [Getting started](getting-started.md). For all the build settings, refer to
[Build reference](build.md).

```{toctree}
:maxdepth: 2
:caption: User guide

getting-started
viewer
segments
export
snapshots
live
siggen
audio
gpu
logging
controls
cli
```

```{toctree}
:maxdepth: 2
:caption: Reference

build
signal-processing
self-checks
source-layout
writing-docs
```

```{toctree}
:maxdepth: 1
:caption: Design records

design/index
```
