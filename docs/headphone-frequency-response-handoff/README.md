# Neuracoust DAW headphone frequency-response handoff

Generated from the verified-measurement catalog in `src/bridge/NeuracoustEngineBridge.cpp` and AutoEq commit `7ae0f56d53074872b028649617a22bbb4232feb7`.

## Contents

- `catalog_status.csv`: one row for every retained DAW catalog entry.
- `measurements.jsonl`: provenance, match type, point count, checksum, and source link for every copied curve.
- `curves/`: source-preserving numerical CSV files (`frequency,raw`; dB values are relative/normalized, not absolute SPL).
- `summary.json`: machine-readable counts.

## Rules used

No curve was synthesized, averaged across rigs, or substituted from a successor model. Multiple source files are retained independently because fixtures/targets are not directly interchangeable, especially above 8–10 kHz. Modified-pad measurements are excluded unless the catalog itself specifies those pads. Models previously classified as unavailable, ambiguous, generic, or conditionally measured were removed from both the DAW picker and this handoff package.

Primary numerical collection: https://github.com/jaakkopasanen/AutoEq (MIT). AutoEq aggregates measurements from the source named in each path; consult each source's terms before redistributing commercially. This package is research input, not yet a correction filter set.
