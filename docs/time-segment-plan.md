# Plan: open a time segment of a file (issue #16)

Branch: `16-time-segment`, from `main` at `29dca41`.
Status: **steps 1–5 implemented** (2026-10-19), not committed yet. The GUI is not tested on
screen; XQuartz was not running in this session. See §7.

## 1. Goal

Issue #16 asks for these items:

1. Two text boxes in the main UI: a start time and an end time, in seconds.
2. When a file opens, wvt processes only that segment of the file.
3. The user can change the values after the file is open. A change opens the file again
   at the new times.
4. `-1` and `-1` mean the whole file. Any other value marks a specific time.

## 2. Starting point (`main`)

### What is slow today

Measured on a 7.5 GB file (ci16, 20 MS/s, 100 s), on the 10-core development Mac with
16 GB of memory. The tool was `open_bench`, run on the jobs that `App` starts after a file
opens:

| Job | Whole file | 40–50 s segment | Whole file, second time |
|---|---|---|---|
| `IqFile::open` | 0.6 ms | — | — |
| Waterfall | **3394 ms**, FFT sees **13 %** | 427 ms, FFT sees **100 %** | 255 ms, 13 % |
| Demod (sparse) | 37 ms | 21 ms | 23 ms |
| Overview strip | 46 ms | — | — |

Conclusions:

- Opening the file costs nothing. `IqFile` memory-maps the file.
- The time goes into the first waterfall. It spreads 256 M samples
  (`kMaxInputSamples`) over the whole file, so it reads pages from the full 7.5 GB.
  The second run is fast because the pages are then in memory. The file in the test was
  just written, so part of it was already in memory. On a cold disk, a network drive, or a
  file larger than memory, the first view is slower than this.
- On the whole file, the waterfall sees only 13 % of the samples, so short signals can be
  missing. On a 10 s segment, the same work sees 100 %.
- Each pan or zoom over a wide view repeats this disk read.

A segment fixes the slow first view. It also makes the full-coverage waterfall possible
for the part of the file that the user wants.

### What exists

- `App::openFile` (`src/app.cpp`) gets the metadata, opens the `IqFile`, and calls
  `onSourceChanged(true)`. That function sets `nSamples_` and `dur_` and resets the view
  to `0 … dur_`. The first frame then sends the waterfall, demod and overview requests
  (`submitRequests`).
- The workers already take absolute sample ranges (`s0`, `s1`). The overview always reads
  the whole file (`OverviewReq` has no range).
- About 20 places in `src/app.cpp` and 2 in `src/app_audio.cpp` use `0` and `dur_` /
  `nSamples_` as the limits of the file:
  - `setView` clamping;
  - `|<`, `>|`, `All` and the span presets;
  - the Home, End and A keys;
  - the scrollbar and the overview axis;
  - the plot axis constraints;
  - the toolbar info text;
  - the default time markers;
  - the audio "Whole file" range and the playhead slider.
- The command line already has `--view T0:T1` (initial view). In headless audio export,
  `--view` also sets the export range (`runAudioExport`, `src/main.cpp`).
- Row 2 of the toolbar already has a **Start** box. It moves the view, not the segment.
  The new boxes need different names.

## 3. Design decisions

| Topic | Decision | Reason |
|---|---|---|
| Time axis | **Absolute file time.** The plots, readouts, bookmarks, markers and audio playhead show seconds from the start of the file. A segment from 40 s to 50 s shows 40–50 s, not 0–10 s. | The values the user types match what the plots show. The sample numbers match the file and its SigMF annotations. The workers already use absolute sample numbers, so they do not change. |
| How the segment limits work | `App` keeps `segS0_` and `segS1_` (samples) and two helpers, `tBegin()` and `tEnd()` (seconds). Every place that now uses `0` and `dur_` / `nSamples_` as the file limits uses the helpers instead. `OverviewReq` gets an `s0` / `s1` range. | One source of truth. Each limit in §2 becomes a mechanical change that is easy to check. |
| Rejected: a "virtual file" in `FormatParams` (sample offset + length) | Not used. | It would be fewer changes, but every time on screen would be relative to the segment start. To show file time, a time offset would then be needed in all the same places. |
| Values stored | The two values, as typed in seconds (`segT0_`, `segT1_`). The sample range comes from them in `onSourceChanged`, each time the rate, format or header changes. | A changed sample rate or format moves the samples, but the user means seconds. |
| `-1` | Start `-1` = file start. End `-1` = file end. Each one works on its own: `-1` to `20` = the first 20 s, and `80` to `-1` = from 80 s to the end. Any negative value means the same as `-1`. | The issue asks for `-1` / `-1` = whole file. Treating each side on its own is the natural extension. |
| Bad values | End at or before start, or start at or after the end of the file: the status bar and `LOG_ERROR` give the reason, and the segment stays as it was. An end past the end of the file is clamped to the end, with a `LOG_WARN`. | The user never gets an empty view, and always sees why a value was not used. |
| When it applies | The boxes are always in the toolbar, also before a file is open. `openFile` uses the current values. Enter in a box (or focus leaving it) applies the change to the open file. | The issue says "prior to opening" and "changeable after opening". |
| "Opens the file again" | The file is **not** opened again from disk. A segment change keeps the mapped file, the format settings and the bookmarks. It resets the view to the full segment and clears the old results, as `onSourceChanged(true)` does, and the workers compute again. | A real reopen would cost nothing but would read the metadata again and reset the format and rate. The user sees the same result. |
| New file | The segment values stay when a new file opens. If they do not fit the new file, the "bad values" rule applies, and the segment falls back to the whole file. | Matches "enter the times, then open". |
| Overview strip | Shows the segment only. | The issue says "only process that segment". The scrollbar and the overview stay in step. |
| Audio | "Whole file" becomes "Whole segment". The playhead slider and the play ranges stay inside the segment. | Audio follows the same limits. |
| Siggen playback "from viewer" | No change: it plays the whole file. | Not part of the issue. It can come later. |
| Command line | New option `--segment T0:T1`, with `-1` allowed on each side (`--segment 40:-1`). `--view` is then absolute time inside the segment. Headless audio export (`--audio-out`) also uses the segment. | The same feature from the CLI, and a way to test it without a display. |
| Logging (#14) | INFO `segment 40 s to 50 s (200 MS of 2 GS)` or `whole file`. ERROR or WARN for the bad-value cases above. | Follows `docs/verbose-logging-plan.md`. |
| UI labels | Row 1, after **File**: **Segment** `[from] s` `[to] s`, with the tooltip "Load only this part of the file, in seconds. -1 = file start / end. Press Enter to apply." The toolbar info text shows `segment 10 s of 100 s` when a segment is active. | Row 2 already has a **Start** box for the view. |

## 4. Implementation steps

1. **Segment state and limits** (`src/app.h`, `src/app.cpp`).
   - Add `segT0_ = -1`, `segT1_ = -1`, `segS0_` and `segS1_`, and the `tBegin()` and
     `tEnd()` helpers.
   - Add `applySegment(std::string& err)`. It turns the values into samples with the
     rules in §3. `onSourceChanged` calls it.
   - Change each limit listed in §2 to use the helpers.
   - `OverviewReq` gets `s0` / `s1`, and `computeOverview` (`src/engine.cpp`) works on
     that range.
2. **Toolbar boxes.**
   - Add two `InputSI` boxes in row 1, with the tooltip from §3.
   - Enter calls `setSegment(t0, t1)`: check the values, apply them, reset the view, log
     the result.
   - Add the segment text to the info line.
3. **Audio** (`src/app_audio.cpp`). Rename "Whole file" to "Whole segment", and keep
   `playRange` and the playhead slider inside the segment.
4. **Command line** (`src/main.cpp`).
   - Add `--segment T0:T1` and its usage text.
   - Add a public `App::setSegment`. `openFile` applies the segment before the first
     view.
   - Headless audio export clamps its range to the segment.
5. **README.** Add the segment boxes and `--segment` to the user guide and the CLI table.

## 5. Testing

| Test | Check |
|---|---|
| Segment math (new `tools/segment_check.cpp`, or add to an existing check) | `applySegment` with `-1`/`-1`, `-1`/`20`, `80`/`-1`, `40`/`50`, end before start, start past the end of the file, end past the end of the file. Also after a sample-rate change and after a header-bytes change. |
| Overview | `computeOverview` with a range reads only inside it. Its time values are absolute. |
| Speed | `open_bench`-style timing on a large file: with a 10 s segment, the first waterfall reads only that span. |
| Headless (no display) | `wvt FILE --audio nfm --audio-out x.wav --segment 40:50` writes only those 10 s, and the log has the INFO segment line. |
| GUI (**you must do this**; this session has no X display) | Type the values before and after open. The view, scrollbar, overview, the `\|<` `>\|` `All` buttons and the keys stay inside the segment. The axes show file time. Bookmarks and markers. Audio "Whole segment". Bad values give a status message and keep the old segment. |
| Regression | With `-1` / `-1` (the default), behavior is the same as `main`. `siggen_check`, `audio_check` and `log_check` pass. |

## 6. Decisions (2026-10-19)

The user said to go ahead with the plan, so the recommendations apply:

1. **Time axis:** absolute file time.
2. **Overview strip:** the segment only.
3. **Next file:** the segment stays.
4. **"Opens the file again":** the file stays mapped. The view is reset, and the plots are
   computed again. The metadata is not read again.
5. **Siggen playback from the viewer:** the whole file, as before.

## 7. Implementation notes

### Files

| File | Change |
|---|---|
| `src/engine.h`, `src/engine.cpp` | `SegmentRange segmentRange(t0, t1, fs, n)`: the rules in §3. `OverviewReq` has `s0` / `s1`. |
| `src/app.h`, `src/app.cpp` | Segment state, `setSegment`, `applySegment`, `resetTimeView`, `tBegin()` / `tEnd()`. The Segment boxes. All the limits in §2. |
| `src/app_audio.cpp` | Play ranges and the playhead inside the segment. "Whole segment" in the Range list. |
| `src/main.cpp` | `--segment T0:T1`. The headless audio export uses the segment. |
| `tools/segment_check.cpp` | Self-test: 13 rule checks and 6 overview checks. |
| `README.md` | "Large files: load a segment", the option, the layout table, the audio ranges. |

### Differences from the plan

- `onSourceChanged` returns the segment note. `openFile` adds the note to its "Opened"
  status message.
- A format, rate or header change maps the segment again. An end past the end of the file
  gives the WARN only when the sample range changes, not on each change.
- After a format, rate or header change, the view is clamped into the segment
  (`setView`). Before, the view was not clamped.
- `--segment` checks "end before start" at parse time and exits with status 2.
- `segment_check` links `ui_util.cpp` and `imgui_implot`, because `segmentRange` uses
  `fmtSI` for its messages.

### Verification (macOS)

- The build has no warnings. `segment_check`, `siggen_check`, `audio_check` and
  `log_check` pass.
- Headless export of the 7.5 GB test file (ci16, 20 MS/s, 100 s):

  | Arguments | Result |
  |---|---|
  | `--segment 40:50` | 10.0 s WAV, INFO "segment 40 s to 50 s" |
  | `--segment 40:50 --view 45:46` | 1.0 s WAV |
  | `--segment 90:200` | WARN (end clamped), 10.0 s WAV |
  | `--segment 150:-1` | ERROR "start is past the end of the file", status 2 |
  | `--segment 50:40` | ERROR, status 2 |

- Compared with a `main` build on a 3 s file: the WAV export is byte-for-byte the same with
  no `--segment`, and with `--segment -1:-1`, with and without `--view`. `--help` differs
  only by the new option.

### Not verified

- The GUI. To test it, start XQuartz, then run
  `build/wvt big.ci16 -f ci16 -r 20M --segment 40:50 -v debug`. Check the GUI rows of §5:
  - the boxes before and after open;
  - Enter;
  - a bad value;
  - the scrollbar, the overview, `|<` `>|` `All`, and Home / End / A;
  - the axes in file time;
  - the audio "Whole segment" range and the playhead.
