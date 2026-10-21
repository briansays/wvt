# Signal processing

This page tells you how wvt calculates each plot of the file viewer.

## Waterfall

- wvt calculates windowed FFTs with fftshift, in dBFS.
- The view fits in a maximum of *Rows* rows. When a row covers more time than one FFT, the row
  averages sufficient FFTs to cover its full time slice. Thus a short pulse cannot fall between
  two FFTs.
- *Avg* sets the minimum number of FFTs for each row.
- Above 256 M samples of FFT input (2 G on the GPU), wvt uses a sample of each slice. The settings
  row then shows the percentage of the view that the FFTs see.
- wvt max-pools adjacent bins to *Cols* columns. Thus narrow signals stay visible.

## Spectrum of the view

The spectrum above the waterfall uses the linear power of each waterfall row:

- The average trace is the mean of the rows, in dB.
- The peak-hold trace is the maximum of the rows, in dB.

## Channel filter

The channel filter is optional. It does these steps:

1. It mixes the selected offset down to 0 Hz.
2. It applies a windowed-sinc FIR low-pass filter (Blackman window).
3. It decimates.

The automatic settings are: decimation = ⌊Fs / (2 · BW)⌋, and taps ≈ 12 · Fs / BW. With a GPU,
large full-resolution views do the mixer and the filter on the GPU. The mixer phase is a function
of the absolute sample number. Thus the results do not move when you pan.

## AM, FM and PM

AM
: |x|, as linear, dB, or normalized m(t) = |x| / mean − 1. The modulation index is
  m = (max − min) / (max + min).

FM
: The instantaneous frequency arg(x[n] · x*[n−1]) · Fs / 2π. It is absolute (the offset from the
  capture center) or relative to the channel center. The readout shows the mean, the peak deviation
  and the RMS deviation.

PM
: The phase: wrapped, unwrapped, or unwrapped with the best-fit line removed. The last option
  removes a remaining carrier offset. The unit is radians or degrees.

Smoothing
: A moving average for each of AM, FM and PM.

Squelch
: Blanks FM and PM where the smoothed power is below the threshold.

## Large files

- wvt memory-maps the files.
- wvt demodulates views up to *Max pts* samples (and approximately 32 M input samples) at full
  resolution.
- Wider views use **SPARSE** mode. This mode demodulates a short block for each screen pixel.
- All plots show the exact minimum and maximum envelope for each pixel.

## Background work

Each calculation runs on its own worker thread. A new request replaces the old request. Thus a
setting change shows its effect immediately, and the UI does not stop.
