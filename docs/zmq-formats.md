# ZMQ message formats

The **ZMQ stream** device receives the stream of another wvt in the built-in format `wvt`. It can
also receive samples from other programs, for example GNU Radio. A format file tells wvt the
structure of the messages from such a program. This page is the reference for the format files.

For how to receive a stream, refer to [Receive other formats](live.md#receive-other-formats).

## Where wvt finds the files

A format file is `zmq_NAME.ini` in the [config folder](config.md#the-config-folder). `NAME` is the
name of the format: for `zmq_gnuradio_sub_cf32.ini`, it is `gnuradio_sub_cf32`. wvt reads only the
config folder that it uses. It does not read the files in the other folders.

The format `wvt` is always available, also without a config folder. Do not name a file `zmq_wvt.ini`:
the name `wvt` is the built-in format, and wvt shows an error for that file.

wvt reads the files again each time that you open the **Format** list. You do not have to start wvt
again after you add or change a file.

## Check a file

1. Put the file in the config folder.
2. Run this command:

   ```sh
   wvt --list-zmq-formats
   ```

wvt shows each format with its file, its socket and its samples. A file that has a problem shows the
error. Most errors include the line number. A key that wvt does not know gives a warning, with the key that you
possibly mean. The command stops with exit code 1 if a file has an error, so that a script can use
it.

```text
ZMQ message formats (/home/user/wvt/config):
* wvt                    the wvt stream (built-in)
      dish, udp bind, the wvt header
  gnuradio_sub_cf32      GNU Radio PUB Sink (cf32)
      file:  /home/user/wvt/config/zmq_gnuradio_sub_cf32.ini
      sub, tcp connect 127.0.0.1:5555, cf32 (complex float32)
* = zmq.format, the default of --device zmq
```

In the **Format** list, a file with an error shows, but you cannot select it. Its tooltip shows the
error.

## The file

The file is an INI file:

- A `[section]` line starts a section. The sections are `[format]`, `[socket]`, `[header]`,
  `[samples]` and `[stream]`.
- Each setting is `key = value`.
- `#` or `;` starts a comment, at the start of a line or after a space.
- Numbers are written as in `wvt.conf`, for example `2.4e6`. SI suffixes such as `2.4M` are not
  permitted.

This example is `config/zmq_example_header.ini` in the repository. Copy it to start a new file:

```ini
[format]
name        = Example header (ci16 BE)
description = 32-byte header, then big-endian ci16 samples

[socket]
type      = sub
transport = tcp
mode      = connect
address   = 127.0.0.1:5557
topic     =
frames    = last
max_bytes = 1048576

[header]
bytes       = 32
match.magic = "ABCD" @ 0         # a message that does not match is ignored
match.type  = u8 @ 4 == 1
seq         = u32be @ 8          # message counter
fc          = f64be @ 12
rate        = u32be @ 20 * 1e3   # the field is in kS/s
time_ns     = u64be @ 24

[samples]
format = ci16
endian = big
order  = iq

[stream]
gain = 0        # the header has no gain
```

### [format]

| Key | Default | Function |
|---|---|---|
| `name` | The `NAME` of the file | The text in the **Format** list |
| `description` | Empty | The tooltip in the **Format** list |

### [socket]

| Key | Default | Function |
|---|---|---|
| `type` | Necessary | The ZMQ socket that wvt opens: `dish`, `sub` or `pull`. The source sends with `radio`, `pub` or `push` |
| `transport` | `udp` for `dish`, else `tcp` | `dish` works only on `udp`. `sub` and `pull` work only on `tcp` |
| `mode` | `bind` for `dish`, else `connect` | `bind`: wvt listens and the source sends to wvt. `connect`: wvt connects to the source. `dish` works only with `bind` |
| `address` | Empty | The address when the **Address** field is empty. `host:port`. With `mode = bind`, `*:port` listens on all interfaces |
| `topic` | Empty (all messages) | `sub` only: wvt receives only the messages that start with this text |
| `group` | `wvt` | `dish` only: the group to join, 1 to 255 characters |
| `frames` | `last` | A message with more than one frame (for example a topic frame, then the data): the frame with the data. `last`, or 1 to 16 |
| `max_bytes` | `1048576` | The largest frame, 64 bytes to 64 MiB. libzmq drops a larger frame. On `tcp`, libzmq also closes the connection to the source |

The address of a `dish` format is as for the built-in format: an address of this computer, or a
multicast group. Refer to [The address](live.md#the-address).

wvt limits the queue of messages to approximately 160 MB. A larger `max_bytes` gives a shorter
queue. Set `max_bytes` near the size of the largest message from the source.

### [header]

The header is the bytes at the start of each message. The samples start after it.

| Key | Function |
|---|---|
| `bytes` | The size of the header, 0 to 65536 bytes. `0`: the message has only samples |
| `match.NAME` | Bytes that each message must have. wvt ignores a message that does not match. You can have more than one match, each with its own `NAME` |
| `fc` | The center frequency, in Hz |
| `rate` | The sample rate, in S/s |
| `gain` | The gain, in dB |
| `time_ns` | The time of the first sample, in ns since 1970. wvt reads it, but does not use it now |
| `index` | The index of the first sample: a counter of samples since the start of the stream |
| `seq` | A message counter: 1 more in each message |
| `count` | The number of samples in the message. Without it, all the bytes after the header are samples |

A field is `TYPE @ OFFSET`, for example `u32be @ 8`. `OFFSET` is the position of the first byte in
the message. The first byte of the message has the offset 0. The field must be in the header.

`fc`, `rate`, `gain` and `time_ns` can have a scale: `TYPE @ OFFSET * SCALE`. wvt multiplies the value
by the scale. For example, `rate = u32be @ 20 * 1e3` reads a rate in kS/s. `index`, `seq` and `count`
must have an integer type and no scale.

The types are:

| Type | Size | Value |
|---|---|---|
| `u8`, `i8` | 1 byte | Unsigned or signed integer |
| `u16le`, `u16be`, `i16le`, `i16be` | 2 bytes | Integer, little endian (`le`) or big endian (`be`) |
| `u32le`, `u32be`, `i32le`, `i32be` | 4 bytes | Integer |
| `u64le`, `u64be`, `i64le`, `i64be` | 8 bytes | Integer |
| `f32le`, `f32be` | 4 bytes | Floating point |
| `f64le`, `f64be` | 8 bytes | Floating point |

A match has one of these forms:

- `TYPE @ OFFSET == VALUE`: a number. For example, `match.type = u8 @ 4 == 1`.
- `"TEXT" @ OFFSET`: the bytes of a text. For example, `match.magic = "ABCD" @ 0`.

Use a match when the source sends more than one type of message. Then wvt uses only the messages
with samples.

### [samples]

| Key | Default | Function |
|---|---|---|
| `format` | Necessary | The samples: `cu8`, `ci8`, `ci16`, `ci32`, `cf32` or `cf64`. Each sample is an I value and a Q value |
| `endian` | `little` | The byte order of each value: `little` or `big` |
| `order` | `iq` | `iq`: I first. `qi`: Q first |

wvt keeps `cu8`, `ci16` and `cf32` as they are. It changes the other formats to `cf32`.

### [stream]

The values that the header does not give.

| Key | Function |
|---|---|
| `rate` | The sample rate in S/s. It is necessary if the header has no `rate` field |
| `fc` | The center frequency in Hz (default 0). wvt shows it on the frequency axis |
| `gain` | The gain in dB (default 0) |

A field in the header wins over a value in `[stream]`.

## Lost messages

wvt finds lost messages with a counter in the header:

- With `index`, wvt knows the exact number of lost samples. If the index goes back by more than the
  samples of 1 s, wvt shows that the source started again.
- With `seq`, wvt counts the lost messages. The number of lost samples is the lost messages
  multiplied by the samples of one message, so it is an estimate. The counter can go back to 0 when it
  is at its maximum value.
- Without `index` and `seq`, wvt does not find lost messages. TCP does not lose messages, but a
  `pub` socket drops messages when the subscriber is too slow.

The status bar shows the lost samples as *network loss*, and a recording has an annotation for each
gap. Set `index` or `seq`, not both.

If the sample rate in the header changes, wvt stops the device. Click **Start** again. If the center
frequency in the header changes, wvt retunes at the first sample of that message.

## The examples

The repository has these examples in `config/`:

| File | Source |
|---|---|
| `zmq_gnuradio_sub_cf32.ini` | A GNU Radio *ZMQ PUB Sink* with the type complex and *Pass Tags* off. The rate and the frequency come from `[stream]`: set them to the values of the flowgraph |
| `zmq_raw_pull_ci16.ini` | Raw `ci16` samples from a ZMQ PUSH socket. The rate and the frequency come from `[stream]` |
| `zmq_example_header.ini` | A template for a message with a binary header. Copy it and change it for your source |
| `zmq_wvt_ci16.ini` | The built-in format as a file, for `ci16` data messages only. It is a template, and the self-checks use it |

A GNU Radio *ZMQ PUB Sink* with the address `tcp://*:5555` binds on port 5555. wvt connects to it on
`127.0.0.1:5555`, or on the address of the computer that runs GNU Radio. With tags on, the sink adds
a header that these files do not read. Set *Pass Tags* to off.

## Design

For the design, refer to [the design record](design/zmq-formats-plan.md).
