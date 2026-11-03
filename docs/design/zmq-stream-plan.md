# Plan: publish a live stream over ZMQ and UDP (issue #96)

Status: phases 1 to 4 (build, wire format, publisher, subscriber device) implemented (2026-11-03)
on `zmq-udp-stream`. Phases 5 to 8 are planned.

## 1. Goal

From the issue:

- Run wvt on a remote computer, and publish its live samples as a ZMQ stream over UDP. The stream
  goes in one direction only.
- Another computer subscribes to the stream by its address.
- The header of each message has all the data that the subscriber needs to set its stream to the
  same parameters as the source (frequency, rate, sample format, gain).
- On the subscriber, the stream is an entry in the **Device** drop-down of the **Live** source,
  with an address field like the USRP **Args** field.
- **Record** on the Live page works as it does now, on both computers.

## 2. What wvt does now

- A device backend (`src/device/sdr_*.cpp`) implements `SdrDevice`. It pushes native samples
  (`cu8` or `ci16`) into a `SampleRing` (`src/common/sample_ring.h`) from its own thread.
- The ring has lossy readers (the spectrum, the audio) and **one** lossless gate reader (the
  recorder). A publisher cannot use the gate without taking it from the recorder.
- `LiveEngine::start` (`src/device/live.cpp`) opens the device and sizes the ring from
  `dev->sampleRate()` and `dev->format()` **before** `start()`. Thus a backend must know its rate
  and format when it opens.
- A retune is marked at its sample by `setRetuneMark` (devices that restart to retune) or after
  `setCenterFreq` returns. The recorder starts a new SigMF capture there.
- `drawDeviceToolbar` (`src/app/app_live.cpp:213`) shows the **Args** field for `uhd` only.
  `drawRecordBar` (`src/app/app_live.cpp:521`) has **Record** and its ⚙ popup.
- libzmq is not a dependency. It is not installed on the macOS build machine.

## 3. Decisions

The owner agreed to the transport and the addressing on 2026-11-03. The owner changed the default
address the same day: it is the address of the local computer, or `127.0.0.1:5600`.

| Question | Decision | Why |
|---|---|---|
| ZMQ transport | **RADIO/DISH sockets on `udp://`** (agreed) | The only ZMQ sockets that use UDP without OpenPGM. PUB/SUB does not run on UDP. |
| Draft API | wvt builds libzmq 4.3.5 itself with `ENABLE_DRAFTS=ON` (CMake `FetchContent`, static). A system libzmq is used only if it has the draft API (`zmq_join`). | RADIO/DISH are draft sockets. Homebrew and most distribution packages are built without drafts. |
| Addressing | `ip:port`, unicast or a multicast group (224.0.0.0/4). wvt finds the type from the address (agreed, changed) | UDP cannot "connect" to a sender. The address is where the subscriber receives. **Unicast:** the subscriber listens on an address of its own computer, and the publisher sends to that address. On one computer, both use the same address (for example the loopback). **Multicast:** the publisher and all subscribers type the same group. |
| Default address | The first IPv4 address of an interface that is up and is not the loopback (`getifaddrs`), port 5600. If there is none: `127.0.0.1:5600`. `zmq.address` in the configuration file replaces it (agreed) | The owner asked for it. It works without a change for two wvt on one computer. |
| ZMQ group name | `wvt` (fixed) | DISH must join a group. One stream per multicast address is sufficient. |
| Sample format on the wire | The native format of the source (`cu8` or `ci16`), no conversion | Smallest and without loss. Same as a *native* recording. |
| Message size | At most 8000 bytes of samples per message (2000 `ci16` or 4000 `cu8` samples) | The libzmq UDP engine sends one datagram per message and limits it to 8192 bytes, group name included. |
| Parameters in each message | Yes: frequency, rate, format, gain, AGC, sample index, time | The issue asks for it. A subscriber that joins late starts at the next message, and a lost message loses no state. |
| Device name and other text | A small *info* message once per second | Too large for each data message. Only the display uses it. |
| Loss detection | From the sample index in the header | The subscriber reports a jump as a gap with an exact count. SigMF recordings on the subscriber get the same gap annotations as a local device. |
| How the publisher reads | Lossy reader of the ring, like the audio | Leaves the gate to the recorder, so **Record** does not change. If the publisher falls behind, it skips to new samples, and the sample index tells the subscriber how many it lost. |
| Control from the subscriber | None. Frequency, rate and gain are read only on the subscriber | The stream is in one direction. |
| Source changes rate or format | The subscriber stops with *the source changed its rate (or format): click Start again* | `LiveEngine` sizes the ring for one rate. A rate change on the source restarts its stream anyway. |
| Security | No authentication. Multicast TTL 1 by default, so the stream stays on the local network. The docs say so. | Small scope. The header parser checks all fields, and a fuzz target tests it. |

## 4. Wire format

All values little-endian. One ZMQ message is one datagram.

Data message (header 64 bytes, then the samples):

| Offset | Type | Field |
|---|---|---|
| 0 | `char[4]` | magic `WVTZ` |
| 4 | `u8` | version (1) |
| 5 | `u8` | kind (1 = data, 2 = info) |
| 6 | `u8` | sample format: 1 cu8, 2 ci16, 3 cf32 (wire codes, not the `SampleFormat` values) |
| 7 | `u8` | flags: bit 0 AGC, bit 1 retune at the first sample, bit 2 end of stream |
| 8 | `u32` | stream ID: random, new for each start of the source |
| 12 | `u32` | sample count in this message |
| 16 | `u64` | sample index of the first sample, from the start of the stream |
| 24 | `f64` | center frequency, Hz |
| 32 | `f64` | sample rate, S/s |
| 40 | `f64` | gain (dB; BB60: reference level, dBm) |
| 48 | `u64` | time of the first sample, ns since 1970 (UTC) |
| 56 | `u8[8]` | reserved: 0 when sent, ignored when read |

Info message (one each second): the same 64-byte header with kind 2 and 0 samples, so it also has
the stream parameters. Then the driver (`rtlsdr`, `uhd`...) and the description (`"USRP B205mini
serial 31A2B3C"`), each a `u16` length and the bytes, at most 1000 bytes each. The sender cuts a
longer string before a whole UTF-8 character. The receiver changes control characters to `?`.

The parser rejects a message if:

- it is shorter than 64 bytes, or the magic is not `WVTZ`;
- the version is not 1 (a later version must change the version number to change the layout);
- the kind or the sample format is unknown;
- the rate is not finite or not in (0, 10 GS/s], the frequency is not finite or not in
  [0, 1 THz], or the gain is not finite or more than 1000 in size;
- a data message does not have exactly `count` samples after the header (a message without
  samples is allowed: the end-of-stream message);
- an info message has samples, a string longer than 1000 bytes, or bytes after its strings.

It ignores the reserved bytes and the unknown flags, so that a later version can add fields.
`fuzz_zmq_wire` fuzzes the parser.

## 5. Implementation

### Phase 1 — Build (`CMakeLists.txt`, `.github/workflows/build.yml`)

- `WVT_WITH_ZMQ` AUTO / ON / OFF, as `WVT_WITH_RTLSDR`.
- Try `pkg_check_modules(libzmq)` and a test program with `zmq_join` and
  `ZMQ_BUILD_DRAFT_API`. If that fails, build libzmq from `WVT_ZMQ_SOURCE_DIR`, or download the
  v4.3.5 release with `file(DOWNLOAD)` and check its SHA-256. Then `add_subdirectory` with
  `ENABLE_DRAFTS=ON`, `BUILD_SHARED=OFF`, `BUILD_TESTS=OFF`, no CURVE, WebSocket or TLS.
  `file(DOWNLOAD)` and not `FetchContent`, so that with AUTO a failed download disables ZMQ
  instead of stopping the configure step.
- Define `WVT_HAVE_ZMQ`; `libzmq-static` exports `ZMQ_STATIC` and `ZMQ_BUILD_DRAFT_API`. Print
  `wvt: ZMQ streaming enabled (libzmq 4.3.5, static, draft API, built here)`.
- `zmq_check` (stage 1): one 8064-byte message through RADIO/DISH on `udp://127.0.0.1`.
- CI: `WVT_WITH_ZMQ=ON` in the Linux, macOS, sanitizer and clang-tidy jobs. ctest runs `zmq_check`.

### Phase 2 — Wire format (`src/net/zmq_wire.h/.cpp`, new; no libzmq)

- `StreamHeader`, `packStreamHeader` (the publisher writes the header into the ZMQ message and the
  samples after it), `packStreamInfo`, `parseStreamMessage` (returns false with a reason).
  `streamBytesPerSample` and `streamMaxSamples`: 4000 cu8, 2000 ci16 or 1000 cf32 samples in a
  message. cf32 is for the signal generator device.
- `parseStreamAddress` gives a `StreamAddress` (`ip`, `port`, `multicast`, `iface`) and its
  `endpoint()`, for example `udp://127.0.0.1:5600`. Without a port, 5600. An interface
  (`en0;239.10.0.1:5600`, the libzmq `udp://iface;group:port` form) only for a group. It rejects
  host names, IPv6, port 0 and ports above 65535, 0.x.x.x, and 240.0.0.0 and above (reserved and
  broadcast).
- `defaultStreamAddress` = `pickStreamAddress(streamInterfaces())`. `streamInterfaces` reads
  `getifaddrs`. `pickStreamAddress` (tested without the OS) takes the first interface that is up,
  is not the loopback and is not point-to-point (a VPN). It prefers one that runs (on Linux, an
  unused `docker0` does not run), and an address that is not link-local (169.254.x.x). If there is
  none, `127.0.0.1:5600`.
- `StreamTracker::next` gives *First*, *InOrder*, *Lost N*, *Late* (drop: late, repeated, or over
  the expected index) or *NewStream* (another stream ID).
- `zmq_check` now builds in every configuration. It tests all of this, and the socket test only
  with ZMQ.

### Phase 3 — Publisher (`src/net/zmq_pub.h/.cpp`, new)

- `StreamPublisher`: `start(ring, PublishSource, PublishConfig, err)`, `stop()`, `active()`,
  `noteRetune(pos, fc)`, `setGain(db, agc)`, `status()`. `PublishSource` has the format, the
  rate, the frequency, the gain and AGC, the driver and the description. `PublishConfig` has the
  address and the multicast TTL (default 1).
- The class has the same layout in every build (a private `Impl`), because `live.h` is in targets
  with and without ZMQ. Without ZMQ, `start()` fails with *this wvt was built without ZMQ*.
- `start()` parses the address, opens a RADIO socket (`ZMQ_SNDHWM` 10 000 messages, `ZMQ_SNDBUF`
  4 MB, `ZMQ_LINGER` 200 ms, `ZMQ_MULTICAST_HOPS` = TTL for a group) and connects. Errors come back
  from `start()`. It takes a random stream ID that is not 0, and starts at the newest sample of the
  ring: index 0 of the stream.
- Its thread reads the ring with `read()` (a lossy reader) into the ZMQ message after the header:
  at most 2000 ci16 samples, and never across a retune position. If it is more than 3/4 of the ring
  behind, it skips to 1/4 of the ring behind the newest sample. A failed read skips to the newest
  sample. `status().skipped` counts the skipped samples; the subscriber sees them as a jump in the
  index. When it has sent everything, it waits 5 ms.
- The time of a message is the wall clock at `start()` + index / rate.
- An info message each second. `stop()` sends what the ring has at that moment, then a data message
  with the end flag and 0 samples.
- RADIO drops a message without an error when its queue is full. Then the subscriber sees the gap.
- `LiveEngine`: `startPublishing`, `stopPublishing`, `publisher()`. The retune hook and
  `setCenterFreq` give the publisher the same ring position as `freshFrom_`. `setGain` passes the
  gain. `stop()` stops the publisher after the recorder and before the device. `start()` (a restart
  for a new rate or device option) publishes again to the same address, with a new stream ID.
- `zmq_check`: a publisher streams 100 000 ci16 samples to a DISH on the loopback, with a retune
  and a gain change half way: every sample, the order, the split at the retune, the gain and AGC
  flag, the time, the info and end messages. Then a ring of 4096 samples gets 200 000 samples at
  once: the samples that the subscriber lost are the samples that the publisher skipped.

### Phase 4 — Subscriber device (`src/device/sdr_zmq.cpp`, new)

- Driver `zmq`, only with ZMQ. `StreamConfig::args` is the address; empty is the default address.
  `sdrDrivers()` lists it, and `enumerateDevices()` adds one entry, *ZMQ stream (192.168.1.113:5600)*,
  with the default address. `checkSampleRate` accepts every rate for it: the source sets the rate.
- `open`: DISH socket (`ZMQ_RCVHWM` 20 000 messages, `ZMQ_RCVBUF` 8 MB, receive timeout 100 ms,
  `ZMQ_MULTICAST_LOOP` for a group), `zmq_bind`, `zmq_join("wvt")`. Then it waits up to 3 s for a
  data message, because `LiveEngine` needs the rate and the format before `start()`. It keeps that
  message, and `start()` delivers it first, so no samples are lost between open and start.
- libzmq binds a `udp://` endpoint later, on its I/O thread: `zmq_bind` returns 0 and a bind error
  is never reported, and libzmq allows a second socket on the port. So, for unicast, `open` first
  binds a plain UDP socket without `SO_REUSEADDR` and closes it. Errors: *192.0.2.1 is not an
  address of this computer: type an address of this computer, and send to it from the source*,
  *port 5600 is in use (another subscriber?)*, *no stream on 127.0.0.1:5600 in 3 s (is the source
  publishing to this address?)* (for a group also *Does the network pass multicast?*).
- Its thread: a jump in the index calls `gap_(lost, "network loss")`. A new frequency or the retune
  flag calls the retune hook (`usesRetuneMark()` = true) before the samples. Late and repeated
  messages are dropped. Messages that are not a wvt stream are ignored and logged (at most every
  5 s). The end message of the stream sets `fatalError()` = *the source stopped publishing*. A new
  stream ID with another rate or format sets *the source started again with another rate (2 MS/s):
  click Start again*; with the same rate and format, the stream goes on after a gap of unknown size.
- `notes()`: *waiting for the source: no message for N s* after 2 s without a message.
- `setCenterFreq` and `setGain` fail: *the source sets the frequency (this wvt receives the stream
  on 127.0.0.1:5600)*. `gainRange()` is the gain of the source (display only). `description()`:
  *ZMQ 127.0.0.1:5600: wvt simulator*, with the description from the info message.
- `zmq_check`, with ZMQ, now links the live engine. A live engine with the simulator publishes, and
  a second live engine opens the `zmq` device: the rate, frequency, format and description; a
  SigMF recording on the subscriber over a retune of the source (two captures, no gaps); the gain;
  no loss on the loopback; and the errors above, a second source with another rate, the source
  that stops, and a restart of the source at another rate. Then the same on a multicast group
  (passed on the development Mac), skipped where multicast does not work.

### Phase 5 — Command line (`src/app/cli.cpp`, `main.cpp`)

- `--publish ADDR` publishes the live stream. With `--no-gui`, wvt streams until Ctrl+C, with or
  without `--record`. This is the main use on a remote computer:

  ```sh
  wvt --device uhd -c 915M -r 10M --publish 192.168.1.50:5600 --no-gui   # to the subscriber at 192.168.1.50
  ```

- `--publish-ttl N` (default 1).
- `--device zmq:192.168.1.50:5600` subscribes (on the computer with that address). `--device zmq` alone uses the default address. `-c`, `-r` and `--gain` are not allowed with it.

### Phase 6 — GUI (`src/app/app_live.cpp`, `app.h`)

- Device row: for `zmq`, an **Address** field in the place of **Args**. It starts with the
  default address. The tooltip: *Where this wvt receives the stream (Enter applies):
  an address of this computer, e.g. 192.168.1.20:5600 or 127.0.0.1:5600 (send to it from the
  source); or a multicast group, e.g. 239.10.0.1:5600 (the same group as the source)*.
- For `zmq`: center frequency, rate, gain, AGC and the ⚙ device options are read only and show
  the values from the stream. The status bar shows the source description and *network loss*
  counts.
- Record bar: a **Publish** / **Stop publishing** button next to **Record**, and a ⚙ popup with
  **Address** (the default address at first; tooltip: *the address of the subscriber, or a
  multicast group*) and **TTL** (multicast only). It works with and without a recording, and it does not lock the
  tuning. The status bar shows the publish rate in MB/s and the skipped samples. **Publish** is
  disabled when the source is a `zmq` stream to the same address (a loop).
- `--publish` in the GUI starts publishing when the device starts.

### Phase 7 — Configuration file (`src/app/config.cpp`, `docs/config.md`)

- `zmq.address` (empty: the default address, §3) and `zmq.ttl = 1`. They set the defaults of
  both sides.

### Phase 8 — Checks and docs

- `tools/zmq_check.cpp` (new): header round trip and rejection cases; address parser;
  `SampleTracker` (order, loss, late, new stream); `defaultStreamAddress` gives a valid address.
  A loopback test: a publisher on a simulated ring sends to `127.0.0.1:<free port>`, and a `zmq`
  device receives there. It compares the samples, the frequency, a retune and the gap count. This
  test always runs (CI too). The same test on a multicast group prints *SKIP: multicast not
  available* instead of failing on a computer without a multicast route.
- `tools/fuzz/fuzz_zmq_header.cpp` (new) and a corpus.
- `docs/live.md`: new section *Stream over the network*: the two commands, multicast and firewall
  notes (UDP port, IGMP snooping, Wi-Fi is poor for multicast), bandwidth table, no security.
- `docs/cli.md`, `docs/source-layout.md`, `docs/design/index.md` (this record).

## 6. Verification

1. `zmq_check` and the fuzz target (short run) pass on macOS and Linux.
2. **Loopback, two instances on one computer** (the owner's test):

   ```sh
   wvt --device sim -c 100M -r 2.4M --publish 127.0.0.1:5600     # instance 1: the source
   wvt --device zmq:127.0.0.1:5600                               # instance 2: the subscriber
   ```

   Also in the GUI only: in instance 1, select the simulator, **Start**, then **Publish** to
   `127.0.0.1:5600`. In instance 2, select *ZMQ stream*, type `127.0.0.1:5600` in **Address**,
   **Start**. Check:
   - The waterfalls and the frequency, rate and gain readouts agree.
   - Retune instance 1: instance 2 follows. Instance 2 cannot change the frequency.
   - Start instance 2 before instance 1: it reports *no stream* after 3 s, then works when you
     click **Start** again.
   - Stop instance 1: instance 2 shows *the source stopped*. Start instance 1 at 10 MS/s: instance
     2 asks for a restart and then works at 10 MS/s.
   - Raise the rate of instance 1 to 20 and 50 MS/s, and note the loss on instance 2.
3. Record SigMF on both instances of test 2 at the same time, with a retune during the recording.
   The files have the same samples (allowing for the start position) and the same capture
   segments. Gaps on the subscriber have annotations.
4. The default address: with no `zmq.address`, both instances show the address of the computer
   (`192.168.x.y:5600`); test 2 works with it too. With the network off, they show
   `127.0.0.1:5600`.
5. Multicast: test 2 with `239.10.0.1:5600` on both instances.
6. Two computers on a wired LAN with a real device (RTL-SDR 2.4 MS/s, USRP 10 and 20 MS/s): the
   source publishes to the address of the subscriber. Note the loss at each rate.
7. Headless: `--publish ... --no-gui` with and without `--record`, Ctrl+C stops cleanly.

## 7. Risks and open points

- The 8 KB datagram limit gives many messages: 20 MS/s `ci16` is 10 000 messages/s. If libzmq
  cannot send that many, a later version can add a larger message with IP fragmentation, or
  plain UDP sockets.
- With unicast, the default address is right for the subscriber (its own address) and for a
  publisher on the same computer. A publisher that sends to another computer must change it to
  the address of that computer. The tooltips and `docs/live.md` say so.
- With unicast, one subscriber can listen on an address and port. A second subscriber on the
  same computer needs another port, or both use a multicast group.
- Multicast first did not work on the development Mac, even with plain sockets: a network
  permission blocked it. After the owner fixed the permission (2026-11-03), the multicast part of
  `zmq_check` passed. macOS can block multicast for a program that does not have the *Local
  Network* permission, so `docs/live.md` must say so. Where multicast does not work, the multicast
  part of `zmq_check` skips.
- Multicast on macOS can need an interface (`en0;239.10.0.1:5600`) when there is more than one
  network.
- FetchContent needs the network at configure time. Builds without a network need
  `WVT_ZMQ_SOURCE_DIR` or `WVT_WITH_ZMQ=OFF`.
- The draft API of libzmq can change between versions. wvt pins 4.3.5.

## 8. Files touched

| File | Change |
|---|---|
| `CMakeLists.txt`, `.github/workflows/build.yml` | libzmq, `WVT_WITH_ZMQ`, `zmq_check`, fuzz target |
| `src/net/zmq_wire.h/.cpp` | New: header, address parser, sample tracker |
| `src/net/zmq_pub.h/.cpp` | New: publisher |
| `src/device/sdr_zmq.cpp` | New: subscriber backend |
| `src/device/sdr_device.h/.cpp` | `zmq` driver, device list entry, rate check |
| `src/device/live.h/.cpp` | Publisher next to the recorder; retune marks |
| `src/app/app_live.cpp`, `app.h` | Address field, read-only tuning, Publish button and popup, status |
| `src/app/cli.cpp`, `main.cpp` | `--publish`, `--publish-ttl`, `--device zmq:` |
| `src/app/config.cpp/.h` | `zmq.address`, `zmq.ttl` |
| `tools/zmq_check.cpp`, `tools/fuzz/fuzz_zmq_header.cpp` | New checks |
| `docs/live.md`, `docs/cli.md`, `docs/config.md`, `docs/source-layout.md`, `docs/design/index.md` | Docs |
