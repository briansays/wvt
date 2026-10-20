# Self-checks and test signals

## Test recording

`iqgen` writes a 2 s capture at 2 MS/s:

```sh
./build/iqgen out.sigmf-data --fmt ci16
```

The capture contains these signals:

| Offset | Signal |
|---|---|
| +300 kHz | FM, 1 kHz tone, 50 kHz deviation, slow fade |
| −250 kHz | AM, 400 Hz tone, 60 % depth |
| −600 kHz | PM, 2 rad sine, plus 90° steps each 20 ms |
| +650 kHz | 2-FSK bursts |
| +100 kHz | A weak CW tone |
| — | A chirp from 1.0 s to 1.4 s |

To examine the FM signal, type `wvt out.sigmf-meta --chan 300k:150k --view 0.5:0.505`.

The `sim` device streams synthetic signals without hardware: `wvt --device sim -c 100M -r 2.4M`.

## Self-check programs

The self-checks do not need hardware. Each program shows its result and returns a non-zero exit
code if a check fails.

| Program | Checks |
|---|---|
| `build/siggen_check` | The signal generator: waveforms, sweeps, chains, playback cache, chain files and the file output |
| `build/audio_check` | Each audio demodulator mode |
| `build/log_check` | The logger: levels, options, the log file and threads |
| `build/segment_check` | The time segment rules and the overview on a sample range |
| `build/extract_check` | The region export: copies, the band filter, metadata, input checks and cancel |
| `build/gpu_check` | The GPU results against the CPU results. `--bench` measures the speed |

To run all of them:

```sh
for t in siggen_check audio_check log_check segment_check gpu_check extract_check; do
  ./build/$t || echo "$t FAILED"
done
```

## Continuous integration

GitHub Actions builds each pull request and each push to `main` (`.github/workflows/build.yml`):

| Job | Contents |
|---|---|
| Linux (CPU, RTL-SDR, UHD) | Builds with the two device backends, runs all self-checks and some command-line runs |
| Linux CUDA 12.0 and 12.8 | Builds the CUDA backend. The runners have no GPU, so the job makes sure that a CUDA build runs on the CPU |
| Documentation | Builds these pages with Sphinx. A warning causes the job to fail |
