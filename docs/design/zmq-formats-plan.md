# Plan: ZMQ message formats from configuration files (issue #101)

Status: phases 1 (config folder), 2 (format files and decoder), 3 (subscriber device), 4 (selection) and 5 (docs) implemented on `zmq-formats` (2026-11-05). Phase 6 (publisher) not started.

## 1. Goal

From the issue:

- Connect wvt to other tools that send samples over ZMQ in their own message formats.
- A standard file format describes one ZMQ message structure. Each structure is a separate file,
  `config/zmq_*.ini`.
- At run time, the user selects the format from a drop-down list. wvt fills the list from the
  `zmq_*.ini` files that it finds.

From the owner (2026-11-05):

- First the **subscriber** (the *ZMQ stream* device) gets the selectable formats. A later phase
  lets the **publisher** use the same files.
- A **local config folder** keeps user configuration files. The ZMQ format files are the first
  files in it.
- `wvt.conf` moves to the same folder, so all settings are in one place. The folder is found from
  `--config-dir`, from where the program is, or in the home folder (`~/.config/wvt/`); there is no
  environment variable for it.

## 2. What wvt does now

- The wire format is fixed (`src/net/zmq_wire.h`): a 64-byte header with the magic `WVTZ`, then
  `cu8`, `ci16` or `cf32` samples. `parseStreamMessage` reads it. Info messages carry the device
  text.
- The subscriber (`src/device/sdr_zmq.cpp`) has one socket type: DISH on `udp://`, group `wvt`,
  bind. It waits up to 3 s for the first data message, because `LiveEngine::start` needs the
  rate and the format before it starts. A sample index in the header gives loss detection
  (`StreamTracker`).
- The address is `StreamConfig::args`. The **Address** field in the device toolbar
  (`src/app/app_live.cpp`, `drawDeviceToolbar`) sets it. `zmq.address` in the configuration file
  sets the default.
- The configuration file is found by `configPath` (`src/app/config.cpp`): `--config`, then
  `$WVT_CONFIG`, then `$XDG_CONFIG_HOME/wvt/wvt.conf`, then `~/.config/wvt/wvt.conf`. wvt has no
  other configuration folder, no INI reader, and no function that finds the folder of the
  executable.

## 3. Decisions

| Question | Decision | Why |
|---|---|---|
| Config folder | **`config/` next to the program, else `~/.config/wvt/`** (owner, 2026-11-05). wvt uses the **first** of these that exists, and only that one: 1. `--config-dir DIR`; 2. `<exe folder>/config/`; 3. `<exe folder>/../config/`; 4. `~/.config/wvt/`. No environment variable (owner, 2026-11-05) | Entry 3 finds the repository's `config/` (at the repository root) when wvt runs from `build/wvt`. Entry 4 is for an installed wvt and for users who keep their settings in the home folder. One folder, not a merge, so it is always clear which files are in use. |
| `wvt.conf` location | **`<config folder>/wvt.conf`** (owner, 2026-11-05). The order: `--config FILE`, then `$WVT_CONFIG`, then `<config folder>/wvt.conf`. `$XDG_CONFIG_HOME` is no longer read | All settings in one place. `--config` and `$WVT_CONFIG` name one file, for scripts and checks, so they stay. A user with `~/.config/wvt/wvt.conf` and no `config/` folder next to wvt sees no change. |
| Files that are not used | If wvt uses a `config/` folder and `~/.config/wvt/` also has `wvt.conf` or `zmq_*.ini` files, wvt logs one line: *~/.config/wvt/ is not used, because wvt uses <folder>*. `--show-config` shows it too | A user who edits the home folder files must see why nothing changes. |
| Repository files | `config/wvt.conf.example`: the output of `--write-config`, tracked. `config/wvt.conf` is the user's file and is ignored. `wvt_config.txt` at the repository root is removed | The example shows every key. The user's own settings are not committed. |
| Files in the folder | `zmq_*.ini`, one format per file. Later features add other `<feature>_*.ini` files | The pattern from the issue. |
| Socket types | **DISH (UDP), SUB and PULL (TCP or UDP where libzmq allows it)** (owner, 2026-11-05). The format file selects the type | Most other tools (for example GNU Radio's ZMQ blocks) send on PUB or PUSH over TCP. |
| The current wvt format | Stays built in, with the name `wvt`. It is always in the list, also when the folder is missing | Nothing that works now stops working. Its info messages and its sample format byte do not fit the generic model. |
| Message model | A fixed-size header (0 bytes or more) with fields at fixed offsets, then the samples to the end of the message (or the last frame) | Covers raw samples (GNU Radio without tags) and simple binary headers. Variable layouts (PMT tags, VITA-49 with optional fields, JSON) are not in scope. |
| Values not in the header | The file gives them (`[stream] rate = 2e6`). `rate` is required, from a field or from the file | `LiveEngine` must know the rate before it starts. |
| Sample formats | `cu8`, `ci8`, `ci16`, `ci32`, `cf32`, `cf64`; little or big endian; I/Q or Q/I order | The formats of `SampleFormat`. wvt converts the formats that the ring does not carry (all but little-endian `cu8`, `ci16`, `cf32` in I/Q order) to `cf32`. |
| Loss detection | From an `index` field (samples), or a `seq` field (messages). No field: no loss detection | TCP does not lose messages. UDP formats should have one of the two fields. |
| Selection | `zmq.format = NAME` in `wvt.conf`, `--zmq-format NAME`, and a **Format** drop-down next to **Address**. The default is `wvt` | Same pattern as `zmq.address`. |
| Bad files | wvt lists a bad file in the drop-down, disabled, with the error as the tooltip, and logs it. The other files still load | The user sees why a file is missing from the choices. |
| Security | The decoder checks every offset and size against the message length. A fuzz target tests the INI parser and the decoder | The messages and the files come from outside wvt. |

## 4. The format file

INI: `[section]` lines, `key = value`, comments start with `#` or `;`. Numbers use the rules of
the configuration file (`parseConfigNumber`: `2.4e6`, no SI suffixes). Unknown keys give a warning
with "did you mean".

```ini
# config/zmq_example_header.ini
[format]
name        = Example header, ci16 big endian   # the text in the drop-down; default: the file name
description = 32-byte header, then samples      # the tooltip

[socket]
type      = sub            # dish | sub | pull
transport = tcp            # tcp | udp (dish: udp only)
mode      = connect        # bind | connect (default: dish bind, sub and pull connect)
address   = 127.0.0.1:5555 # the default for the Address field
topic     =                # sub: the topic prefix to subscribe to; empty = all messages
group     = wvt            # dish: the group to join
frames    = last           # multipart messages: which frame has the data: last | 1 | 2 ...
max_bytes = 1048576        # a larger message is dropped (and ZMQ_MAXMSGSIZE is set)

[header]
bytes       = 32               # the samples start here; 0 = no header
match.magic = "ABCD" @ 0       # a message that does not match is ignored
match.type  = u8 @ 4 == 1
seq         = u32be @ 8        # message counter (or: index = u64le @ 8, a sample counter)
fc          = f64be @ 12
rate        = u32be @ 20 * 1e3 # the field is in kS/s
time_ns     = u64be @ 24       # optional: ns since 1970
# also: gain, count (samples in the message; default: from the message length)

[samples]
format = ci16              # cu8 | ci8 | ci16 | ci32 | cf32 | cf64
endian = big               # little | big
order  = iq                # iq | qi

[stream]                   # the values that the header does not give
# rate = 2e6
# fc   = 100e6
gain   = 0
```

A field is `TYPE @ OFFSET [* SCALE]`. TYPE is `u8`, `i8`, `u16le`, `u16be`, ... `u64be`,
`i16le` ... `i64be`, `f32le`, `f32be`, `f64le`, `f64be`. A match is a field with `== VALUE`, or a
quoted string at an offset.

The repository ships these examples in `config/`:

- `zmq_gnuradio_sub_cf32.ini`: GNU Radio *ZMQ PUB Sink*, complex float, tags off. SUB on TCP, no
  header, the rate from the file.
- `zmq_raw_pull_ci16.ini`: raw `ci16` on PULL, no header.
- `zmq_example_header.ini`: the file above, as a template.
- `zmq_wvt_ci16.ini`: the built-in format, written as a file (data messages with `ci16` samples
  only). It is a template, and the checks use it (phase 2).
- `README.md`: what the folder is and the file format in short.

## 5. Implementation

### Phase 1 — Config folder and `wvt.conf` (`src/common/paths.h/.cpp`, new; `src/app/main.cpp`, `cli.cpp`, `config.cpp`)

- `executableDir()`: `_NSGetExecutablePath` on macOS, `readlink("/proc/self/exe")` on Linux, then
  `realpath`.
- `configDir(option)`: the search order in section 3. It returns an empty string if no folder
  exists. wvt does not make the folder at start. A `--config-dir` that does not exist is an error.
- `--config-dir DIR`, taken out of the arguments as `configTakeArgs` does for `--config`, before
  the configuration file is read.
- `configPath(option, configDir, required)`: `--config`, then `$WVT_CONFIG`, then
  `<config folder>/wvt.conf` (optional: missing is not an error). The `$XDG_CONFIG_HOME` entry is
  removed. The note for files that are not used (section 3).
- `--write-config` with no FILE writes `<config folder>/wvt.conf`. If no folder exists, it makes
  `~/.config/wvt/` and writes there. It does not replace a file that exists.
- `--show-config` shows the folder, why wvt picked it, and the file it read.
- Repository: a new `config/` folder at the root with the ZMQ examples, `wvt.conf.example` and
  `README.md`. `.gitignore` ignores all other files in `config/` (`wvt.conf` too), so the user's own
  files are not committed by mistake. `wvt_config.txt` is removed; to keep its settings, copy it to
  `config/wvt.conf`.
- CMake: no copy step. `<exe folder>/../config/` finds the repository folder from `build/`.
- Checks (`tools/config_check.cpp`): each step of the folder search (with a temporary executable
  folder and `HOME`), `--config-dir`, and the note for files that are not used. The
  `$XDG_CONFIG_HOME` case is removed.

### Phase 2 — Format model and INI reader (`src/net/zmq_format.h/.cpp`, new; no libzmq)

- `IniFile parseIni(text, name, warnings)`: sections, keys, line numbers for the messages.
- `StreamFormat`: the socket settings, the header fields and matches, the sample layout, the fixed
  values. `bool loadStreamFormat(path, StreamFormat&, std::vector<std::string>& warnings, err)`
  checks it all: the fields are inside `bytes`, `rate` is given, the types are known, `dish` uses
  `udp`, and so on.
- `std::vector<StreamFormatEntry> scanStreamFormats(dir)`: the built-in `wvt`, then the
  `zmq_*.ini` files sorted by name, each with its name and its error (if any).
- `bool decodeStreamMessage(const StreamFormat&, const uint8_t*, size_t, StreamMessage&, why)`:
  the generic decoder. It fills the same `StreamMessage` that `parseStreamMessage` fills, so the
  device code stays the same for both. The built-in format calls `parseStreamMessage`.
- Sample conversion to the ring format: byte swap, Q/I swap, and `ci8`, `ci32`, `cf64` to `cf32`.
- Checks (`tools/zmq_check.cpp`): each example file loads. A wvt `ci16` message decodes the same
  with `zmq_wvt_ci16.ini` and with the built-in parser. Bad files give the expected errors. Byte
  order, scale and Q/I order are correct.
- Fuzz (`tools/fuzz/fuzz_zmq_format.cpp`): the INI reader, and the decoder with a format from the
  corpus.

### Phase 3 — Subscriber device (`src/device/sdr_zmq.cpp`, `src/device/sdr_device.h`)

- `StreamConfig::format`: the name of the format. Empty is `wvt`.
- `open()` loads the format, makes the socket (`ZMQ_DISH`, `ZMQ_SUB`, `ZMQ_PULL`), and binds or
  connects. For SUB it sets `ZMQ_SUBSCRIBE` to the topic. It sets `ZMQ_MAXMSGSIZE` from
  `max_bytes`. The bind check (`probeBind`) runs only for a unicast UDP bind.
- The address: the Address field, else `[socket] address`, else the current default. A TCP address
  is `host:port`; a full `tcp://` or `udp://` endpoint is also accepted.
- Multipart messages: wvt reads all frames and decodes the frame that `frames` selects.
- The wait for the first message stays. If the rate comes from the file, the first message still
  must arrive: it shows that the source sends.
- `StreamTracker` gets a mode for `seq` (message counter). With no `index` and no `seq`, wvt does
  not report loss.
- Error messages name the format, for example *no message in 3 s on tcp://127.0.0.1:5555 (format
  GNU Radio PUB Sink)*, and *messages arrive, but none match the format* when the matches fail.
- `description()` shows the format name.

### Phase 4 — Selection (`src/app/config.cpp`, `cli.cpp`, `app_live.cpp`, `app.h`, `src/io/session.cpp`)

- `zmq.format` in the key table, with help text. `--zmq-format NAME` on the command line.
  `--list-zmq-formats` prints the list with errors, for scripts and for a check of new files.
- GUI: a **Format** combo next to **Address** when the device is *ZMQ stream*. wvt scans the
  folder each time the combo opens, so a new file shows without a restart. A change while the
  device runs restarts it, as the Address field does. The tooltip of each entry is the file path
  and the description. A bad file is disabled, with its error.
- When a format has a default address and the Address field is empty, the field shows it as the
  hint text.
- Sessions: not done. A session (issue #76) saves the file viewer only, not the live device or
  its address, so there is no place for the format. `zmq.format` is the saved choice.
- `zmq.address` is the address of the built-in format. A format file uses its own address: when
  the format changes, the Address field is emptied and shows the file's address as hint text.

### Phase 5 — Docs

- `docs/live.md`: *Stream over the network* gets *Receive other formats*.
- New `docs/zmq-formats.md`: the format file reference (section 4), the examples, and how to test
  a file with `--list-zmq-formats`.
- `docs/config.md`: the config folder and its search order, `wvt.conf` in it, `zmq.format`, `--config-dir`. `docs/cli.md`: the new options, and `--config`
  and `WVT_CONFIG` with the new default.

### Phase 6 — Publisher (later)

- `decode` gets the reverse, `encodeStreamMessage`. A format can publish if every header field
  has a value that wvt knows (fc, rate, gain, index, seq, time, count) and the matches are
  constants.
- Socket types RADIO, PUB and PUSH. Sample conversion from the native format to the format of the
  file.
- A **Format** combo in the **Publish** ⚙ popup, `publish.format` in `wvt.conf`,
  `--publish-format` on the command line. The built-in `wvt` format stays the default.
- `[socket]` may need different defaults for the publisher (for example PUB binds). The file gets
  a `[publish]` section if needed.

## 6. Verification

- `zmq_check` and the fuzz target (phase 2). The fuzz target runs in CI with the others.
- Loopback test in `zmq_check`: a PUB and a PUSH socket on `tcp://127.0.0.1` send example
  messages, and the device receives them through each example file.
- By hand: GNU Radio *Signal Source* → *ZMQ PUB Sink* (complex, tags off) into wvt with
  `zmq_gnuradio_sub_cf32.ini`. Then two wvt with the built-in format, to show that #96 still works.
- A bad file in the folder: the drop-down shows it disabled, and `--list-zmq-formats` gives the
  error.

## 7. Risks and open points

- **TCP connect does not fail.** libzmq connects in the background, so a wrong address shows
  only as *no message in 3 s*. The error text must say this.
- **SUB slow joiner.** A SUB socket loses the messages before its subscription reaches the
  publisher. This is not a problem for a live stream.
- **Large TCP messages.** One message can be megabytes. `max_bytes` and `ZMQ_RCVHWM` together
  limit the memory. The default `max_bytes` (1 MiB) and HWM must be chosen together.
- **Conversion cost.** `cf64` and big-endian formats are converted on the receive thread. At high
  rates this can lose samples. Measure in phase 3.
- **Feedback loop check.** `sameStreamAddress` (app_live.cpp) compares UDP addresses only. It must
  also compare the transport.
- **A `config/` folder hides the home folder.** When wvt runs from `build/`, the repository's
  `config/` is used and `~/.config/wvt/` is not. The note (section 3) tells the user. Users of
  `$XDG_CONFIG_HOME` must move their files to `~/.config/wvt/`; the release notes say so.
- **A build folder outside the repository.** `<exe folder>/../config/` does not find the
  repository's `config/` from, for example, `/tmp/wvt-build`. wvt then uses `~/.config/wvt/`, or
  `--config-dir`.
- **An installed wvt.** A system install (`/usr/local/bin`) has no `config/` next to it, so it uses
  `~/.config/wvt/`. The ZMQ example files are not in that folder; the docs tell the user to copy
  them from the repository.

## 8. Files touched

| File | Change |
|---|---|
| `src/common/paths.h/.cpp` | New: `executableDir`, `configDir` |
| `src/net/zmq_format.h/.cpp` | New: INI reader, `StreamFormat`, scan, decoder, sample conversion |
| `src/net/zmq_wire.h/.cpp` | `StreamTracker` `seq` mode |
| `src/device/sdr_zmq.cpp`, `sdr_device.h` | Socket types, format decoder, `StreamConfig::format` |
| `src/app/config.h/.cpp` | `configPath` from the config folder, the note for files that are not used, `zmq.format`, the folder in `--show-config` |
| `src/app/cli.cpp`, `main.cpp` | `--config-dir`, `--write-config` default, `--zmq-format`, `--list-zmq-formats` |
| `src/app/app_live.cpp`, `app.h` | **Format** combo, address hint, loop check |
| `src/io/session.cpp` | Save the format |
| `config/` | New: ZMQ example files, `wvt.conf.example`, `README.md` |
| `wvt_config.txt` | Removed |
| `.gitignore` | Ignore user files in `config/` |
| `tools/config_check.cpp` | The new search order |
| `CMakeLists.txt` | New sources, `fuzz_zmq_format` |
| `tools/zmq_check.cpp`, `tools/fuzz/` | Checks, fuzz target and corpus |
| `docs/zmq-formats.md`, `live.md`, `config.md`, `cli.md`, `design/index.md` | Docs |

`docs/design/config-file-plan.md` is not changed: design records are a history.
