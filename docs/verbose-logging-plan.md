# Plan: Verbose console logging and a timestamped log file

Issue: #14 — *Usability: add verbose console logging with an output timestamped logfile*
Branch: `14-verbose-logging` (from `main` at `5688d28`)
Status: **steps 1–4 committed.** Step 5 (README) skipped at the user's request (2026-10-18). Step 6
(GUI testing) remains. The open questions are decided (section 7, 2026-10-18).

## 1. Goal

From the issue:

- A runtime flag `-v` / `--verbose` prints log messages about application actions to the console.
- The flag takes an optional level (`DEBUG`, `INFO`, `WARNING`, `ERROR`) that sets the minimum
  level printed.
- Every run **always** writes a log file, `wvt_log_<date>_<time>.log`, whether or not `-v` is
  given. The issue names `/tmp`; the file goes to `$TMPDIR` (fallback `/tmp`) so that it works
  the same on macOS and Linux (section 7).

Checklist from the issue, mapped to sections below:

| Issue item | Section |
|---|---|
| Add log statements to actions, with levels | 5 |
| Print logs to console | 3, 4 |
| Save logs to file | 3, 4 |

Out of scope: an in-GUI log viewer window, log rotation / cleanup of old files (the OS clears the
temp folder), syslog / journald, and a TRACE level for per-block signal-processing output.

## 2. Starting point

| Area | What exists | Gap |
|---|---|---|
| Console output | ~95 `fprintf(stderr, "wvt: ...")` calls, **all in `src/main.cpp`** (arg errors, headless record / audio / siggen progress, device errors) | No levels, nothing goes to a file |
| Library code | `engine`, `live`, `recorder`, `sdr_*`, `siggen_out`, `audio_*` print nothing; they return errors via `std::string& err` or `stats().error` / `status()` fields that the GUI shows | Actions (device start, tune, record start/stop, gaps, TX, audio start) leave no trace |
| Threads | `live`, `recorder`, `sdr_rtlsdr`, `sdr_uhd`, `sdr_sim`, `siggen_out`, `audio_engine`, `dsp` all run worker threads | Logger must be thread-safe |
| Real-time path | miniaudio callback in `audio_out.cpp`, SDR RX callbacks | Must not block on a mutex or do file I/O there |
| Timestamps | `recorder.cpp` / `wav_writer.cpp` use `strftime("%Y%m%d_%H%M%S")` on `gmtime_r` | Reuse the same stamp format for the log name |
| CLI | Hand-rolled loop in `main()` with a `next()` lambda; `-v` is unused | Need an *optional* value, which the loop does not support yet |

## 3. Design decisions

| Topic | Decision | Reason |
|---|---|---|
| Library | Small in-house `src/log.h` / `src/log.cpp` (~150 lines), no new dependency | Matches the vendored, no-network build; spdlog would be overkill |
| Levels | `DEBUG < INFO < WARN < ERROR` (accept `WARNING` as an alias, case-insensitive) | The "standard message types" in the issue |
| Console default | Without `-v`: WARN and ERROR still print (keeps today's error messages). `-v` alone = INFO. `-v LEVEL` = that level | No change in behavior for users who don't pass `-v` (decided, section 7) |
| File level | The file always gets **DEBUG and up**, independent of `-v` | Issue: "always generate a log file of those verbose actions". The file is for after-the-fact debugging |
| Optional value parsing | `--verbose=LEVEL`, `-v LEVEL`, `-vLEVEL`. With a space, the next token is taken as the level **only if** it is a known level name; otherwise it is left for the file / next option | `wvt -v rec.sigmf-meta` must still open the file |
| File location | First match of: `--log-dir DIR`, `$WVT_LOG_DIR`, `$TMPDIR`, `/tmp`. `--no-log-file` turns it off | `$TMPDIR` is the per-user temp folder on macOS (`/var/folders/...`) and is usually unset on Linux, so the fallback gives `/tmp` there. The overrides help read-only or shared machines (decided, section 7) |
| File name | `wvt_log_<YYYYMMDD>_<HHMMSS>.log`, local time. If the name exists (two runs in one second) append `_<pid>` | Local time matches what the user sees on the clock; `strftime` format matches the recorder |
| Line format | File: `2026-10-18 14:08:37.123 [INFO ] [live] streaming from rtlsdr:0, 2.4 MS/s at 915 MHz`. Console with `-v`: same without the date. Console without `-v`: today's `wvt: <msg>` (WARN: `wvt: warning: <msg>`) | Sortable, greppable; the `[component]` tag shows where it came from. The plain console format keeps the output of users who don't pass `-v` unchanged |
| Thread safety | One mutex around the write of each formatted line; format outside the lock | Simple. Log volume is low (actions, not per-sample) |
| Who logs an error | An error that returns through `err` or a status field is logged at ERROR **once**, by the code that shows it to the user (`main.cpp`, `app*.cpp`). Library code logs the same failure at DEBUG for context. Library WARN only for problems nothing else reports (dropped samples, clamped gain, metadata rewrite failures, TX notes) | Without this rule one failure printed two or three times on the default console |
| Real-time paths | No logging inside the audio callback or SDR sample callbacks. Count events there (underruns, gaps) with atomics and log a summary from the worker / UI thread | Keeps the audio and RX paths lock-free |
| Header line | First lines of the file: wvt version / git hash, build options (RTL-SDR / UHD / audio on or off), full command line, OS | Makes a log file useful on its own in a bug report |
| Failure to open the file | Print one WARN to stderr and continue with console only | Logging must never stop the app |
| Flush | Flush after WARN/ERROR lines and at exit; also flush on SIGINT/SIGTERM in headless mode | Lines are not lost on Ctrl+C or a crash after an error |
| API shape | Free functions `logInit(cfg)`, `logWrite(LogLevel, "component", fmt, ...)` printf-style, `logParseArgs()`, plus macros `LOG_DEBUG("live", ...)` etc. that skip formatting when no sink takes the level | The code base uses no namespaces (`fmtSI`, `parseSI`), and a `log` namespace clashes with `::log` from `<cmath>`. printf style matches the existing `fprintf` code; the macro check keeps DEBUG cheap |

## 4. Implementation steps

1. **Logger core** — *done.* `src/log.h`, `src/log.cpp`; add to `wvt`, `siggen_check` and `audio_check`
   in `CMakeLists.txt` (the tools link library sources that will now call the logger; the tools
   do not call `init`, so they get console WARN+ and no file).
2. **CLI** in `src/main.cpp` — *done* (`logParseArgs()` strips the log options from `argv`; the
   git revision for the header comes from `git describe` at configure time):
   - Parse `-v` / `--verbose[=LEVEL]`, `--log-dir`, `--no-log-file` as the first pass over
     `argv`, then call `log::init()` **before** the main parse loop, so arg errors are logged too.
   - Add the options to `usage()` and print the log file path at INFO on start.
3. **Convert `main.cpp` output** — *done.* Replace `fprintf(stderr, "wvt: ...")` error / status calls
   with `LOG_ERROR` / `LOG_WARN` / `LOG_INFO`. Keep as plain `printf`/`fprintf`:
   `usage()`, `--list-devices`, `--list-audio-devices`, and the `\r` progress line of headless
   recording (it is a live display, not a log). Log a progress summary to the file at DEBUG
   instead, once every few seconds.
   - As built: messages that printed without `-v` before ("streaming from", "recording to",
     "wrote", "metadata", "TRANSMITTING", "screenshot written") use `logStatus()`: always on the
     console, INFO in the file. Default console output is byte-for-byte the same as before.
   - The `\r` lines use `logProgress()` / `logProgressEnd()`: always on the console, DEBUG in the
     file at most every 10 s. A log line that arrives while a progress line is open starts on a
     new line, so step 4 logs from worker threads don't garble it.
   - `wvt: note: ...` messages became WARN (`wvt: warning: ...`). Arg errors are ERROR.
   - New INFO lines for the GUI path: window size / renderer, device start, file open, exit.
4. **Add action logs** to the library code (section 5) — *done.*
   - `sdr_device.cpp` logs every open centrally (requested vs actual rate / fc, notes), so each
     backend only adds DEBUG detail (`uhd`: open, tune result, packet size, gain clamp; `rtlsdr`:
     USB strings, tuner, gain steps, transfer size).
   - `live`: start/stop summary, retune, gain, spectrum settings (DEBUG), dropped-sample summary
     at most every 5 s from the spectrum thread, and the device stopping on its own.
   - `recorder`: start, end summary (reason, bytes, gaps, lost samples), retune segments and the
     first 50 gaps (DEBUG), writer lag high-water mark (DEBUG).
   - `siggen_out`: file generation start/end; TX start, retune, gain, stop summary (underflows,
     lost packets).
   - `audio_engine`: start (mode, channel, output), stop summary (underruns, resyncs), WAV
     start/close, WAV export. Nothing in the miniaudio callback; it only counts underruns.
   - `engine`: `Worker` logs jobs that take 250 ms or more (DEBUG). Logging every recompute
     would add many lines a second while panning (section 7: no DSP detail for now).
   - `iq_file`: metadata guess result and file open (DEBUG).
   - `app*.cpp`: ERROR at each place an error reaches the status bar, plus a recording that
     fails on its own (edge-detected in `pollLive()`); INFO for opened files.
   - Found while doing this: `openDevice()` discarded the warning from `checkSampleRate()`
     (e.g. UHD above 8 MS/s needs USB 3). It is now in the log at DEBUG; the GUI still doesn't
     show it for UHD (RTL-SDR folds it into `notes()`).
5. **Docs** — *skipped (2026-10-18).* README: a short *Logging* section (flag, levels, file location, what to attach to
   a bug report).
6. **Test** (section 6).

## 5. What to log

| Component | INFO | DEBUG | WARN / ERROR |
|---|---|---|---|
| `main` / `app` | start-up, log path, file opened (path, format, rate, fc, size), window size / renderer (GLX or software), exit | parsed CLI overrides, SigMF / filename auto-detection results | bad args, file open failure, GLX fallback |
| `engine` / `dsp` | — | FFT size, decimation, view changes, recompute timings (once per recompute, not per block) | cancelled / failed compute |
| `live` + `sdr_*` | device open (type, serial), stream start/stop, retune, gain / antenna / bandwidth changes | actual vs requested rate & fc, buffer sizes | device errors, overflow / dropped samples (summarised) |
| `recorder` | record start (paths, format), stop (duration, bytes, gaps) | ring fill high-water mark | write errors, gaps |
| `siggen` / `siggen_out` | waveform mode + key params, file gen start/finish, loopback start/stop, TX start/stop, tx gain | per-pulse schedule summary | TX errors, underruns |
| `audio_*` | mode, channel, output device, start/stop, WAV export path | resampler ratio / PI loop corrections | device open failure, underrun count (summarised) |
| UI | — | window open/close (siggen, audio), screenshot path | — |

Where the code already builds an `err` string or sets `stats().error`, log it at the point it
is set, so the GUI and the log show the same text.

## 6. Testing

- **Unit-ish** (add to `tools/`, or a small `log_check`): level parsing incl. `WARNING`
  alias and case; filtering per level; file gets DEBUG while console is at WARN; name
  collision gets `_<pid>`; concurrent logging from 8 threads gives no interleaved lines.
- **CLI**: `wvt -v file`, `wvt -v debug file`, `wvt --verbose=error file`, `wvt -vDEBUG`,
  `wvt -v` with a file literally named `debug` (documented edge case: use `./debug`).
- **Headless**: `--device sim --record x --no-gui --record-duration 3` → log has start, stop,
  byte count; Ctrl+C mid-run still leaves a complete file.
- **No regressions**: run `siggen_check` and `audio_check`; output without `-v` matches today.
- **Log location**: `$TMPDIR` set (macOS) → file there; `$TMPDIR` unset (typical Linux) → `/tmp`;
  `$WVT_LOG_DIR` and `--log-dir` override in that order; directory not writable → one warning,
  app still runs.
- **Platforms**: macOS (XQuartz) and Linux.

## 7. Decisions (2026-10-18)

| Question | Decision |
|---|---|
| Console level without `-v` | Stay at **WARN and up**, as today. `-v` / `-v LEVEL` only lowers it |
| Log directory | Use **`$TMPDIR`**, fall back to `/tmp` when it is unset, for cross-platform behavior. `--log-dir` and `$WVT_LOG_DIR` still override |
| Old log files | **No cleanup** in wvt. The OS clears the temp folder |
| Signal-processing detail (TRACE) | **Left out for now.** DSP logs stay at one DEBUG line per recompute; no per-block output |
