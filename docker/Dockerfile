# syntax=docker/dockerfile:1
#
# wvt in a container. A multi-stage build: the "build" stage compiles wvt with
# all the open-source backends, "test" runs the self-checks, and the final
# stage has wvt and only its run-time libraries.
#
# The build context is the repository root, not this folder. Use compose.yaml
# in this folder (docker compose build), or from the repository root:
#
#   docker build -f docker/Dockerfile -t wvt .                  # CPU image (Ubuntu 24.04)
#   docker build -f docker/Dockerfile --target test .           # build and run the self-checks
#
# The vendor SDKs are not open source and are not in the image by default. To
# build the BB60 or Sidekiq backend, give the SDK folder as a build context
# (the image then holds the SDK: do not publish it):
#
#   docker build -f docker/Dockerfile -t wvt:vendor \
#       --build-context sidekiq_sdk=third_party/sidekiq_sdk/sidekiq_sdk_v4.26.0 \
#       --build-context signalhound_sdk=third_party/signal_hound_sdk .
#
# Refer to docker/README.md.

ARG BUILD_IMAGE=ubuntu:24.04
ARG RUN_IMAGE=ubuntu:24.04

# Empty by default; --build-context sidekiq_sdk=DIR or signalhound_sdk=DIR replaces them.
FROM scratch AS sidekiq_sdk
FROM scratch AS signalhound_sdk

# ---------------------------------------------------------------------------
FROM ${BUILD_IMAGE} AS build
ARG DEBIAN_FRONTEND=noninteractive
# libboost-*: uhd.h includes boost/config.hpp and the uhd pkg-config module
# links boost_system; libuhd-dev gets them only through a recommended package.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake pkg-config ca-certificates \
      libx11-dev libgl1-mesa-dev librtlsdr-dev libuhd-dev libboost-dev libboost-system-dev \
      libusb-1.0-0-dev libglib2.0-dev libtirpc-dev \
 && rm -rf /var/lib/apt/lists/*

COPY --from=sidekiq_sdk / /opt/sidekiq_sdk/
COPY --from=signalhound_sdk / /opt/signalhound_sdk/
COPY . /opt/wvt/src

# AUTO for CUDA: a CUDA base image builds the GPU backend, Ubuntu does not.
ARG WVT_CUDA=AUTO
# The values that "Help > About" and the log show (the image has no .git folder).
ARG WVT_BUILD_HASH=docker
ARG WVT_BUILD_TAG=
# More CMake options, for example "-DCMAKE_CUDA_ARCHITECTURES=89".
ARG WVT_CMAKE_ARGS=
RUN WVT_BUILD_HASH="${WVT_BUILD_HASH}" WVT_BUILD_TAG="${WVT_BUILD_TAG}" sh -ec '\
      cmake -S /opt/wvt/src -B /opt/wvt/build -DCMAKE_BUILD_TYPE=Release \
        -DWVT_WITH_RTLSDR=ON -DWVT_WITH_UHD=ON -DWVT_WITH_ZMQ=ON -DWVT_WITH_AUDIO=ON \
        -DWVT_WITH_CUDA=${WVT_CUDA} \
        -DSIDEKIQ_SDK_DIR=/opt/sidekiq_sdk -DSIGNALHOUND_SDK_DIR=/opt/signalhound_sdk \
        ${WVT_CMAKE_ARGS}; \
      cmake --build /opt/wvt/build -j "$(nproc)"; \
      mkdir -p /opt/wvt/build/bb_api'

# ---------------------------------------------------------------------------
# docker build --target test : the self-checks, as in CI.
FROM build AS test
RUN ctest --test-dir /opt/wvt/build --output-on-failure -j "$(nproc)"

# ---------------------------------------------------------------------------
FROM ${RUN_IMAGE} AS runtime
ARG DEBIAN_FRONTEND=noninteractive
# The FPGA images that UHD loads into a USB USRP: "b2xx" (B200, B205mini, B210),
# a list for uhd_images_downloader -t (for example "b2xx|e3xx"), "all" or "none".
# The network USRPs (N3xx, X3xx, X4xx) load their FPGA from their own flash.
ARG UHD_IMAGES=b2xx
# 1: zenity, for the Browse buttons (the native file dialogs). It adds GTK
# (approximately 150 MB). 0: no zenity; you type the paths.
ARG ZENITY=1
# libgl1-mesa-dri and libglx-mesa0: GLX on the X server of the host (Mesa). With
# no GLX, wvt uses its software renderer. libasound2t64 and libpulse0: miniaudio
# loads one of them for the audio window.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates libx11-6 libgl1 libglx-mesa0 libgl1-mesa-dri \
      librtlsdr2 libuhd4.6.0t64 uhd-host \
      libusb-1.0-0 libglib2.0-0t64 libtirpc3t64 \
      libasound2t64 libpulse0 \
      $( [ "${ZENITY}" = 1 ] && echo zenity ) \
 && if [ "${UHD_IMAGES}" = all ]; then uhd_images_downloader; \
    elif [ "${UHD_IMAGES}" != none ]; then uhd_images_downloader -t "${UHD_IMAGES}"; fi \
 && rm -rf /var/lib/apt/lists/*

# The same paths as in the build stage: the BB60 and Sidekiq libraries are found
# through rpaths into these folders (both are empty without a vendor SDK).
COPY --from=sidekiq_sdk / /opt/sidekiq_sdk/
COPY --from=build /opt/wvt/build/wvt /opt/wvt/build/iqgen /opt/wvt/build/
COPY --from=build /opt/wvt/build/bb_api/ /opt/wvt/build/bb_api/
COPY --from=build /opt/wvt/src/LICENSE /opt/wvt/src/THIRD_PARTY_NOTICES.md /opt/wvt/
# The example ZMQ formats. compose.yaml mounts the config folder of the
# repository over /opt/wvt/config ("config/" above the program, as for build/wvt).
COPY --from=build /opt/wvt/src/config/ /opt/wvt/config/
RUN ln -s /opt/wvt/build/wvt /usr/local/bin/wvt && ln -s /opt/wvt/build/iqgen /usr/local/bin/iqgen \
 && mkdir -p /data && chmod 1777 /data

# /data holds the recordings and the logs: mount a folder of the host there.
# Any user ID can run the image (docker run --user).
ENV HOME=/data \
    WVT_LOG_DIR=/data/logs
WORKDIR /data
VOLUME /data
ENTRYPOINT ["wvt"]
