# wvt config folder

This folder holds the user configuration files of wvt (issue #101):

- `wvt.conf`: the configuration file. Copy `wvt.conf.example` to `wvt.conf` and change it, or run
  `wvt --write-config`. See [docs/config.md](../docs/config.md).
- `zmq_*.ini`: ZMQ message formats for the *ZMQ stream* device, one format per file. The file
  format is described at the top of `src/net/zmq_format.h` and in
  [docs/design/zmq-formats-plan.md](../docs/design/zmq-formats-plan.md). The examples:
  - `zmq_gnuradio_sub_cf32.ini`: GNU Radio *ZMQ PUB Sink*, complex float, tags off.
  - `zmq_raw_pull_ci16.ini`: raw `ci16` samples from a PUSH socket.
  - `zmq_example_header.ini`: a template for a format with a binary header.
  - `zmq_wvt_ci16.ini`: the built-in wvt format, written as a file.

  Choose one in the **Format** list next to the *ZMQ stream* device's **Address**, with
  `--zmq-format NAME`, or with `zmq.format = NAME` in `wvt.conf`. `wvt --list-zmq-formats` lists
  them with any errors.

wvt uses the first of these folders that exists, and only that one:

1. `--config-dir DIR`
2. `config/` next to the program
3. `config/` in the folder above the program (this folder, when wvt runs from `build/`)
4. `~/.config/wvt/`

`wvt --show-config` shows which folder and which file wvt uses.

Git tracks only the examples and this file. Your own files here (`wvt.conf` and others) are
ignored, so they are not committed by mistake.
