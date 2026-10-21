# Load a segment

On a large file, the first view of the full file can be slow. The waterfall reads samples from all
of the file. Above its work limit, it analyzes only a part of the samples ("FFT sees" below
100 %). To work on a part of the file only, load a segment.

## Set a segment

1. In the file row, type the start time in the first **Segment** box, in seconds.
2. Type the end time in the second **Segment** box.
3. Press Enter.

wvt then shows the new segment and calculates all the plots again.

You can type the times before you open the file, or change them after. From the command line, use
`--segment T0:T1`.

## Rules

- `-1` in the first box is the start of the file. `-1` in the second box is the end of the file.
  `-1` to `-1` loads the full file. This is the default.
- The plots, readouts, bookmarks and markers show the file time. For example, a segment from 40 s
  to 50 s shows 40 s to 50 s, not 0 s to 10 s.
- The view, the scrollbar, the overview strip and the audio ranges stay in the segment.
- If the start is after the end of the file, or the end is before the start, wvt tells you why.
  wvt then keeps the previous segment.
- If the end is after the end of the file, wvt uses the end of the file.
- The segment stays when you open the next file. If the segment does not fit that file, wvt loads
  the full file.

## Examples

```sh
wvt big.sigmf-meta --segment 40:50                 # 40 s to 50 s
wvt big.sigmf-meta --segment 80:-1 --view 90:91    # 80 s to the end; start with a 1 s view at 90 s
wvt big.sigmf-meta --segment 40:50 --chan 300k:200k --audio wfm --audio-out seg.wav   # 10 s of audio
```

## How it works

wvt does not copy the segment. The file stays memory-mapped, and all the calculations use the
sample range of the segment. The overview strip calculates its power only over this range. The
self-check `build/segment_check` tests the rules on this page.

For the design, refer to [the time segment design record](design/time-segment-plan.md).
