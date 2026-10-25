# Signal generator

The signal generator makes CW and pulsed waveforms: chirps, Barker codes, staggered or jittered
PRIs, bursts, sweeps and impairments. It also plays back recordings as canned waveforms, one at a
time or as a chain. The generator writes its waveforms to files, shows them in the live view
(loopback), or transmits them with a USRP.

## Open the generator

Do one of these steps:

- Select **Tools → Signal generator**.
- Press **Ctrl+G**.
- Start wvt with `--siggen` or `--gen MODE`.

The window floats over the main window. It works in File mode and in Device mode.

## Modes and settings

Select the mode at the top of the window: **CW**, **Pulsed**, **Playback** or **Chain**.

| Setting | Function |
|---|---|
| Carrier | The sample rate, the center frequency, the offset from the center, the peak amplitude (dBFS) and the start phase |
| CW | A steady tone, with an optional drift (Hz/s). When you tune to a new frequency, the phase stays continuous |
| Sweeps | CW and pulsed. Refer to [Sweeps](#sweeps) |
| Pulsed | The PRI or the PRF, and the pulse width or the duty cycle. The width includes the edges |
| Edges | Rectangular, linear, raised cosine or Gaussian, with separate rise and fall times |
| PRI pattern | Fixed, stagger (a list of PRIs in turn), or jitter (± %, repeatable with a seed) |
| Bursts | N pulses, then more dead time. A start delay before the first pulse |
| In-pulse modulation | An LFM chirp (up, down or triangle) with a set bandwidth, or a Barker code (2 to 13) |
| Playback | A SigMF or raw IQ recording as a canned waveform. Refer to [Playback](#playback) |
| Chain | Many recordings in turn. Refer to [Chain](#chain) |
| Impairments | White noise (SNR), DC offset, IQ gain and phase imbalance, carrier phase noise (linewidth) |

The pulses are slices of one continuous carrier. Thus the pulses are phase-coherent.

wvt checks the settings while you type. A setting with a problem shows in red, and wvt does not
apply it. These are examples of problems:

- A pulse that is wider than the PRI.
- A Barker chip that is shorter than one sample.
- An offset outside the band.

The **preview** shows one pulse or the pulse train in time, and the averaged spectrum. For a sweep,
it shows the frequency and the level over one period, and a max-hold spectrum of the swept band.

## Sweeps

A sweep changes the frequency, the level, or the two, over one period. The sweep repeats until the
output stops.

- **Frequency sweep:** ramp up, ramp down, triangle, or stepped (a step size and a dwell time). The
  sweep goes between two offsets from the center.
- **Level sweep:** ramp up, ramp down or triangle, linear in dB, between two dBFS levels.
- The frequency sweep and the level sweep have one period.

While the frequency sweeps, the sweep replaces the **Offset** and the **Drift**. While the level
sweeps, the sweep replaces the **Amplitude**.

In **CW** mode, the tone follows the sweep continuously. Its phase stays continuous through the
wraps and the steps.

In **Pulsed** mode, use the **Sweep** section under **Pulse**. Each pulse takes the frequency and
the level of the sweep at the start of the pulse. The pulse keeps them for its full length. Use this
for pulse-to-pulse frequency agility, stepped-frequency trains, or a scan-like amplitude pattern. A
chirp or a Barker code has its center on the swept frequency.

- If you change a sweep setting while an output runs, the period starts again.
- The noise (SNR) refers to the peak of the level sweep.

## Playback

Playback plays a recording. The recording can be a file from the generator, a wvt recording, or
any SigMF or raw IQ capture.

1. Type the path of the recording, or use a shortcut:
   - **Last generated** loads the last file from the generator.
   - **Viewer's file** loads the file that is open in the viewer, with the format settings of the
     viewer.
   - **Viewer's markers** plays only the part between the time markers of the viewer.
2. Click **Load**. wvt reads the format, the rate and the frequency from the SigMF metadata or the
   file name. It then sets the generator to the same values. Thus the recording plays at its
   original rate and frequency.

### Rules

- The 0 Hz point of the recording goes to the center plus the offset.
- The peak of the recording plays at the set amplitude. **Keep its level** sets the amplitude to
  the recorded peak. The recording then plays without a change of level.
- If the recording has a different rate, wvt resamples it.
- wvt can play up to 32 Mi samples (2^25) at a time, at the recording rate or at the generator
  rate.
- The impairments apply on top of the recording.
- With **Loop** off, a file output has the start delay and one play.
- Changes apply immediately. A different recording starts again from its beginning.

### How resampling works

wvt uses a windowed-sinc resampler with a stopband of approximately 80 dB. It changes the rate by
up to 64 times in each direction. When wvt lowers the rate, a filter removes all content outside
the new band. Thus this content does not alias.

## Chain

A chain plays a list of canned signals in order. A signal can be a file from the generator, a
region that you exported from the viewer, or any SigMF or raw IQ recording.

### Make a chain

1. Select the **Chain** mode.
2. Add recordings. Type a path and click **Add**, or use the **Last generated**, **Viewer's file**
   and **Viewer's markers** shortcuts.
3. Click a row to edit its item.
4. Use the **^**, **v** and **x** buttons on a row to move the item up, move it down, or remove
   it.

Each item has its own format, file rate, segment, **offset**, **level**, number of **plays** and
**gap after** each play.

- When you add a recording, wvt reads its metadata.
- The offset puts the recording at its recorded frequency, if that frequency is in the band.
- The first recording also sets the rate and the center frequency of the generator.
- The default level is **As recorded**: the samples play without a change. If you clear this
  checkbox, the peak of the recording plays at the set dBFS value.

### Loop

- *Forever* plays the chain until the output stops.
- *Times* plays the chain N times. After the last pass, the output is silent. A file ends there,
  the loopback tells you, and a USRP stops the transmission.

### Changes while the chain plays

- Offset and level changes apply immediately.
- If you add, remove or move an item, or change a segment, the chain starts again from the first
  item. This occurs after the current item ends.

### Limits

- wvt loads all the items before the chain starts.
- All the items together can have up to 128 Mi samples (1 GiB as `cf32`). Each item can have up
  to 32 Mi samples.
- The noise (SNR) refers to the loudest item.

### Chain files

**Save** writes the chain to a `.wvtchain.json` file. The file has the items, the loop count, the
start delay, and the rate and the center frequency of the generator. Recordings in the folder of
the chain file, or below it, have paths relative to that folder. Thus you can move the folder.

**Load** reads a chain file. `--chain FILE` also reads a chain file.

The preview shows one pass. Each item has a shaded area and a number. The preview also shows the
average spectrum.

## Outputs

Select one output with *Output* at the top of the window: **File (SigMF / raw)**, **Loopback
(live view)** or **USRP transmit**.

### File

The generator writes SigMF or raw files in `cf32`, `ci16`, `ci8` or `cu8`. The file names have the
same pattern as recordings.

- The length is a duration. In pulsed mode, the length can also be a number of PRIs. With a sweep,
  it can be a number of sweep periods. With a chain, it can be a number of passes.
- The SigMF metadata has all the generator settings (`wvt:siggen`).
- As an option, the metadata has one annotation for each pulse, play, sweep or chain item.
- **Generate file** writes the file. **Open in viewer** opens the result.

### Loopback

The loopback streams the waveform into the live spectrum and waterfall.

1. Select the **Loopback (live view)** output.
2. Click **Start loopback**. wvt starts the device *Signal generator loopback* in the main window,
   at the rate and the center frequency of the generator.
3. To stop, click **Stop loopback**.

You can also select the device *Signal generator loopback* in the **Device** mode of the main
window, or start wvt with `--device siggen`.

The live view shows the waveform as a receiver tuned to the center frequency shows it. Changes
apply while you make them. The main window can record the loopback stream.

### USRP transmit

The generator transmits the waveform from a USRP, tuned to the center frequency. You can change
the frequency, the gain and all waveform settings while the USRP transmits.

| Setting | Function |
|---|---|
| Device args | The UHD device arguments, for example `serial=31A2B3C` or `addr=192.168.40.2`. Empty: the first USRP that UHD finds |
| Antenna | The transmit port. Empty: the first transmit port of the channel (`TX/RX` on the B2xx and N3xx, `TX/RX0` on the X410) |
| Channel | The transmit channel |
| Reference | The clock and time reference (*internal*, *external* or *gpsdo*). It applies to the full device |
| Bandwidth | The analog filter bandwidth. 0 sets it automatically |
| TX gain | The transmit gain. While the USRP transmits, the slider shows the range of the device |

If the live view receives from the same USRP, the receiver and the transmitter share it.

:::{warning}
Transmit only into a dummy load or an attenuated cable, or where you have a license.
:::

wvt transmits only after you do these steps:

1. Tick **Arm transmitter**.
2. Click **Transmit...**.
3. In the dialog, examine the frequency, the rate and the gain.
4. Click **Transmit**.

The arm setting resets after each transmission.

Safety and limits:

- The TX gain starts at 0 dB.
- While wvt transmits, the window title, the menu bar (**TX ON**) and a large **STOP
  TRANSMITTING** button show it.
- If you close the generator window or stop wvt, the transmission stops.
- If you change the center frequency during a transmission and the USRP cannot tune to it, the
  transmitter stays on the last good frequency and the field shows that frequency. If the USRP
  cannot go back to that frequency, the transmission stops.
- If a transmission stops because of an error (for example, the USRP is disconnected), wvt sends
  an end-of-burst to the USRP before it stops. This tells the USRP to stop transmitting.
- To monitor your own signal on a receiver, put a minimum of 30 dB of attenuation between the
  ports.
- On one USRP, wvt can receive and transmit at the same time (full duplex). On a B2xx, the two
  directions must use the same sample rate, because the B2xx uses one clock for the two. A network
  USRP can use a different rate for each direction.
- Between pulses, the USRP transmits zeros. Thus a small LO leakage stays at the center frequency.
  A non-zero offset keeps the signal away from this leakage.

### Transmit limits

For a lab, you can set limits that wvt cannot go past. The command line and the window both obey
them.

| Limit | Option | Environment variable | [Configuration file](config.md) |
|---|---|---|---|
| The maximum TX gain, in dB | `--tx-max-gain DB` | `WVT_TX_MAX_GAIN` | `tx.max_gain_db` |
| The frequency bands where wvt can transmit | `--tx-allow LO:HI[,LO:HI...]` | `WVT_TX_ALLOW` | `tx.allow` |

- The strictest limit applies. Each source only adds limits: wvt uses the lowest maximum gain, and
  only the frequencies that every source allows. An option cannot remove a limit of the
  configuration file or of an environment variable.
- If the allowed bands of two sources do not overlap, wvt cannot transmit at any frequency.
- The full transmit band (the center frequency ± half the sample rate) must be in one allowed
  band, because the generator can put a signal anywhere in that band.
- wvt does not start a transmission, change the gain or change the frequency past a limit. The
  TX gain slider stops at the maximum gain. The **Transmit** dialog shows the limits, and why it
  cannot transmit.
- If the value of an environment variable is not correct, wvt does not start.

For example, put this in your shell profile:

```sh
export WVT_TX_MAX_GAIN=30
export WVT_TX_ALLOW=902M:928M,2.4G:2.4835G
```

## Command line

`-c` sets the center frequency and `-r` sets the sample rate.

| Option | Function |
|---|---|
| `--siggen` | Open the signal generator window |
| `--gen cw\|pulsed\|playback` | The mode. This option also opens the window |
| `--offset HZ` | The carrier offset from the center (default 100 kHz) |
| `--amp DBFS` | The peak amplitude (default −6 dBFS) |
| `--drift HZ` | The CW frequency drift per second |
| `--sweep FROM:TO` | A frequency sweep between two offsets, for example `-400k:400k`. CW, or pulse to pulse with `--gen pulsed` |
| `--sweep-shape up\|down\|tri` | The sweep shape |
| `--sweep-step HZ:DWELL` | A stepped sweep |
| `--sweep-time S` | The sweep period |
| `--level-sweep FROM:TO` | A level sweep in dBFS over the same period |
| `--level-shape up\|down\|tri` | The level sweep shape |
| `--pri S` or `--prf HZ` | The pulse repetition interval or frequency |
| `--pw S` | The pulse width, with the edges |
| `--edge rect\|linear\|cos\|gauss` | The edge shape. `--rise S` and `--fall S` set the edge times |
| `--stagger S,S,..` | PRIs in turn |
| `--jitter PCT` | PRI jitter. `--seed N` makes it repeatable |
| `--burst N[:GAP]` | N pulses for each burst, and `GAP` seconds of silence after each burst |
| `--delay S` | Silence before the first pulse |
| `--lfm up\|down\|tri:BW` | A chirp in each pulse, for example `--lfm up:2M` |
| `--barker N` | A Barker phase code (2, 3, 4, 5, 7, 11 or 13) |
| `--play FILE` | Play a SigMF or raw IQ recording. Its rate and frequency become `-r` and `-c`, if you do not give them |
| `--play-rate HZ` | The rate of the recording |
| `--play-segment START:LEN` | Play a part of the recording. `LEN` 0 plays to the end |
| `--play-gap S` | Silence between the plays |
| `--play-once` | Do not repeat |
| `--chain FILE` | Play a chain file (`.wvtchain.json`) |
| `--chain-add PATH[@OFFSET][:N]` | Add a recording to the chain, played N times. You can use this option more than one time. The first recording sets `-r` and `-c`, if you do not give them |
| `--chain-loops N` | The number of passes through the chain. 0 (default) plays until you stop it |
| `--snr DB`, `--dc I:Q`, `--iq GAIN_DB:PHASE_DEG`, `--linewidth HZ` | Impairments |
| `--gen-out PREFIX` | Write to a file, then stop. `--record-format` sets the file type |
| `--gen-sample cf32\|ci16\|ci8\|cu8` | The file sample format (default `cf32`) |
| `--gen-duration T` or `--gen-pulses N` | The file length (default 1 s). N counts pulses, plays, sweeps or chain passes |
| `--gen-annotate` | SigMF: one annotation for each pulse, play, sweep or chain item |
| `--tx` | Transmit with a USRP until Ctrl+C, or for `--gen-duration` |
| `--tx-gain DB` | The transmit gain (default 0 dB) |
| `--tx-args ARGS` | The UHD device arguments. The default is the first USRP found |
| `--tx-antenna NAME` | The transmit port. The default is the first transmit port of the channel |
| `--tx-dry-run` | Show what `--tx` transmits, check the limits, then stop. wvt does not open the USRP |
| `--tx-confirm` | Transmit, and do not ask first. Use this in a script |
| `--tx-max-gain DB` | The maximum TX gain (see [Transmit limits](#transmit-limits)) |
| `--tx-allow LO:HI[,LO:HI...]` | The bands where wvt can transmit (see [Transmit limits](#transmit-limits)) |
| `--usrp-channel N`, `--ref SRC` | The USRP channel and reference, as for the receiver |

Before `--tx` transmits, wvt shows the signal, the frequency, the rate, the gain, the device, the
duration and the limits. Then it asks you to type `yes`. If the input is not a terminal (for
example, in a script), wvt cannot ask: it stops, unless you give `--tx-confirm`.

### Examples

```sh
wvt --gen pulsed --pri 1m --pw 20u --lfm up:2M -r 10M -c 915M                     # open the window
wvt --gen pulsed --pri 1m --pw 20u --barker 13 -r 2M --gen-out ~/caps/b13 --gen-pulses 100 --gen-annotate
wvt --device siggen --gen cw --offset 250k -c 915M -r 2M                          # loopback view
wvt --device siggen --sweep -400k:400k --sweep-shape tri --sweep-time 2 -c 915M -r 2M  # sweep, loopback
wvt --sweep -1M:1M --sweep-time 10m --level-sweep -40:-6 -r 5M -c 915M --gen-out ~/caps/sw --gen-pulses 20
wvt --sweep -200k:200k --sweep-step 25k:5m -r 1M -c 915M --tx --tx-gain 10        # stepped, transmit
wvt --gen pulsed --pri 1m --pw 20u --sweep -2M:2M --sweep-step 500k:10m --level-sweep -30:-6 -r 10M -c 915M
wvt --gen cw --offset 100k -c 915M -r 1M --tx --tx-gain 10 --gen-duration 5         # transmit 5 s
wvt --gen cw --offset 100k -c 915M -r 1M --tx-gain 10 --tx-dry-run                   # only show the plan
wvt --gen cw -c 915M -r 1M --tx --tx-confirm --tx-max-gain 20 --gen-duration 5      # from a script
wvt --play ~/caps/b13_*.sigmf-meta --play-gap 5m --gen-out ~/caps/b13x10 --gen-pulses 10  # 10 plays
wvt --play capture.cs16 --play-rate 2.4M -r 10M -c 915M --play-segment 1.5:0.2 --tx    # replay a burst
wvt --chain-add ~/caps/b13.sigmf-meta:3 --chain-add ~/caps/lfm.sigmf-meta@250k --chain-loops 5 --tx
wvt --chain ~/caps/demo.wvtchain.json --gen-out ~/caps/demo --gen-pulses 10 --gen-annotate
```

## How it works

- **Parameter changes:** the window sends a full copy of the settings to the generator. The
  generator applies the copy at the next block. In pulsed mode, it applies a change after the
  current pulse ends. Thus a change does not cut a pulse.
- **Sweeps:** a CW sweep uses a piecewise-linear phase rotator. Its step also rotates, so the phase
  is exactly quadratic. The rotator starts again at the end of each period, at the turn of a
  triangle, and at each step. The period is a whole number of samples.
- **Playback and chains:** wvt loads each recording once, resamples it, and keeps it in a cache.
  The cache keeps up to 128 Mi samples. When the cache is full, wvt removes the recordings that
  are not in use.
- **Outputs:** the file output, the loopback and the transmitter use the same generator. Thus the
  three outputs give the same samples.

The self-check `build/siggen_check` tests the generator without hardware. For the designs, refer to
[the sweep design record](design/siggen-sweep-plan.md) and
[the chain design record](design/siggen-chain-plan.md).
