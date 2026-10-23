# Mouse and keyboard

**Help → Keyboard and mouse** shows these controls in the window.

## Mouse

| Action | Input |
|---|---|
| Pan | Left-drag |
| Zoom | Mouse wheel. To zoom one axis only, put the pointer on that axis |
| Zoom to a region | Right-drag a box. Hold Alt or Shift to limit the box to one axis |
| Fit to the data | Double-click |
| Tune the channel | Ctrl+click the spectrum or the waterfall. Or drag the cyan lines: the center line moves the channel, and the edge lines change the bandwidth |
| Play audio from a point | Shift+click a time plot or the waterfall |
| Put a point marker (P1, P2) | Alt+click a plot, or press `M`. Drag a marker to move it. Refer to [Point measurements](viewer.md#point-measurements) |
| Export a region | Right-click the waterfall |
| Save plot snapshots | Click **Snapshot...** in the status bar |
| Plot options | Right-click a time plot (I/Q, AM, FM, PM), or click its ⚙ button |

## Keyboard

| Action | Key |
|---|---|
| Pan the view | `←` and `→` (10 %), `PgUp` and `PgDn` (one page) |
| Zoom the time axis | `+` and `-` |
| Go to the start or the end | `Home` and `End` |
| Show everything | `A` |
| Show or hide the time markers | `T` |
| Show or hide the frequency markers | `F` |
| Put a point marker at the pointer | `M` |
| Clear the point markers | `Shift+M` |
| Zoom to the time markers | `Z` |
| Enable or disable the channel filter | `C` |
| Add a bookmark | `B` |
| Signal generator window | `Ctrl+G` |
| Audio window | `Ctrl+D` |
| Export window | `Ctrl+E` |
| Stop wvt | `Ctrl+Q` |

## Copy and paste

The text fields (for example, the file path, the frequencies and the times) use the system
clipboard. Thus you can copy a value from wvt into a different program, and paste a value into
wvt.

| Action | Key |
|---|---|
| Copy or cut the selected text | `Ctrl+C` or `Ctrl+X` |
| Paste | `Ctrl+V` |
| Select all the text in the field | `Ctrl+A` |

On macOS, the Command key (`⌘`) also does the `Ctrl` shortcuts, for example `⌘C` and `⌘V`. For
this, XQuartz must send `⌘` keys to wvt, and must copy between the X11 clipboard and the macOS
clipboard. In **XQuartz → Settings**:

- **Input:** clear **Enable key equivalents under X11**. If it is set, XQuartz uses the `⌘` keys
  itself. `Ctrl+C` and `Ctrl+V` work in each case. For Alt+click (point markers) with the Option
  key, set **Option keys send Alt_L and Alt_R**. If it is not set, press `M` instead.
- **Pasteboard:** set **Enable syncing**, **Update Pasteboard when CLIPBOARD changes** and
  **Update CLIPBOARD when Pasteboard changes** (the defaults).

wvt reads text that other programs give as UTF-8, Latin-1 (`STRING`) or `TEXT`, and large texts
that come in parts.
