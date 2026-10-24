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
| `build/snapshot_check` | Plot snapshots: file names, numbering, the crop and the PNG files |
| `build/session_check` | File sessions: write and read back, the recording after a folder move, session files and recordings told apart, no replace without asking, bad values |
| `build/measure_check` | Point measurements: the nearest sample, the snap to a peak or a valley, which differences apply, the plot names |
| `build/device_check` | The device list entries from a UHD search, device arguments and the sample rate rules |
| `build/clipboard_check` | The system clipboard, both ways, with a second X client: UTF-8, Latin-1, large texts in parts. It needs an X server (for example `xvfb-run -a build/clipboard_check`); without one it is skipped |
| `build/bb_check` | The BB60 backend with the simulated device: open, rates, bandwidths, streaming, overload, unplug, and the capture segments of a recording across retunes. Only with `-DWVT_BB_SIM=ON` |
| `build/skiq_check` | The Sidekiq logic: the gain in dB to a gain index, the lost samples from the RF timestamps, the 12-bit scaling and the I/Q order. With `-DWVT_SKIQ_SIM=ON`, also the backend with the simulated card: open and its errors, streaming, the tone level on a 12-bit and a 16-bit card, a retune while streaming, overload, USB, an overrun and a card removal |
| `build/gpu_check` | The GPU results against the CPU results. `--bench` measures the speed |

To run all of them, use CTest:

```sh
ctest --test-dir build --output-on-failure
```

CTest shows each program as one test. To run one program, add `-R NAME`, for example
`-R extract_check`.

## Sanitizer builds

wvt has many threads, lock-free rings and memory-mapped files. Run the self-checks in a sanitizer
build to find memory errors and data races. Refer to [`WVT_SANITIZE`](build.md#wvt_sanitize).

```sh
cmake -S . -B build-asan -DWVT_SANITIZE=address -DWVT_BB_SIM=ON
cmake --build build-asan -j
cmake -S . -B build-tsan -DWVT_SANITIZE=thread -DWVT_BB_SIM=ON
cmake --build build-tsan -j
```

Then run the self-checks from `build-asan/` and `build-tsan/`. A sanitizer error stops the
program with a report and a non-zero exit code.

:::{note}
On Linux kernels with a high address-space randomization (for example Ubuntu 24.04), ThreadSanitizer
can stop with `unexpected memory mapping`. Then type `sudo sysctl vm.mmap_rnd_bits=28` before you
run the checks.
:::

## Fuzz tests

The fuzz targets give random and mutated input to the parsers that read text from files and from
the user. They need clang with libFuzzer (on macOS: the Homebrew `llvm`, because Apple clang has no
libFuzzer). Refer to [`WVT_FUZZ`](build.md#wvt_fuzz).

| Program | Parser |
|---|---|
| `fuzz_json` | `parseJson` and the typed getters of `JsonValue` |
| `fuzz_chain` | `loadChain` (chain files), then `checkParams` on the result |
| `fuzz_meta` | `guessMeta`: file names (gqrx and the rate and frequency in a name) and SigMF metadata files |
| `fuzz_parse_si` | `parseSI`, the number parser of the command line and the input fields (`2.4M`, `915 MHz`) |

To build them and run them on their seed inputs (`tools/fuzz/corpus/`):

```sh
cmake -S . -B build-fuzz -DWVT_FUZZ=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz -j --target fuzz_json fuzz_chain fuzz_meta fuzz_parse_si
ctest --test-dir build-fuzz -R '^fuzz_' --output-on-failure
```

To fuzz one parser for 10 minutes, use a copy of the seed inputs. libFuzzer adds new inputs to the
first folder that you give it, and you do not want them in the repository:

```sh
cp -r tools/fuzz/corpus/chain /tmp/corpus-chain
./build-fuzz/fuzz_chain -max_total_time=600 -max_len=4096 /tmp/corpus-chain
```

If the program finds a problem, it stops with a report and writes the input to a `crash-...` file.
To run that input again, type `./build-fuzz/fuzz_chain crash-...`.

## Static analysis

clang-tidy checks the wvt code with the `bugprone`, `cert`, `concurrency` and `performance` check
families. The configuration is in `.clang-tidy`. It turns off the checks that found only intended
code in wvt, and gives the reason for each. `tools/.clang-tidy` turns off some more checks for the
test programs. A finding is an error. To run it as CI does:

```sh
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
pip install clang-tidy==23.1.3
tools/run-clang-tidy.sh build
```

Use the same clang-tidy version as CI, because a different version can find different things. To
accept one finding on purpose, put `// NOLINT(check-name)` and the reason on the line.

## Continuous integration

GitHub Actions builds each pull request and each push to `main` (`.github/workflows/build.yml`):

| Job | Contents |
|---|---|
| Linux (CPU, RTL-SDR, UHD) | Builds with the two device backends and the simulated BB60, runs all self-checks with CTest and some command-line runs |
| macOS (Apple silicon, Metal, RTL-SDR, UHD) | Builds on an Apple silicon runner with XQuartz, Accelerate, the Metal backend, the two device backends and the simulated BB60. Runs all self-checks and the same command-line runs as the Linux job |
| Linux CUDA 12.0 and 12.8 | Builds the CUDA backend. The runners have no GPU, so the job makes sure that a CUDA build runs on the CPU |
| Sanitizers (address, thread) | Builds with `WVT_SANITIZE=address` (ASan and UBSan) and with `WVT_SANITIZE=thread` (TSan), and runs all self-checks |
| clang-tidy | Runs `tools/run-clang-tidy.sh` (clang-tidy 23.1.3). A finding causes the job to fail |
| Fuzz the parsers | Builds the fuzz targets with clang, runs them on the seed inputs, then fuzzes each one for 60 s. If a fuzz target finds a problem, the job keeps the input as a `fuzz-crashes` artifact |
| Documentation | Builds these pages with Sphinx. A warning causes the job to fail |

The Linux, macOS and CUDA build jobs use [`WVT_WERROR=ON`](build.md#wvt_werror): a compiler warning in
the wvt code causes the job to fail.
