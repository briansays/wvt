# Audio

The audio window demodulates one channel (AM, NFM, WFM, USB, LSB or CW) and plays it through the
speakers. It plays a recording or the live stream, and it writes WAV files.

## Open the audio window

Do one of these steps:

- Select **Tools → Audio**.
- Press **Ctrl+D**.
- Start wvt with `--audio MODE`.

The window floats over the main window. It works in File mode and in Device mode.

## Select the channel

With **Follow viewer channel** on (the default), the audio uses the channel of the viewer. This
channel shows as the cyan lines on the spectrum and the waterfall.

- To tune, Ctrl+click the spectrum or the waterfall, or drag the lines.
- Live mode also shows these lines.

With **Follow viewer channel** off, the audio channel is the orange line. Set its offset and its
bandwidth in the audio window.

When you select a mode, wvt sets the bandwidth to the default of that mode:

| Mode | Default width | Notes |
|---|---|---|
| AM | 10 kHz | The envelope. *AGC* divides by the carrier level |
| NFM | 12.5 kHz | Audio up to 5 kHz |
| WFM | 200 kHz | Mono, audio up to 15 kHz, 75 µs de-emphasis. Select 50 µs for Europe |
| USB / LSB | 2.8 kHz | The passband is the shaded channel. The carrier is at its lower edge (USB) or upper edge (LSB) |
| CW | 500 Hz | A carrier at the channel center plays at the *BFO pitch* (200 Hz to 1500 Hz) |

## Levels

- **Gain** adds gain after the demodulator.
- **Squelch** mutes the audio while the channel power is below the threshold. The window shows the
  channel power. Set the threshold just above the noise.
- **Volume** and **Mute** change only the speaker level. They do not change the WAV files.
- **Device** selects the sound output. The change applies at the next **Play**.

## File mode

**Play** plays the **Range**. Select one of these ranges:

- *From view start* to the end of the file.
- *View*.
- The part between the *time markers*.
- *Whole file*.

With a segment loaded, each range stays in the segment, and *whole file* becomes *whole segment*.

- A green playhead line moves across the time plots and the waterfall. To seek, drag the line or
  the position slider.
- To play from a point, **Shift+click** a time plot or the waterfall.
- *Loop* repeats the range.
- *Follow playhead* moves the view forward one page when the playhead leaves it. This applies only
  to views of 0.25 s or more.

## Device mode

**Play** plays the live stream, with a delay of approximately 0.15 s.

- The sound card and the SDR use different clocks. wvt measures the buffer level and adjusts the
  resampling ratio. The window shows the correction in ppm. Thus the delay stays constant.
- If the demodulator falls behind, it skips to the newest samples and counts a *resync*.
- After a retune or a stream restart, the audio continues automatically.

## WAV files

The WAV files are 48 kHz, 16-bit mono. The name is the *File prefix* plus the date, the time, the
frequency and the mode, for example `~/wvt-recordings/audio_20261018_143000_100.3MHz_WFM.wav`.

- File mode: **Save WAV** demodulates the range faster than real time.
- Device mode: **Record audio** records the demodulated stream. It also works without a sound
  device.

## Command line

`--chan OFFSET:BW` sets the channel. Without it, the channel is at 0 Hz with the default width of
the mode.

| Option | Function |
|---|---|
| `--audio am\|nfm\|wfm\|usb\|lsb\|cw` | Open the audio window and play |
| `--audio-out FILE.wav` | Demodulate to a WAV file, then stop. The range is the file, the `--segment` or the `--view` range. With `--device`, the range is the live stream until Ctrl+C or `--record-duration` |
| `--audio-device NAME` | The sound output. The default is the system default |
| `--list-audio-devices` | Show the sound outputs, then stop |
| `--volume DB` | The playback volume (default −6 dB) |
| `--squelch DBFS` | Mute while the channel power is below this level |

```sh
wvt capture.sigmf-meta --chan 300k:200k --audio wfm                       # play a broadcast FM channel
wvt capture.sigmf-meta --chan -250k:10k --audio am --view 0.5:1.5 --audio-out am.wav   # export 1 s
wvt --device rtlsdr -c 100.4M -r 2.4M --audio wfm --chan -300k:200k       # listen live to 100.1 MHz
wvt --device sim -c 100M -r 2.4M --audio wfm --chan 360k:200k --audio-out fm.wav --record-duration 10
```

## How it works

The demodulator does these steps:

1. It shifts the channel to 0 Hz.
2. It decimates with half-band filters to a minimum of two times the channel width.
3. It filters the channel with a windowed-sinc FIR filter.
4. It demodulates.
5. A polyphase resampler sets the audio bandwidth and the 48 kHz output rate.

All stages keep their state between blocks. Thus the audio is continuous. The self-check
`build/audio_check` tests each mode without hardware.

For the design, refer to [the audio design record](design/audio-demod-plan.md).
