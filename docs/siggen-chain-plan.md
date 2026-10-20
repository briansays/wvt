# Signal generator: chain playback (issue #27)

Status: plan (2026-10-20), decisions in §7. Not started.

## 1. Goal

The issue asks to play a chain of canned signals: files made by the generator's File output,
regions exported from the viewer (issue #18), or any SigMF or raw recording. The chain plays its
items in order and then starts again from the beginning. It loops until the user stops it, or for a
set number of loops.

## 2. Starting point

- **Playback mode** plays one recording.
  - `loadPlayback()` reads a segment, resamples it to the output rate and caches it.
  - `SigGen` uses the result as its template, with the rate, offset and level of `WaveformParams`.
  - A play repeats with a gap, or plays once after the start delay.
- **The cache holds a single waveform** (`gWave`). The preview, loopback, transmitter and file
  output share it.
  - The window marks the generator ready only once the cache has the recording (`sgReady_`).
  - That way the streaming threads never load a file themselves.
- **Limits.**
  - A play is limited to 32 Mi samples (`kMaxPlaySamples`, 256 MB as cf32).
  - `lastPulses()` reports each play. The file output uses that to count plays and annotate them.
- **Formats.**
  - `iq_file` reads the SigMF fields wvt needs: datatype, rate, frequency and datetime. It does
    this with a small key lookup (`jsonValue`), not a JSON parser.
  - Generator files also carry `wvt:siggen`.

## 3. Design

### 3.1 A new mode: **Chain**

Playback keeps its simple one-file form. Chain is a fourth radio button, next to CW, Pulsed and
Playback, and has its own section:

```
Chain                                                       [Load…] [Save…]
 #  Recording            Length   Offset     Level        Repeats  Gap after
 1  b13_20261020.sigmf   12.0 ms  +0 Hz      as recorded  1        5 ms      [↑][↓][✕]
 2  lfm_burst.sigmf      40.0 ms  +250 kHz   -10 dBFS     3        0         [↑][↓][✕]
 3  region_0001.sigmf    1.20 s   -1.1 MHz   as recorded  1        100 ms    [↑][↓][✕]
 [ path…                                   ] [Add]  Last generated  Viewer's file/markers
 Loop      (•) Forever  ( ) [ 5 ] times   Start delay [ 0 s ]
 Total     1.37 s per pass, 312 MB loaded
```

- Selecting a row opens its details below the table: format, file rate and segment, as in
  Playback today, plus offset, level, repeats and gap. Each row also has a tooltip with the full
  path.
- **Adding an item** reads its metadata, as **Load** does now.
  - **Offset** defaults to `(item's recorded fc) − (generator fc)`, so each signal plays at its
    original RF frequency.
  - If that frequency is outside the band, the offset is 0 and a warning says so.
  - The first item can set the generator's rate and fc from its metadata. This uses the same
    rule as Load.
- **Level** is per item.
  - "As recorded" (the default) keeps the file's own samples.
  - A dBFS value scales the item's peak to it.
  - Generator files and exports were made at a deliberate level, so "as recorded" is the right
    default.
  - The section has no single amplitude field. Impairments still apply on top of the whole chain.

### 3.2 Parameters

```cpp
struct ChainItem {
    std::string path;
    FormatParams fp;
    double fileRate = 0;             // 0 = output rate
    double start = 0, len = 0;       // segment, s
    double offset = 0;               // Hz from fc
    bool keepLevel = true;           // else peak at ampDb
    double ampDb = -6;
    int repeats = 1;
    double gapAfter = 0;             // s, after each repeat
    bool operator==(const ChainItem&) const = default;
};
// in WaveformParams:
std::vector<ChainItem> chain;
int chainLoops = 0;                  // 0 = forever
```

`startDelay` is reused before the first pass.

### 3.3 Loading and memory

- **The cache becomes a map**, keyed by `WaveKey`, with a total budget of 1 GiB of cf32
  (128 Mi samples).
  - It evicts the least recently used waveforms that no generator holds (shared_ptr
    `use_count() == 1`).
  - Playback keeps working through the same calls.
- **Loading.**
  - The preview worker loads every item, cancellably, in the same way it loads the recording for
    Playback.
  - The generator is ready when all items are in the cache. The window shows "loading 2 / 5" until
    then.
  - The streaming threads still never read files.
- **Errors.**
  - An item that can't be loaded marks its row red with the reason, and the chain is not ready.
  - A chain over the budget is an error that names the total.
  - The 32 Mi samples per item limit stays.
- **Streaming instead of preloading** would remove the size limits. It needs a streaming resampler
  and read-ahead on the transmit thread, so it is a follow-up if large chains turn out to matter.

### 3.4 Generation

- `SigGen` keeps the position in the chain: item, repeat and pass.
- **Within a pass:**
  - an item plays as a template, like Playback;
  - its own offset and gain go into the carrier while it plays;
  - then the gap; then the next repeat or item.
- The carrier and the phase run continuously across items, as between pulses now.
- **Timing.**
  - The schedule keeps fractional sample positions (`lastStartF_`), as Pulsed does, so the gaps
    are exact.
  - An item at a different file rate was already resampled at load time, so every item runs at the
    output rate.
- **Events.** Each item start goes into `lastPulses()` with its item index.
  - The file output can write "1: b13_20261020" annotations.
  - The file length can be a number of **passes** ("Pulses" → "Passes"), an exact count.
- **End of the loops.** After the last pass the output is silent, and `SigGen::finished()` becomes
  true.
  - **File:** the file ends there.
  - **Loopback:** stays silent and says "chain finished".
  - **USRP transmit:** stops by itself when the generator finishes (`TxEngine` checks
    `finished()`), with the reason "chain finished after N passes". This stops the zeros and the
    LO leakage.
- **Changes while running.**
  - Editing an item's offset or level applies at once.
  - Adding, removing or reordering items, or changing a segment, restarts the chain from item 1
    after the current item ends. Pulsed uses the same rule.
  - Changing the loop count applies at once. If the current pass is already past the new count,
    the chain finishes when the pass ends.

### 3.5 Saving a chain

- **Save… / Load…** write and read a small JSON file (`*.wvtchain.json`):
  - the items, the loops and the start delay;
  - paths relative to the chain file when they sit beside it or below it, so a folder of canned
    signals can be moved as a whole.
- **Reading it back** needs a JSON reader. `iq_file`'s key lookup can't read arrays, so the plan
  adds a minimal parser to `src/json.*`:
  - about 200 lines, for objects, arrays, strings, numbers and booleans;
  - it can later replace `jsonValue` too.
- **CLI:**
  ```
  --chain FILE.wvtchain.json       open the chain (implies Chain mode)
  --chain-add PATH[@OFFSET][xN]    add an item (repeatable), e.g. b13.sigmf-meta@250k x3
  --chain-loops N                  0 = forever (default)
  ```
  - This works with `--gen-out` (with `--gen-pulses N` for N passes), with `--tx` and with the
    loopback device.

### 3.6 Preview

- **Time plot:** one pass, shown as the envelope.
  - The item boundaries are shaded, with their numbers.
  - A long pass is decimated with max-hold, as Playback does now.
- **Spectrum:** an average over the pass.

## 4. Steps

1. **Cache.**
   - Turn the waveform cache into a map with a budget.
   - Playback must behave the same (existing tests).
2. **Parameters.**
   - `ChainItem`, `GenMode::Chain` and the `checkParams` rules: an empty chain, repeats ≥ 1,
     gaps ≥ 0, offsets in band, and a level ≤ 0 dBFS.
   - Update `describeParams` ("chain of 3, 5 passes") and `paramsJson` (the items).
3. **Generator.**
   - The chain schedule, per-item offset and gain, events, `finished()`, and the rules for changes
     while running.
4. **Outputs.**
   - `FileGenJob` "Passes" length and item annotations.
   - `TxEngine` stops when the generator finishes.
   - Loopback says the chain finished.
5. **JSON reader and chain files:** `src/json.*`, save and load.
6. **Window:**
   - the Chain section, the table and its row editor, the add shortcuts and loading progress;
   - the preview with the item boundaries.
7. **CLI and `--help`.**
8. **Self-test** (`tools/siggen_check.cpp`), using three short generated files with distinct tones:
   - order and repeats: the tone sequence matches;
   - the gaps are exact to the sample;
   - "as recorded" keeps the samples bit-exact at a matching rate;
   - a dBFS level sets the peak;
   - the per-item offset moves the tone;
   - 3 loops end with `finished()`, and the file is exactly 3 passes long;
   - forever keeps going;
   - a reorder while running restarts after the current item;
   - the save → load round trip is equal;
   - the cache budget evicts unused waveforms only.
9. **README** (a Chain part in the Signal generator section) and CLI examples.
10. **Screenshots** of the window with a 3-item chain, and a loopback run that shows the items in
    the live waterfall.

## 5. Questions

1. Should chain items be files only (recommended)? Any setting can already be made into a file
   with File output. The alternative is to also allow generator settings (CW, pulsed or sweep for
   T seconds) as items, which turns this into a sequencer.
2. Should Chain be a new mode with Playback unchanged (recommended), or should Playback become a
   chain of one?
3. Should the USRP stop transmitting when a finite chain ends (recommended), or keep sending
   silence until Stop?
4. Should chains be savable as `.wvtchain.json` with a small JSON reader (recommended)?
5. Is preloading with a 1 GiB budget fine, with streaming from disk left for later (recommended)?

## 6. Order with issue #28

- I'd do the sweep (#28) first.
  - It is self-contained: about 4 files, with no new memory or file-format work.
  - Its "N sweeps" event and annotation path is the same mechanism Chain uses for passes.
- The chain (#27) touches the cache, the outputs, a new JSON reader and a larger window section.
- Two branches, `28-siggen-sweep` and then `27-siggen-chain`, give two PRs that can be tested
  on their own.

## 7. Decisions (2026-10-20)

1. Chain items are files only.
2. Chain is a new mode. Playback stays as it is.
3. The USRP stops transmitting when a finite chain ends.
4. Chains can be saved and loaded as `.wvtchain.json`, with a small JSON reader.
5. Items are preloaded with a 1 GiB budget. Streaming from disk is left for later.
