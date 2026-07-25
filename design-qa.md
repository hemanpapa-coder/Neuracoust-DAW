# SSL 4000E Channel Strip Design QA

Reference:
`/var/folders/5r/mjkhlkhn3xd55cw3xjkdyy4m0000gn/T/TemporaryItems/NSIRD_screencaptureui_pnuxD7/스크린샷 2026-07-25 오전 10.42.20.png`

Verified build:
`/Volumes/Program Dev/DW/build/Neuracoust DAW.app`

Captured state:
Neuracoust DAW `260725.1048`, Edit view, Audio 1 channel, EQ module selected.

## Comparison

- The compressor and EQ now use the reference's fixed two-column hardware rhythm.
- EQ band colors and ordering match the reference: HF red, HMF green, LMF cyan, LF dark.
- HMF and LMF use the compact SSL-style triangular layout: Gain and Q in the left
  column, with Frequency centered between them in the right column.
- Band headers are reduced to a compact identifier row, removing the previous unused
  vertical gap.
- Knobs are approximately 20% larger than the prior implementation.
- Numeric values are centered inside the knob face; units remain outside.
- The value pointer is a thin hollow circle at the knob perimeter instead of a line
  through the numeric readout.
- The redundant `4000E` power indicator beside each module title is removed. The
  Filter, EQ, Gate/Expander, Compressor, and Saturator titles now provide the
  enable/bypass interaction and brighten when active.
- Console knobs are enlarged by a further 10% while keeping their numeric value and
  external unit labels clear.
- HF/HMF/LMF/LF identifiers and Bell switches use the formerly empty right column,
  reducing dedicated header rows and overall EQ height.
- Every built-in processor panel now exposes its model in a consistent TYPE menu.
- Processor toggles update only their owning channel rather than reloading and
  remeasuring the complete mixer.
- Compressor utility labels use compact console abbreviations (`F.ATK`, `THR`, `REL`).
- `4000E` is the compressor enable/bypass control, matching the EQ interaction.
- The module face is a flat blue-black panel without the previous decorative gradient.
- No control or unit label is visibly clipped at the normal channel width.

## Remaining polish

- P3: At whole-window screenshot scale the smallest unit labels are intentionally quiet;
  they remain legible at the native application scale.

final result: passed
