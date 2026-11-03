# Plan: publish a live stream over ZMQ and UDP (issue #96)

Status: phase 1 (build) implemented (2026-11-03) on `zmq-udp-stream`. Phases 2 to 8 are planned.

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
| 6 | `u8` | sample format (`SampleFormat`: cu8, ci16) |
| 7 | `u8` | flags: bit 0 AGC, bit 1 retune at the first sample, bit 2 end of stream |
| 8 | `u32` | stream ID: random, new for each start of the source |
| 12 | `u32` | sample count in this message |
| 16 | `u64` | sample index of the first sample, from the start of the stream |
| 24 | `f64` | center frequency, Hz |
| 32 | `f64` | sample rate, S/s |
| 40 | `f64` | gain (dB; BB60: reference level, dBm) |
| 48 | `u64` | time of the first sample, ns since 1970 (UTC) |
| 56 | `u8[8]` | reserved, zero |

Info message: the same first 16 bytes, then the driver (`rtlsdr`, `uhd`...) and the description
(`"USRP B205mini serial 31A2B3C"`) as length-prefixed strings.

The parser rejects a message if the magic, the version, the kind or the format is unknown, if the
count does not agree with the payload size, or if the rate or frequency is not finite and positive.

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

- `struct StreamHeader`, `packData`, `packInfo`, `parse` (returns false with a reason).
- `parseStreamAddress("192.168.1.20:5600")` to an endpoint `udp://192.168.1.20:5600` and a
  `multicast` flag. A multicast group can have an interface: `en0;239.10.0.1:5600` (the libzmq
  `udp://iface;group:port` form). It rejects a host name, a port outside 1-65535, and IPv6 (for
  now).
- `defaultStreamAddress()`: the first IPv4 address of an interface that is up, is not the
  loopback (`getifaddrs`), with port 5600. If there is none,
  `127.0.0.1:5600`. The configuration file replaces it.
- `SampleTracker`: given the expected index and a header, returns *in order*, *lost N*, *late
  (drop)* or *new stream*. The subscriber uses it, and the self-check tests it without sockets.

### Phase 3 — Publisher (`src/net/zmq_pub.h/.cpp`, new)

- `class StreamPublisher { bool start(shared_ptr<SampleRing>, StreamSource, addr, err); void stop();
  Status status() const; void noteRetune(int64_t pos, double fc); }`. `StreamSource` has the
  format, rate, driver, description, and a function that returns the gain and AGC.
- Its thread: RADIO socket, `zmq_connect(endpoint)`. For a group, `ZMQ_MULTICAST_HOPS` = TTL
  (default 1). For unicast, the address is that of the subscriber.
  It reads from `ring->written()` on, in blocks of at most 8000 bytes, with `ring->read()`. If a
  read fails (overwritten), it skips to the newest samples. It splits a block at a retune position
  and sets the retune flag on the next message. One info message each second. An end-of-stream
  message at stop.
- Status: messages/s, MB/s, samples skipped. A warning in the status bar above about 80 % of a
  1 GbE link (approximately 25 MS/s `ci16`).
- `LiveEngine` owns the publisher next to `rec_`: `startPublishing`, `stopPublishing`,
  `publisher()`. The retune hook and `setCenterFreq` call `pub_.noteRetune` where they call
  `rec_.noteRetune`. `stop()` stops it.

### Phase 4 — Subscriber device (`src/device/sdr_zmq.cpp`, new)

- Driver `zmq`. `StreamConfig::args` is the address. `openZmqDevice`: DISH socket,
  `zmq_bind(endpoint)`, `zmq_join("wvt")`. For a group, `ZMQ_MULTICAST_LOOP` is on (two wvt on
  one computer). For unicast, the address must be on this computer. If the bind fails, the error
  says so: *192.168.1.50 is not an address of this computer: type an address of this computer,
  and send to it from the source*, or *port 5600 is in use (another subscriber?)*.
- It waits up to 3 s for a data message to get the rate, the format and the frequency, because
  `LiveEngine` needs them before `start()`. Error: *no stream on 127.0.0.1:5600 in 3 s (is the
  source publishing to this address?)*, and for a group, *does the network pass multicast?*.
- Its thread receives messages with a 100 ms timeout and calls `data_`. It uses the
  `SampleTracker`: a jump calls `gap_(lost, "network loss")`; a frequency change or the retune flag
  calls the retune hook (`usesRetuneMark()` = true) before the samples, so a recording on the
  subscriber starts its new capture at the correct sample. A new stream ID with a different rate or
  format, or an end-of-stream message, sets `fatalError()`. If no message comes for 2 s, the status
  shows *waiting for the source*.
- `setCenterFreq` and `setGain` return false: *the source sets the frequency*. `gainRange()` uses
  the gain from the header (display only). `description()`: *ZMQ 239.10.0.1:5600: USRP B205mini
  serial 31A2B3C* (from the info message).
- `sdrDrivers()` adds `zmq`. `enumerateDevices()` adds one entry *ZMQ stream* with the default
  address (from the configuration file, if it sets one). `checkSampleRate("zmq")` accepts all.

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
