# Signal generator: frequency and amplitude sweep (issue #28)

Status: implemented (2026-10-20) on `28-siggen-sweep`; decisions in §6, notes in §7, pulsed sweeps in §8.

## 1. Goal

The issue asks for a signal that sweeps its frequency, its amplitude or both over a set range,
and keeps running until the user stops it.

## 2. Starting point

- `SigGen` (`src/siggen/siggen.*`) has three modes: CW, Pulsed and Playback.
- CW already changes its frequency over time: `driftHzPerSec` is a linear drift that wraps at the
  band edge. `carrier()` steps the phase in blocks of 256 samples, and gets the frequency from
  `p_.offset + drift * t` at the start of each block.
- The phase is continuous across parameter changes (`apply()`), and the carrier stays continuous
  between pulses (`advanceCarrier`).
- The outputs (File, Loopback, USRP transmit) run whatever `SigGen` produces. Loopback and transmit
  already run until the user stops them, so "until told to stop" comes for free there.
- `lastPulses()` reports events, which the file output turns into annotations and counts. Pulses
  and plays already use it.

## 3. Design

### 3.1 Where it lives: CW gets sweeps (recommended)

A sweep is a tone whose frequency or level changes, so it belongs with CW. The plan adds two
optional, independent sweeps to the CW section:

```
CW
  Drift          [ 0 Hz/s ]                    (disabled while the frequency sweeps)
  Freq sweep     [Off | Ramp up | Ramp down | Triangle | Stepped]
    From / to    [ -400 kHz ] [ +400 kHz ]     offsets from the center, like Offset
    Period       [ 10 ms ]                     one sweep (a triangle goes up and down in it)
    Step / dwell [ 10 kHz ] [ 1 ms ]           Stepped only; Period is then derived
  Level sweep    [Off | Ramp up | Ramp down | Triangle]
    From / to    [ -40 dBFS ] [ -6 dBFS ]      linear in dB, over the same period
```

- There is one period for both sweeps (§6). A level sweep alone uses the Period row, which shows
  whenever either sweep is on. With a stepped frequency sweep the period is the number of steps
  times the dwell, and the level sweep follows it.
- Offset sets the frequency only while the frequency sweep is off, and Amplitude sets the level
  only while the level sweep is off.
- The alternative is a fourth mode called **Sweep**. It would duplicate the carrier settings, so I
  don't recommend it.

### 3.2 Parameters (`WaveformParams`)

```cpp
enum class SweepShape : int { Off = 0, RampUp, RampDown, Triangle, Stepped, Count };
SweepShape fSweep = SweepShape::Off;
double fFrom = -400e3, fTo = 400e3;  // offsets from fc, Hz
double sweepPeriod = 10e-3;          // s, both sweeps (Stepped: derived from step and dwell)
double fStep = 10e3, fDwell = 1e-3;  // Stepped
SweepShape aSweep = SweepShape::Off; // Stepped is not offered for the level
double aFrom = -40, aTo = -6;        // dBFS, over the same period
```

### 3.3 Generation

- A sweep clock `sweepN_` counts samples from the start of the sweep.
  - The frequency at sample k is `f(t)` for the selected shape, with `t = (sweepN_ + k) / rate`
    taken modulo the period.
  - The phase adds `2π f(t) / rate` for each sample, so the signal is phase-continuous through the
    wraps and the steps.
- `carrier()` gets a sweep path that computes the frequency every 16 samples. This is the same idea
  as the block anchoring it does now, with a block of 16 instead of 256.
  - Over 16 samples the frequency changes by at most `16 × |slope| / rate`. That is small next to
    the bin width for any sweep that `checkParams` accepts.
  - A ramp's wrap and a step change happen exactly on their sample.
- The level is `amp(t) = 10^(a(t)/20)`, computed for each block and multiplied in.
  - Noise is set from the SNR at the **peak** sweep level. This is the same rule as now ("peak
    signal power / total noise").
- **Events.** The start of each period goes into `lastPulses()`, as `{start, periodSamples}`.
  - The file output can then write "sweep N" annotations.
  - It can also be told to write exactly N sweeps: the "Pulses" length option becomes "Sweeps".
- **Changes while running.**
  - A change to a sweep setting starts the period again from the start. The phase stays continuous.
  - A change that doesn't touch the sweep (impairments, phase, fc) leaves the sweep clock alone.
- The sweep always repeats until the user stops the output (§6).

### 3.4 Checks (`checkParams`)

- **Errors:**
  - From and To are inside the band, `|f| < rate/2`, for every point the sweep reaches;
  - Period > 0 and ≥ 2 samples;
  - Step > 0 and dwell ≥ 1 sample;
  - From ≠ To for a frequency sweep;
  - the level is ≤ 0 dBFS at both ends.
- **Warnings:**
  - a sweep rate high enough to smear the spectrum (more than one bin per sample of the preview
    FFT);
  - drift is ignored while the frequency sweeps;
  - a Stepped sweep with more than 10 000 steps.

### 3.5 Preview

- **Time plot.** It shows one period of the frequency (Hz, left axis) and the level (dBFS, right
  axis). These are computed from the parameters, so a 10 s sweep at 50 MS/s costs nothing.
  - It replaces the |x|/I/Q plot for a CW sweep.
- **Spectrum.** A max-hold over the sweep shows the band that it covers.
  - It is built from up to 64 short blocks spaced evenly through one period. Each block is made
    with a helper, `SigGen::seekSweep(t)`, so the preview doesn't generate the whole period.
  - An average would make a slow sweep look like noise, which is why it's a max-hold.

### 3.6 Description, metadata, CLI

- **`describeParams`**, e.g. "CW, sweep -400 kHz → +400 kHz triangle 10 ms, level -40 → -6 dBFS
  1 s".
- **`paramsJson`** adds a `sweep` object, so a file says how it was made.
- **CLI:**
  ```
  --sweep FROM:TO            frequency sweep (offsets, SI suffixes), shape from --sweep-shape
  --sweep-shape up|down|tri|step
  --sweep-time T             period
  --sweep-step HZ:DWELL      stepped sweep
  --level-sweep FROM:TO      dBFS, over the --sweep-time period
  ```
  - Any of these implies `--gen cw`. `--gen-pulses N` also accepts a sweep as "N sweeps".

### 3.7 Out of scope

- **Sweeping beyond the sample band** by retuning the USRP while it transmits (an RF sweep). It
  could be a follow-up issue: `TxEngine::setCenterFreq` exists, but retune timing and the
  transients make it a separate job.
- **Log sweeps.** The band is at most a few tens of MHz, so linear covers the practical cases.
  `SweepShape` leaves room to add it.
- **Sweeps on playback signals.** (Pulsed sweeps were added later: §8.)

## 4. Steps

1. **Parameters and checks.**
   - Add `SweepShape` and its fields, `sweepShapeName`, and `checkParams` rules.
   - Update `describeParams` and `paramsJson`.
2. **Generator.**
   - Add the sweep clock, the sweep path in `carrier()` and the level in each block.
   - Report events through `lastPulses()`, define what a change does, and add `seekSweep`.
3. **File output.** "Sweeps" length mode and "Mark each sweep" annotations (`FileGenJob`, and
   `drawSigGenFile`).
4. **Window.** Sweep rows in the CW section, the preview's frequency/level plot, and the max-hold
   spectrum.
5. **CLI and `--help`.**
6. **Self-test** (`tools/siggen_check.cpp`):
   - a ramp's instantaneous frequency (phase difference) follows the line within 0.1 % of the span;
   - the ramp wraps on the right sample and the phase stays continuous (no step larger than the
     maximum frequency);
   - a triangle turns around at To;
   - a stepped sweep holds each step for the dwell, ±1 sample;
   - the level ramp measured in dB is linear within 0.05 dB;
   - "N sweeps" writes exactly N × period samples;
   - a change mid-sweep restarts the period and keeps the phase;
   - `checkParams` rejects an out-of-band To.
7. **README** (Signal generator section) and the CLI examples.
8. **Screenshots** of the window at 1100 px, and a loopback run that shows the sweep in the live
   waterfall.

## 5. Questions

Answered in §6.

## 6. Decisions (2026-10-20)

1. The sweeps are part of CW, not a separate mode.
2. Shapes: ramp up, ramp down, triangle and stepped. No log sweeps.
3. The level sweep shares the frequency sweep's period (one Period field).
4. The sweep always repeats until the output is stopped. There is no "once" option.

## 7. Implementation notes (2026-10-20)

**Files**

| File | Change |
|---|---|
| `src/siggen/siggen.h/.cpp` | `SweepShape`, the sweep fields, `sweepSamples` / `sweepOffsetAt` / `sweepLevelAt`, checks, description, metadata; `SigGen::sweep()` and `seekSweep()` |
| `src/siggen/siggen_out.h/.cpp` | `eventName()`; file length in sweeps and "sweep N" annotations; the sweep preview (curves and max-hold spectrum) |
| `src/app/app_siggen.cpp`, `src/app/app.h` | sweep rows in the CW section, Offset / Amplitude disabled while they are swept, `drawSweepPreview`, "Sweeps" length |
| `src/app/main.cpp` | `--sweep`, `--sweep-shape`, `--sweep-step`, `--sweep-time`, `--level-sweep`, `--level-shape` |
| `tools/siggen_check.cpp` | 23 sweep checks |
| `README.md` | Sweeps row, a Sweeps paragraph, file length, CLI options and examples |

**Differences from the plan**

- The generator splits the period into pieces where the frequency and the level (in dB) are both
  linear: up to the period end, a triangle's turn and each step. Within a piece, a rotator whose
  step rotates too gives the exact quadratic phase, and the level is a geometric factor in the same
  rotator. The measured frequency is within 0.03 Hz of the formula (the plan allowed 0.1 % of the
  span), and the level is exact to 0.0001 dB.
- The period is rounded to whole samples, so "N sweeps" is exactly N × period samples.
- With only the level sweeping, the frequency follows Offset and Drift, both exactly (drift is a
  linear piece too).
- `--level-shape` was added for the level sweep's shape (the plan had only `--sweep-shape`).
- The preview's max-hold uses between 64 and 1024 blocks, one per 4096 samples of the period, so a
  slow sweep shows a filled band instead of separate lines.
- No "sweep rate smears the spectrum" warning: the max-hold spectrum already shows it.

**Verification**

- `siggen_check` passes, with the new checks: ramp up, ramp down, triangle, stepped, the wrap,
  phase continuity, the level ramp and triangle, level-only with drift, one event per period, a
  change restarting the period, five parameter checks, a 4-sweep file with annotations, and the
  preview's max-hold.
- The other self-tests (audio, log, segment, gpu, extract) pass.
- Screenshots under Xvfb: the window at 1600 and 1100 px (triangle, stepped, level ramp), and the
  loopback live view showing a 2 s ramp with a level triangle in the waterfall.

## 8. Pulsed sweeps (2026-10-20)

The user asked for the same frequency and level sweeps in pulsed mode.

**Design**

- Each pulse takes the sweep's frequency and level at the sample where it starts, and keeps them
  for the whole pulse. The pulse shape, the edges and any chirp or Barker code stay intact.
- This models pulse-to-pulse frequency agility, stepped-frequency trains and scan-like amplitude
  modulation. A continuously swept carrier under each pulse would instead add a small chirp to
  every pulse.
- The sweep clock runs on samples, independently of the PRI schedule (`sweepT0_`), so stagger,
  jitter and bursts sample the sweep where their pulses fall.
- Between pulses the carrier keeps the last pulse's frequency, so the phase stays continuous.
- The events are still pulses: the file length counts pulses, and the annotations mark pulses.
- The settings, CLI options (`--sweep...` now also work with `--gen pulsed`), the description and
  the metadata are shared with CW. The metadata's `sweep` object adds `"per_pulse": true`.

**Checks**

- The swept frequency plus half a chirp must stay inside the band; past the edge it warns, as a
  plain chirp does.
- It warns when the sweep period is shorter than two PRIs.
- It warns when a stepped sweep's dwell is shorter than the PRI, because then some steps get no
  pulse.

**Window**

- The Pulse section is followed by a **Sweep** section with the same rows as CW
  (`drawSweepRows`). It opens by itself when a sweep is on.
- The preview shows the sweep curves above the one-pulse or pulse-train plot.
- The spectrum is a max-hold of blocks spread over the period, with a pulse centred in each block
  (`seekSweep(k, pulseIn)`).

**Also**

- The noise reference (`noiseAmp_`) is now separate from the carrier gain, so noise doesn't follow
  the level of each pulse. With a level sweep it refers to the sweep's peak.
- As a side effect, Playback's noise now refers to its played peak (the amplitude), not to the
  scaled recording.

**Verification**

- `siggen_check` has 8 new checks:
  - each pulse's frequency and level match the sweep at its start (within 0.001 Hz and 1e-6 dB);
  - silence between pulses;
  - a stepped sweep with 3 pulses per step;
  - a chirp centred on the swept frequency;
  - the short-period warning;
  - the description and metadata;
  - the preview's max-hold covering the band.
- All self-tests pass.
- Screenshots: the Pulsed window with a stepped frequency sweep and a triangle level sweep, and
  the loopback showing a pulse train sweeping in triangle with a rising level.
