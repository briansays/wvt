# wvt config folder

This folder holds the user configuration files of wvt (issue #101):

- `wvt.conf`: the configuration file. Copy `wvt.conf.example` to `wvt.conf` and change it, or run
  `wvt --write-config`. See [docs/config.md](../docs/config.md).
- `zmq_*.ini`: ZMQ message formats for the *ZMQ stream* device (planned, see
  [docs/design/zmq-formats-plan.md](../docs/design/zmq-formats-plan.md)).

wvt uses the first of these folders that exists, and only that one:

1. `--config-dir DIR`
2. `config/` next to the program
3. `config/` in the folder above the program (this folder, when wvt runs from `build/`)
4. `~/.config/wvt/`

`wvt --show-config` shows which folder and which file wvt uses.

Git tracks only the examples and this file. Your own files here (`wvt.conf` and others) are
ignored, so they are not committed by mistake.
