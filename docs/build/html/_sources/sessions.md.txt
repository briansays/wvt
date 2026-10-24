# File sessions

A file session keeps the state of the file viewer in a small file, so that you can go back to
the same work later. When you open the session, wvt opens the recording and shows it as it was.

A session file keeps these items:

| Item | Contents |
|---|---|
| The recording | Its path, the sample format, the header, I/Q swap, the sample rate, the center frequency and the segment |
| The view | The time span, the frequency span and relative frequency |
| The waterfall | The FFT size, the window, the overlap, the averages, the size limits, the dB range, Auto, the colormap and peak hold |
| The layout | The panel sizes, the overview strip, and which time plots show and fit their Y axis |
| The channel | On or off, the offset, the bandwidth, the taps, the decimation, and the AM, FM and PM settings and the squelch |
| Markers and bookmarks | The time and frequency markers, the point markers (P1 and P2), and all the bookmarks |

A session does not keep the live mode, the signal generator, the audio window or the export
window.

## Save a session

1. Open a recording, and set the view and the plots as you want them.
2. Select **File → Save File Session...**.
3. Make sure that the path is correct. The default is the name of the recording with
   `.wvtsession.json`, in the folder of the recording.
4. Click **Save**.

wvt does not replace an existing file. To replace it, tick **Replace an existing file**.

From the command line, `--session-out F` opens the recording with the other options, writes
the session to `F`, then stops:

```sh
wvt cap.sigmf-meta --view 10:10.5 --chan 300k:150k --session-out cap.wvtsession.json
```

## Open a session

Type the path of the session file in the **File** box and press Enter, or give it on the command
line:

```sh
wvt cap.wvtsession.json
```

wvt looks at the contents of the file, not only its name: a session file is a small JSON file
with a `wvt_session` member. Thus a recording (for example a `.bin` file) or SigMF metadata is
never opened as a session.

The session keeps the path of the recording in two forms: the full path, and the path from the
folder of the session file. wvt tries the second form first. Thus, if you move a folder that has
the recording and its session, the session still finds the recording.

## The file format

The session file is JSON. You can read and change it with a text editor. Part of a session file
(`...` shows where members are left out here):

```text
{
  "wvt_session": 1,
  "recording": {"path": "/data/cap.sigmf-data", "relative_path": "cap.sigmf-data", "datatype": "ci16_le",
                "header_bytes": 0, "swap_iq": false, "sample_rate": 2400000, "center_freq": 100000000,
                "segment_s": [-1, -1]},
  "view": {"t0_s": 10, "t1_s": 10.5, "f0_hz": 98800000, "f1_hz": 101200000, "relative_freq": false},
  "waterfall": {"nfft": 1024, "window": "Blackman-Harris", "overlap_pct": 50, ...},
  "layout": {...},
  "channel": {"enabled": true, "offset_hz": 300000, "bandwidth_hz": 150000, ...},
  "markers": {...},
  "points": {"shown": true, "p1": {"plot": "am", "t_s": 10.12}, "p2": {"plot": "waterfall", "t_s": 10.15, "f_hz": 100300000}},
  "bookmarks": [{"name": "burst", "t0_s": 10.1, "t1_s": 10.2}]
}
```

- `segment_s` -1 is the start or the end of the file.
- `plot` of a point marker is `waterfall`, `spectrum`, `i`, `q`, `am`, `fm` or `pm`. A marker on the
  waterfall has `t_s` and `f_hz`, on the spectrum `f_hz`, and on a time plot `t_s`.
- A member that is not in the file gets its default value. Only `recording` is necessary.
- A session file from a newer wvt (a larger `wvt_session` number) is refused.
