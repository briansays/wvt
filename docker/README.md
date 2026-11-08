# Run wvt in a container

This folder has the files to build wvt into a Docker image and to run it with Docker Compose. Use it
on a computer that does not have the build tools or the SDR libraries. The window shows on the X
server of the computer.

| File | Function |
|---|---|
| `compose.yaml` | The services: `wvt`, `wvt-sdr`, `wvt-cuda` and `test` |
| `compose.vendor.yaml` | An override file that adds the BB60 and Sidekiq SDKs to the build. Refer to [Vendor SDKs](#vendor-sdks) |
| `Dockerfile` | The image. The build context is the repository root |
| `Dockerfile.dockerignore` | The files that the build does not get (BuildKit reads it next to the Dockerfile) |
| `.env.example` | The settings for your computer. Copy it to `.env` |
| `wvt-data/` | The default data folder, mounted at `/data` |
| `no-sdk/` | An empty SDK folder, for a build with only one vendor SDK |

The image has:

- wvt and `iqgen`, built on Ubuntu 24.04.
- The RTL-SDR and USRP backends, with the FPGA images of the USB USRPs (B200, B205mini, B210).
- The network stream (ZMQ), the audio playback and the simulator.
- Mesa, for OpenGL without a GPU. If OpenGL is not available, wvt uses its software renderer.
- `zenity`, for the **Browse** buttons. The dialogs show the folders of the container.

The image does not have the BB60 or Sidekiq backends, because their SDKs are not open source. To
add them, use `compose.vendor.yaml`. Refer to [Vendor SDKs](#vendor-sdks).

## What you need

- Docker with the Compose plugin (`docker compose`).
- An X server: the desktop session on Linux (X11 or XWayland), or [XQuartz](https://www.xquartz.org)
  on macOS.
- For SDRs, sound and the host network: Linux. Docker Desktop on macOS and Windows cannot give USB
  devices to a container.
- For CUDA: an NVIDIA driver and the
  [NVIDIA Container Toolkit](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/).

## Start wvt

Run all the commands in this folder:

```sh
cd docker
cp .env.example .env               # one time: then change the values for your computer
docker compose build               # build the image wvt:latest (approximately 10 minutes)
```

Let the container use your X server, one time for each session:

```sh
xhost +SI:localuser:$(id -un)      # Linux: only your user can connect
/opt/X11/bin/xhost +localhost      # macOS (XQuartz)
```

Then start a service:

```sh
docker compose run --rm wvt                          # the window, the files and the simulator
docker compose run --rm wvt-sdr                      # Linux: also the SDRs, the host network and the sound
docker compose run --rm wvt-cuda                     # Linux: wvt-sdr and an NVIDIA GPU
docker compose run --rm wvt --device sim -r 1M       # wvt options go after the service name
```

`docker compose run` builds the image if it does not exist. To build it again after a change to
the source, use `docker compose build` or `docker compose run --build --rm wvt`.

### Settings

`.env` sets the variables of `compose.yaml`. Each variable has a default, so `.env` can be empty.

| Variable | Default | Function |
|---|---|---|
| `WVT_UID`, `WVT_GID` | `1000` | The user and group in the container. Set them to `id -u` and `id -g`, so that the files in the data folder are yours |
| `WVT_DATA` | `./wvt-data` | The data folder, mounted at `/data` |
| `WVT_CONFIG_DIR` | `../config` | The config folder, mounted at `/opt/wvt/config` |
| `WVT_DISPLAY` | `$DISPLAY`, else `:0` | The X display. On macOS, `host.docker.internal:0` |
| `WVT_USB_GID` | `46` | `wvt-sdr`: the group of the USB devices (`stat -c %g /dev/bus/usb/001/001`) |
| `WVT_RENDER_GID` | `44` | `wvt-sdr`: the group of the GPU (`stat -c %g /dev/dri/renderD128`) |
| `WVT_IMAGE` | `wvt:latest`, `wvt:cuda` | The image |
| `WVT_BUILD_HASH` | `docker` | The commit in **Help > About** and in the log (`git rev-parse --short HEAD`) |
| `COMPOSE_FILE` | `compose.yaml` | `compose.yaml:compose.vendor.yaml`: build and run with the vendor SDKs |
| `WVT_SIDEKIQ_SDK` | `../third_party/sidekiq_sdk/sidekiq_sdk_v4.26.0` | `compose.vendor.yaml`: the Sidekiq SDK folder |
| `WVT_SIGNALHOUND_SDK` | `../third_party/signal_hound_sdk` | `compose.vendor.yaml`: the Signal Hound SDK folder |

You can also set a variable for one command: `WVT_DATA=~/captures docker compose run --rm wvt`.

### The data folder and the config folder

The container sees the data folder as `/data`. wvt uses it as its home folder:

| In the container | Contents |
|---|---|
| `/data/` (`~`) | The recordings, the snapshots and the signal generator files, in the folders that `wvt.conf` sets. `~/wvt/recordings/` in `wvt.conf` is `/data/wvt/recordings/` |
| `/data/logs/` | The log files |

To open a recording, put it in the data folder, then give its path in the container:

```sh
docker compose run --rm wvt /data/capture.sigmf-meta
```

The config folder of the repository (`config/`) is mounted at `/opt/wvt/config`. This is `config/`
in the folder above the program, so the container uses the same `wvt.conf` and `zmq_*.ini` files as
`build/wvt`. Refer to
[The config folder](../docs/MANUAL.md#the-config-folder). Paths in `wvt.conf` are paths in the
container, for example `/data/...`.

### macOS

1. Install XQuartz.
2. In **XQuartz > Settings > Security**, select **Allow connections from network clients**.
3. Restart XQuartz, then run `/opt/X11/bin/xhost +localhost`.
4. In `.env`, set `WVT_DISPLAY=host.docker.internal:0`.
5. Run `docker compose run --rm wvt`.

On macOS, only the `wvt` service is available: the window, the files and the simulator.

## Services

| Service | Function |
|---|---|
| `wvt` | The window and the files. Runs on all hosts with an X server |
| `wvt-sdr` | Linux. Adds the USB SDRs, the host network, `/dev/dri` and the sound |
| `wvt-cuda` | Linux. `wvt-sdr` with an NVIDIA GPU. Uses the image `wvt:cuda` |
| `test` | The build stage. Runs the self-checks: `docker compose run --rm test` |

`wvt-sdr`, `wvt-cuda` and `test` have Compose profiles. `docker compose up` and
`docker compose build` use only `wvt`. To build a different one, name it:
`docker compose build wvt-cuda`.

`wvt-sdr` and `wvt-cuda` give the container these items of the host:

- **USB:** `/dev/bus/usb`, with access to all USB devices. An SDR that you connect after the start
  also works.
- **The network of the host:** for network USRPs and for the network stream. The default stream
  address is then the address of the host.
- **`/dev/dri`:** OpenGL on the GPU of the X server (Intel or AMD).
- **The sound:** the PulseAudio or PipeWire socket of your session.

## SDRs on the host

The container does not load drivers, and it does not set the access to the devices. Do the steps
for Linux in [Prepare the device](../docs/MANUAL.md#prepare-the-device) one time, on the host:

- RTL-SDR: stop the DVB-T driver, and install the udev rule (the `rtl-sdr` package).
- USB USRP: install the udev rule (the `uhd-host` package). The image already has the FPGA images.
- Network USRP: make the network buffers of the kernel larger.

Set `WVT_USB_GID` to the group of the USB devices (usually `plugdev`). Then start `wvt-sdr`, and
select the device in the window. Or record without the window:

```sh
docker compose run --rm wvt-sdr --device rtlsdr -c 100M -r 2.4M --no-gui \
    --record /data/capture --record-duration 10
```

## Build the image

`docker compose build` builds `wvt:latest`. With `docker build`, from the repository root:

```sh
docker build -f docker/Dockerfile -t wvt .           # the image
docker build -f docker/Dockerfile --target test .    # build, then run the self-checks
```

The build has these arguments (`--build-arg NAME=VALUE`, or `args:` in `compose.yaml`):

| Argument | Default | Function |
|---|---|---|
| `BUILD_IMAGE`, `RUN_IMAGE` | `ubuntu:24.04` | The images of the build and of the result. For CUDA, the `nvidia/cuda` images |
| `WVT_CUDA` | `AUTO` | `WVT_WITH_CUDA`. `AUTO` builds CUDA if the build image has the CUDA toolkit |
| `UHD_IMAGES` | `b2xx` | The FPGA images for USB USRPs: a list for `uhd_images_downloader -t`, `all` or `none` |
| `ZENITY` | `1` | `0`: no `zenity` (approximately 150 MB less). You then type the paths |
| `WVT_CMAKE_ARGS` | Empty | More CMake options, for example `-DCMAKE_CUDA_ARCHITECTURES=89` |
| `WVT_BUILD_HASH`, `WVT_BUILD_TAG` | `docker`, empty | The build and the version in **Help > About** and in the log |

The build context does not have the `.git` folder. To show the commit, set `WVT_BUILD_HASH` in
`.env`, or:

```sh
WVT_BUILD_HASH=$(git rev-parse --short HEAD) docker compose build
```

### Vendor SDKs

The BB60 and Sidekiq backends need the SDKs from Signal Hound and Epiq Solutions. The SDKs are not
open source, so they are not in the repository and not in the default image. `compose.vendor.yaml`
gives the SDK folders to the build as build contexts.

1. Unpack the SDKs on the computer that builds the image. The defaults are the folders that
   `build/wvt` also uses:

   | SDK | Default folder | The folder must have |
   |---|---|---|
   | Sidekiq | `third_party/sidekiq_sdk/sidekiq_sdk_v4.26.0` | `sidekiq_core/inc/sidekiq_api.h` |
   | Signal Hound | `third_party/signal_hound_sdk` | `bb_api.h` and `libbb_api.so.*` (in a subfolder) |

   For different folders, set `WVT_SIDEKIQ_SDK` and `WVT_SIGNALHOUND_SDK` in `.env`. A relative
   path is relative to this folder.

2. In `.env`, select the override file:

   ```sh
   COMPOSE_FILE=compose.yaml:compose.vendor.yaml
   ```

3. Build and run as usual:

   ```sh
   docker compose build                     # the image wvt:vendor
   docker compose build wvt-cuda            # the image wvt:cuda-vendor
   docker compose run --rm wvt-sdr          # the BB60 and the Sidekiq need wvt-sdr or wvt-cuda
   ```

   Or, without `COMPOSE_FILE`:
   `docker compose -f compose.yaml -f compose.vendor.yaml build`.

The build log shows `Signal Hound BB60 support enabled` and `Sidekiq support enabled` when it
finds the SDKs. If it does not find an SDK, the build continues without that backend. To stop the
build instead, add the build argument
`WVT_CMAKE_ARGS: "-DWVT_WITH_BB=ON -DWVT_WITH_SKIQ=ON"` to `x-vendor-build` in `compose.vendor.yaml`
(under `args:`).

To build only one of the two backends, set the variable of the other SDK to `./no-sdk` (an empty
folder). For example, only the BB60: `WVT_SIDEKIQ_SDK=./no-sdk`.

With `docker build`, from the repository root:

```sh
docker build -f docker/Dockerfile -t wvt:vendor \
    --build-context sidekiq_sdk=third_party/sidekiq_sdk/sidekiq_sdk_v4.26.0 \
    --build-context signalhound_sdk=third_party/signal_hound_sdk .
```

> **Warning:** An image with a vendor SDK has the SDK in it. Do not push this image to a public
> registry. To copy it to a different computer, use `docker save` (refer to
> [Copy the image to a different computer](#copy-the-image-to-a-different-computer)) or a private
> registry.

On the computer that runs the image:

- **BB60:** a USB device. Use `wvt-sdr` or `wvt-cuda`, and install the udev rule of the Signal Hound
  SDK on the host.
- **Sidekiq on USB:** use `wvt-sdr` or `wvt-cuda`.
- **Sidekiq on PCIe** (M.2, mPCIe, X2, X4): load the kernel drivers of the Sidekiq SDK on the host.
  `compose.yaml` does not give the PCIe device files to the container. Find them on the host after
  the drivers load, then add them to `devices:` of the service in `compose.vendor.yaml`.

### Copy the image to a different computer

```sh
docker save wvt:latest | gzip > wvt-image.tar.gz
# on the other computer:
docker load < wvt-image.tar.gz
```

Use the image name that you built, for example `wvt:vendor`. Then copy this folder to the other
computer. Set `WVT_CONFIG_DIR` in `.env` to a config folder there. The image is for the CPU type of the computer that built it (`amd64` or `arm64`).

## Problems

| Problem | Cause and remedy |
|---|---|
| `cannot open display` | The X server did not accept the connection. On Linux, run `xhost +SI:localuser:$(id -un)`. On macOS, allow network clients in XQuartz and set `WVT_DISPLAY` |
| The status bar shows `software` | OpenGL is not available. wvt is slower, but it works. On Linux, use `wvt-sdr` for `/dev/dri` |
| The device list does not show the SDR | Use `wvt-sdr`. Make sure that the udev rules are on the host, that the SDR shows in `lsusb`, and that `WVT_USB_GID` is the group of the device |
| No sound | Use `wvt-sdr`. The session must have PulseAudio or PipeWire |
| Files in the data folder belong to root | Docker made the folder because it did not exist. Make the folder yourself before the start, and set `WVT_UID` and `WVT_GID` |
| `permission denied` in `/data` | `WVT_UID` is not the owner of the data folder. Set it to `id -u` |
