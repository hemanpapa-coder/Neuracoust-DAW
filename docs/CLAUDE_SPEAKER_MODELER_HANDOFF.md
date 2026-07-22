# Claude handoff — speaker model dataset refresh

Date: 2026-07-19 (Asia/Seoul)

## Completed scope

- Rechecked every missing catalog entry against the current Spinorama API.
- Preserved strict identity matching. Similar generations and family members were not substituted.
- Added six new studio speakers whose default measurement is an independent, high-quality Klippel data set.
- Regenerated the C++ response-curve table consumed by the monitor DSP.

## Dataset state

- Catalog entries: 191
- Entries with measured response curves: 71
- Entries without a verified curve: 120
- Every measured curve is a 200-point, 20 Hz–20 kHz Listening Window curve normalized to the 300 Hz–3 kHz mean.

## Added models

| Catalog model | Measurement origin | Method / quality | Curve |
|---|---|---|---|
| ADAM D3V (NF) | Erin's Audio Corner | Klippel / high | `Adam D3V`, `eac` |
| Genelec M040 (NF) | Audio Science Review | Klippel / high | `Genelec M040`, `asr` |
| Kali LP-6v2 (NF) | Erin's Audio Corner | Klippel / high | `Kali LP-6v2`, `eac` |
| Kali LP-UNF (NF) | Erin's Audio Corner | Klippel / high | `Kali LP-UNF`, `eac` |
| Kali SM-5 (NF) | Erin's Audio Corner | Klippel / high | `Kali SM-5`, `eac` |
| PreSonus Eris E5 XT (NF) | Audio Science Review | Klippel / high | `Presonus Eris E5 XT`, `asr` |
| Meyer Sound HD-1 (NF) | Meyer Sound official technical report | multiple FFT, continuous 1/3-octave graph / medium after digitization | Figure 1, 0.5 m tweeter axis |

The exact metadata, provenance, review links, selected version and API curve URL are stored in each JSON row under `research`, `sources`, and `response_curve.source_url`.

## Files changed for this work

- `tools/build_speaker_model_dataset.py`
  - Adds an explicit `ADDITIONAL_CATALOG_MODELS` allowlist.
  - Appends missing allowlisted models to an existing dataset.
  - No longer assumes the legacy catalog is exactly 184 entries.
  - The allowlist comment documents the acceptance threshold: independent high-quality Klippel measurements only.
- `src/bridge/NeuracoustEngineBridge.cpp`
  - Adds the same six names to `speakerModelCatalog()`.
- `resources/speaker_model_dataset.json`
  - Now contains 190 rows and 70 measured curves.
- `src/audio/SpeakerProfiles.generated.cpp`
  - Regenerated from the JSON; now contains 70 embedded response curves.

## Commands to reproduce

```sh
cd "/Volumes/Program Dev/DW"
python3 tools/build_speaker_model_dataset.py resources/speaker_model_dataset.json
python3 tools/gen_speaker_profiles.py
cmake --build build
(cd build && ctest --output-on-failure)
```

The refresh requires network access to `https://api.spinorama.org/v1`.

## Verification performed

- JSON/catalog validation passed: 191 unique rows, 71 finite ascending 200-point curves.
- `speakerModelCatalog()` and the JSON catalog matched exactly as sets.
- `python3 tools/gen_speaker_profiles.py` generated 71 embedded C++ profiles.
- `cmake --build build -j 8` completed successfully.
- A full parallel CTest run was attempted. Several existing audio-engine test processes printed passing output but did not terminate, so the run was stopped; this hang is not attributed to the speaker data change. Do not claim a clean 37/37 CTest pass until that pre-existing process-lifetime issue is resolved.

## Important research result

The refresh still reports 120 exact catalog names as unmatched. Do not map a nearby model merely because the brand, cabinet size, or series is similar. In particular:

- `Tannoy Profile 638 Black Ash Plus (MF)` has published specifications but no verified public response curve found.
- Its published specifications are 35 Hz–30 kHz (±3 dB), 91 dB sensitivity, 6 ohm nominal / 4 ohm minimum impedance, and 400 Hz / 2.5 kHz crossovers.
- These limits are not enough to synthesize a frequency-response curve and must remain `points: null`.
- `Tannoy System 600` must not be reused for Profile 638; it is a different loudspeaker.

Other tempting substitutions that remain prohibited without direct evidence include Genelec 8020D→G2, Genelec 8040B→8050B, HEDD Type 05 MK2→Type 05 A-CORE, PreSonus Eris E5→E5 XT, and family-wide mappings for ATC/PMC/Amphion models.

## Acceptance rules for future additions

1. Exact model identity or a documented manufacturer alias is required.
2. Prefer independent Klippel/anechoic measurements.
3. Store source URL, origin, method, quality, selected version, and all evidence links.
4. Never generate an estimated response curve from bandwidth specifications or a related model.
5. Normalize only after a real curve has been retrieved.
6. Regenerate `SpeakerProfiles.generated.cpp` after every JSON curve change.
7. Verify the bridge catalog and JSON catalog contain the same model names.

## Existing worktree warning

The repository contained many unrelated user changes before this task. Do not reset, overwrite, or reformat unrelated files. Limit follow-up edits to the four speaker files above and this handoff unless the user explicitly broadens scope.

## Suggested next research pass

- Search archived print reviews and manufacturer service documents for exact missing model measurements.
- Accept digitized plots only if the source graph identifies the exact model and measurement conditions; label quality and digitization method honestly.
- For rare models such as the Tannoy Profile 638, prefer a new calibrated REW/Klippel measurement over inferred data.
- Consider adding a machine-readable `docs/speaker-measurement-backlog.json` only if research tracking is needed; do not mix research candidates into production curves.
