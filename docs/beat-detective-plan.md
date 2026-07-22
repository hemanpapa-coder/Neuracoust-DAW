# Beat Detective → our DAW — Research & Plan (#7)

Status: PLAN (no code). 2026-07-21 overnight.
User: "프로툴즈의 비트디텍티브를 알지? 어떤 기능들이 있는지 자세히 조사해서 우리 DAW에 어떻게 넣을지 연구해줘.
쉽게 사용할 수 있도록 구조를 만들면 좋겠어."

## 1. What Beat Detective is (Pro Tools feature, faithful breakdown)

Beat Detective is a **transient-driven timing toolkit**. One panel, a fixed **left-to-right workflow**
over a selected region:

1. **Bar|Beat Marker Generation** — analyze transients in a selection and *extract a tempo map/groove*
   from performed audio (turn a live take into bars/beats).
2. **Groove Template Extraction** — capture the *feel* (timing + optional dynamics) of a selection as a
   reusable **groove template** (the DigiGroove / .grv idea).
3. **Clip Separation** — detect transients and **cut the region into little clips at each hit**
   (sensitivity slider + trigger-pad threshold; can quantize the detected points).
4. **Clip Conform** — **move** those separated clips to the grid (or to a groove template), with
   **Strength** (how far toward the grid), **Exclude Within** (leave near-perfect hits alone), and
   **Swing**.
5. **Edit Smoothing** — after conform, gaps/overlaps appear between clips; smoothing **fills gaps**
   (trim + auto-crossfade) so it sounds seamless. Fill Gaps / Fill and Crossfade.

Also: **Collection Mode** (analyze a multitrack drum kit together — detect on the kick, apply the same
cuts to every mic'd track so the phase stays coherent). This is the pro feature that makes it usable
on real drums.

Core primitives underneath: **transient detection** (onset detection), **grid/groove quantize of clip
positions**, **automatic crossfades**, **multitrack-coherent editing**.

## 2. What we already have (big head start)

- **Transient/onset analysis** exists: `AudioImportAnalysis` writes section markers on import, and the
  waveform peak cache (`nc_waveform_peaks`, 256 samples/peak) gives us the envelope to detect onsets.
- **Clip split** (`nc_clip_split`, `splitClip`) — the "separate at a hit" primitive.
- **Snapping / grid** (`snapProjectTime`, beat grid, `EngineController.snap`) — the conform target.
- **Move clip / selection move** with single-undo `nc_clip_*_many` — moving many separated clips.
- **Derived crossfades** (memory `dw-crossfade-derived`) — the render already makes crossfades from
  overlap. So "edit smoothing" (fill gaps + crossfade) largely *falls out of our existing model*:
  overlap the trimmed clips and the crossfade is automatic.
- **MIDI quantize / groove** grids already ported for the piano roll (Logic/Cubase grids) — the groove
  math can be shared.

So Beat Detective for us = **onset detection → split at onsets → conform (move to grid/groove with
strength/exclude/swing) → smoothing (trim+overlap→auto-crossfade)**, most pieces of which exist.

## 3. Proposed feature (fused, simplified — "쉽게 사용")

Rather than Pro Tools' 5-tab modal panel, a **3-step inline flow** on a selected audio range, driven
from the timeline toolbar or clip menu ("비트 정렬"):

- **① 감지 (Detect):** one **Sensitivity** slider. Live overlay of detected hit markers on the clip
  (draggable to nudge, click-to-add/remove — reuse the marker UI). Optional "드럼 킷 함께"
  (Collection Mode): pick the trigger track (kick), detect there, apply to the selected group.
- **② 정렬 (Conform):** **Strength %** + **Exclude within** (ms) + **Swing %**, quantizing to the
  current grid **or** a **groove template**. One button → separates at the hits and moves each piece.
  Records ONE undo step (via `nc_clip_*_many`).
- **③ 매끄럽게 (Smooth):** automatic — after conform, adjacent pieces are overlapped a few ms so the
  derived-crossfade engine fills the seams. A toggle if the user wants hard cuts.

Plus **그루브 추출**: capture the selection's timing (and later dynamics) into a named groove template,
reusable by ② and by the MIDI quantize.

Everything is **non-destructive** (splits + moves + derived crossfades — no new files), consistent with
the clip-edit work just shipped.

## 4. New engine work (the honest gaps)

1. **Onset detection** proper — a spectral-flux / high-frequency-content onset detector over the clip's
   PCM (not just the coarse peak cache) with the sensitivity threshold. Small, well-known DSP; run
   offline on the clip's WAV (like `normalizeClipGainToPeak` already reads the WAV).
2. **Batch "separate at markers" + "conform to grid/groove with strength/exclude/swing"** edit op —
   builds on `splitClip` + move, but as one atomic operation recording one undo step.
3. **Groove template model** — a small `{normalizedPositions[], strengths[]}` stored in the project;
   shared with MIDI quantize.
4. **Collection Mode** — apply the trigger track's detected cut points to sibling tracks at the same
   timeline positions (phase-coherent multitrack edit).

Smoothing/crossfade: **reuse** the derived-crossfade engine (no new code).

## 5. Phasing

1. Onset detector + "① Detect" overlay on a clip (prove detection quality with the sensitivity slider).
2. "② Conform" single-track: separate + move-to-grid with Strength/Exclude/Swing, one undo step;
   smoothing via overlap. This alone is a usable Beat Detective for single tracks.
3. Groove extraction + apply.
4. Collection Mode (multitrack drums) — the pro payoff.

## 6. Risks / decisions

- **Detection quality** is the make-or-break; budget time to tune the onset detector on real drum takes.
- Keep the UI to **3 controls + 1 optional group pick** — the PT panel is powerful but intimidating;
  our differentiator is the simple inline flow.
- Groove **dynamics** (velocity feel) is a v2 nicety; timing-only groove is enough for v1.
