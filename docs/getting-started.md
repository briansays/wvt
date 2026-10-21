# Getting started

This page tells you how to install the necessary packages, build wvt, and open your first
recording. For all the build settings, refer to [Build reference](build.md).

## Requirements

- A C++20 compiler.
- CMake 3.16 or later. A CUDA build needs CMake 3.18 or later.
- The X11 and OpenGL headers and libraries.

The repository contains Dear ImGui 1.91.9, ImPlot 0.16, miniaudio 0.11.25 and stb_image_write 1.16
in `third_party/`.
Thus the build does not need network access. The UI font is in the binary.

## Install the packages

Install the compiler, CMake and the X11 and OpenGL packages for your system:

| System | Command |
|---|---|
| Debian / Ubuntu | `sudo apt install build-essential cmake libx11-dev libgl1-mesa-dev` |
| Fedora | `sudo dnf install gcc-c++ cmake libX11-devel mesa-libGL-devel` |
| Arch | `sudo pacman -S base-devel cmake libx11 mesa` |
| macOS | Install [XQuartz](https://www.xquartz.org) and the Xcode Command Line Tools |

On macOS, the build links `/opt/X11/lib/libX11` and `libGL` from XQuartz. Thus the binary is a
real X11 client.

These packages are optional:

- librtlsdr, for an RTL-SDR. Refer to [Live devices](live.md).
- UHD, for a USRP. Refer to [Live devices](live.md).
- The CUDA toolkit, for an NVIDIA GPU. Refer to [GPU processing](gpu.md).

## Build

Do these steps in the top folder of the repository:

1. Configure the build:

   ```sh
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   ```

2. Read the `wvt:` lines in the CMake output. Each line tells you which optional function the
   build has, and why a function is not available.
3. Compile:

   ```sh
   cmake --build build -j
   ```

The build puts the `wvt` program and the self-check programs in `build/`.

## Make a test recording

If you do not have a recording, make one with the `iqgen` tool:

```sh
./build/iqgen out.sigmf-data --fmt ci16
```

This command writes a 2 s recording at 2 MS/s. For the signals in it, refer to
[Self-checks and test signals](self-checks.md).

## Open a recording

```sh
./build/wvt out.sigmf-meta
```

wvt reads the sample format, the sample rate and the center frequency from the SigMF metadata. For
the other ways that wvt finds these values, refer to [The file viewer](viewer.md).

To start with the channel filter on and a short view, type:

```sh
./build/wvt out.sigmf-meta --chan 300k:150k --view 0.5:0.505
```

## Use the window

- To pan, drag a plot with the left mouse button.
- To zoom, turn the mouse wheel. To zoom one axis only, put the pointer on that axis.
- To zoom to a box, drag with the right mouse button.
- To see all controls, select **Help → Keyboard and mouse**.

For more information, refer to [Mouse and keyboard](controls.md).

## Remote and virtual displays

wvt uses GLX and OpenGL when the X server supports them. If not, wvt uses its built-in software
renderer. Thus wvt also works over `ssh -X`, in Xvfb, and in VNC sessions without GL. To use the
software renderer always, start wvt with `--software`.
