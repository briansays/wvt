# Logging

wvt writes a log file for each run. As an option, it also shows the log on the console. Use the log
to find the cause of a problem, or attach it to an issue report.

## The log file

Each run writes `wvt_log_<YYYYMMDD>_<HHMMSS>.log` (local time). wvt puts the file in the first of
these folders that is set:

1. The `--log-dir DIR` option.
2. The `WVT_LOG_DIR` environment variable.
3. The `TMPDIR` environment variable.
4. `/tmp`.

The file gets all messages, at all levels, also when you do not use `-v`. To write no file, use
`--no-log-file`.

The start of the file has:

- The start time and the process ID.
- The build: the git commit and version tag (see [Version information](build.md#version-information)), and if RTL-SDR, UHD and audio are on or off.
- The operating system.
- The full command line.

Each line has the date and time (to the millisecond), the level, the component and the message:

```text
# wvt log, started 2026-10-20 19:51:53.739, pid 91526
# build: git 0e5ae9d, RTL-SDR on, UHD on, audio on
# os: Darwin 25.6.0 x86_64
# command: ./build/wvt --list-devices
2026-10-20 19:51:53.739 [INFO ] [log] log file /tmp/wvt_log_20261020_195153.log
```

## The console

Without `-v`, the console shows only warnings and errors.

| Option | Function |
|---|---|
| `-v`, `--verbose` | Show messages at INFO level and above on the console, with a time stamp |
| `-v LEVEL`, `--verbose=LEVEL` | Show messages at `LEVEL` and above. `LEVEL` is `debug`, `info`, `warn` or `error` |
| `--log-dir DIR` | The folder for the log file |
| `--no-log-file` | Do not write a log file |

```sh
wvt big.sigmf-meta -v debug        # all messages on the console
wvt --device rtlsdr -v --log-dir ~/logs
```

## Levels

| Level | Use |
|---|---|
| DEBUG | Details for a developer, for example the time of each job |
| INFO | The actions of wvt, for example a file opens, a device starts, or a recording stops |
| WARN | A problem that wvt can continue after, for example a GPU fallback or dropped samples |
| ERROR | An action that failed |

## Components

The component tells you which part of wvt wrote the message:

| Component | Part |
|---|---|
| `main` | The command line and the programs that run without a window |
| `app` | The window and its actions |
| `file` | Opening a file |
| `gpu` | The GPU backends and the fallback |
| `live`, `device`, `rtlsdr`, `uhd` | Live streaming and the devices |
| `record` | Recording |
| `siggen`, `tx` | The signal generator and the transmitter |
| `audio` | The audio demodulator and the sound output |
| `extract` | The region export |
| `log` | The logger |

## How it works

The logger is safe to use from all threads. It does not write from the real-time audio and SDR
callbacks, because a line write takes a lock and can write to the disk. The self-check
`build/log_check` tests the levels, the options, the log file and the threads.

For the design, refer to [the logging design record](design/verbose-logging-plan.md).
