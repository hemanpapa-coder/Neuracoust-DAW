# Codex task — Power-amp / speaker-cable / power-cable / connector catalog + modeling

## Role & goal
You are working in the **Neuracoust DAW** repo (a native macOS SwiftUI/AppKit app over a C++
audio engine, sources under `src/`). Your job is to **research real specifications and
measurement data** for studio **power amplifiers, speaker cables, power cables, and connectors**
— including **vintage studio models** — and turn that into (a) a **spec catalog** the app can show,
and (b) **modeling data** (heuristic tone curves now; real measured curves where a trustworthy
measurement exists) that colours the monitor path.

This feeds the monitor "physical output chain": **audio interface D/A → power amp → speaker cable →
speaker** (and, separately, the AC power cable + connectors feeding the amp/interface). Passive
studio monitors are driven by an external amp + cable; those already have name-heuristic tone in the
app and this task upgrades them with real data and more models.

## NON-NEGOTIABLE honesty rules (do not violate)
These come from `docs/audio-interface-da-modeling-research/claude_handoff.md`. They apply here too:
1. **Never fabricate measurement coefficients.** Only a real, citable measurement may become an
   `independent_measurement: "available"` profile that drives audio as a *measured* curve.
2. **Empty numeric fields stay `null`, never `0`.** Unknown = `null`, not a guessed number.
3. If you only have a **spec sheet** (not a measured frequency/impulse response), it goes in the
   catalog as **spec metadata** and, at most, informs a **small, clearly-labeled name/type
   heuristic** tone — never a fake "measured" curve.
4. Modeling tone must be **level-matched / null-test safe**: an all-flat / identity curve when there
   is no real effect. **Cables and modern solid-state amps are essentially flat — be honest about
   that; do not invent an audible "cable sound."**
5. Every catalog entry records provenance: `measurement_source`, `independent_measurement`
   (`available` | `spec_only` | `none`), `confidence` (`high`|`med`|`low`), and a `source`/URL for
   any measured data. Compute a SHA-256 over any raw measurement file you add.

## What already exists (read these first — grep, don't guess)
- **Amp/cable name catalogs** (C++): `powerAmpModelCatalog()` and `speakerCableModelCatalog()` in
  `src/bridge/NeuracoustEngineBridge.cpp` (near `nc_power_amp_model_name` / `nc_speaker_cable_model_name`).
  These are `std::vector<std::string>` of model names shown in the pickers.
- **Heuristic tone curves** (C++): `powerAmpToneCurve(name)` and `speakerCableToneCurve(name)` in the
  same file (anonymous namespace just above `nc_monitor_eq_sync`). They map a model NAME → a small
  `ResponseCurve` (`std::vector<std::pair<double,double>>` = (Hz, dB)); empty = flat/no colour.
- **Where the tone is applied**: `nc_monitor_eq_sync` folds `powerAmpToneCurve`/`speakerCableToneCurve`
  into the combined monitor EQ, **only when the speaker is passive** (physical speaker via
  `physicalPowerAmpModel`/`physicalSpeakerCableModel`, and per-A/B/C-slot via
  `powerAmpA/B/C` + `speakerCableA/B/C` on the `speaker-simulation` `MonitorDspModule`).
- **Passive-speaker detection**: `speakerModelIsPassive(name)` (same file).
- **Existing catalog JSON patterns to mirror**: `resources/audio_interface_catalog.json`
  (interface specs) and `resources/speaker_model_dataset.json` (speaker specs + measured curves,
  200-point log 20 Hz–20 kHz, midband-normalized). The generated C++ profile pattern is
  `tools/gen_*_profiles.py` → `src/audio/*Profiles.generated.cpp`.
- **The `ResponseCurve` type + helpers**: `src/audio/MonitorCorrection.h`
  (`interpolateCurveDb`, `normalizeCurveMidband`).

## Deliverables
1. **`resources/power_amp_catalog.json`** — one entry per power amp. Fields (use `null` for unknown):
   `name, brand, family, model, era, type (tube|solid_state|class_d), studio_use (bool),
   power_w_8ohm, power_w_4ohm, damping_factor, bandwidth_hz (low, high), thd_percent,
   snr_db, slew_rate_v_us, input_impedance_ohm, output_topology, notes,
   independent_measurement, measurement_source, source_url, confidence`.
2. **`resources/speaker_cable_catalog.json`** and **`resources/power_cable_catalog.json`** and
   **`resources/connector_catalog.json`** — specs: `name, brand, type, gauge_awg,
   resistance_ohm_per_m, inductance_uh_per_m, capacitance_pf_per_m, conductor_material,
   shielded (bool), connector_type, notes, independent_measurement, measurement_source, source_url,
   confidence`. **Be honest**: for reasonable runs these are near-inaudible; reflect that.
3. **Extend the name catalogs** `powerAmpModelCatalog()` / `speakerCableModelCatalog()` (and add
   `powerCableModelCatalog()` / `connectorModelCatalog()` + `nc_*_model_count/name` accessors and
   Swift pickers) to include the researched models, **especially vintage studio staples**.
4. **Upgrade the tone heuristics** `powerAmpToneCurve` / `speakerCableToneCurve` (and add power-cable
   / connector equivalents *only if there is a real, defensible effect*) so they are **derived from
   the catalog's real spec fields** (e.g., tube output-transformer HF rolloff + damping-factor-driven
   LF, cable R+L HF loss from gauge/length) rather than hardcoded guesses. Keep them SMALL and
   honest. Where a **real measured curve** exists for a specific unit, add it as a measured profile
   (generated .cpp, like the speaker/interface profiles) and prefer it over the heuristic.
5. **Wire everything through** so the response window's "실물 출력 체인" (physical output chain) shows
   the specs + status per stage, and the modeling actually applies.

## Models to research (start here, expand)
- **Power amps (vintage/studio)**: Bryston 4B/3B, Yamaha P-series (P2200/P2201/P2500S),
  Crown DC300/D-75/Macro-Tech, BGW 250/750, Hafler, Amcron, Quad 405/909, McIntosh MC-series,
  Threshold/Pass, and classic tube amps (Mcintosh MC275, Marantz 8B, Dynaco Stereo 70, Quad II).
  Modern reference: Purifi/Hypex NCore/Ncore, Benchmark AHB2, ATI.
- **Speaker cables**: common gauges (10–16 AWG OFC), Mogami, Canare 4S11/4S8, Belden, Monster,
  Kimber, generic zip. **Effect is minimal — say so.**
- **Power cables + connectors**: IEC C13/C14/C19, Neutrik powerCON, Furutech/Oyaide (audiophile),
  standard vs shielded, gauge. **Effect on the audio path is essentially nil for competent gear —
  document that honestly; do not model a fake "power cable sound."**

## Constraints & verification
- **English field names** in JSON (mirror the existing catalogs). Comments/labels may be Korean to
  match the codebase; the app UI is Korean.
- Add a **unit test** (mirror `tests/MonitorCorrectionTest.cpp` / `tests/SweepMeasurementTest.cpp`
  style, registered in `CMakeLists.txt` + ctest) proving: an all-flat entry yields an identity curve
  (null-test), and a spec-derived heuristic produces the expected small tilt.
- Build must pass: `cmake --build build`; run `ctest --test-dir build` — `bridge_transport_smoke`
  and `project_io_smoke` must stay green (they catch serialization/aggregate-init breakage — **add
  any new `MonitorDspModule` fields at the END of the struct** or positional init crashes).
- **Cite a source URL for every measured number.** No source → it is spec/`null`, not measured.

## Files to attach when handing this to Codex
- `docs/audio-interface-da-modeling-research/claude_handoff.md` (the honesty contract)
- `src/bridge/NeuracoustEngineBridge.cpp` (catalogs, `powerAmpToneCurve`, `nc_monitor_eq_sync`)
- `src/audio/MonitorCorrection.{h,cpp}` (ResponseCurve + helpers)
- `resources/audio_interface_catalog.json`, `resources/speaker_model_dataset.json` (catalog patterns)
- `src/plugins/MonitorDspModules.h` (per-slot amp/cable fields)
- `src/project/ProjectDocument.cpp` (module serialization pattern)
- `src/app/swift/MonitorDock.swift` + `SpeakerComparisonView.swift` (pickers + chain display)
