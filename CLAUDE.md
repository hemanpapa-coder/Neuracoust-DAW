# Neuracoust DAW — New UI build (DW)

This project builds a **new UI** for the Neuracoust DAW while **reusing the existing, battle-tested C++ audio engine** from the sibling project at `/Volumes/Program Dev/DAW`.

## Goal
Reproduce the Claude Design look (`Neuracoust DAW.dc.html`, made in Claude Design — an HTML/CSS visual spec) as a **native macOS app**, driven by the **same realtime audio engine** we already built. Keep engine and UI as separate layers so both can keep evolving independently.

## Architecture — Plan A (native app, reuse engine)
- **Reuse as-is (do NOT rewrite):** the audio/engine layer from `../DAW/src`:
  - `src/audio` — realtime engine (CoreAudio, render graph, DSP, jitter/dropout handling)
  - `src/plugins` — VST3 hosting incl. the out-of-process realtime bridge + editor host
  - `src/project` — project model, playlists, edit operations
  - `src/core` — DawState and core types
  - Plus the build wiring (`CMakeLists.txt`) and the VST3 SDK references those need.
- **Rebuild fresh:** only the UI layer (equivalent of `../DAW/src/app/macos/DawWindowController.mm`), matching the Claude design.
- **Design-token/theme layer FIRST:** centralize colors, spacing, radii, typography as native variables so future design changes = swap tokens, not rewrite components. Build the UI as reusable component views (transport, track lane, mixer strip, inspector, monitor, meters, knobs, faders, timeline/waveform).
- Consider **SwiftUI** for the new UI (easier to match modern designs; interop with the C++ engine and any AppKit bits via NSHostingView). AppKit is also fine. Decide with the user.

## The engine is proven — inherit these hard-won facts (see `../DAW` git history + its memory)
- **VST3 realtime hosting is out-of-process** over shared memory (crash-prone/blocked plugins run live via a worker). Editor GUIs run in a separate editor-host process.
- **Playlist model is the render source of truth**: playback renders from `trackPlaylists` placements, NOT the flat `project.clips` list. Copy/paste must reconcile into placements or clips play silent.
- **Waves SoundGrid output delivers CoreAudio callbacks in bursts** → wake-jitter reads ~1 buffer period even idle; severity must be based on render headroom + a learned baseline, not raw jitter.
- **Every edit/Play must not rebuild the whole graph**: cache decoded WAVs, reuse still-valid plug-in workers by signature, don't tear down chains on load. Realtime render must not block long on the engine mutex (priority inversion → dropouts).
- Thread priority hierarchy: audio render = RT time-constraint > worker processing (USER_INITIATED) > worker load + editor observer (UTILITY).

## First steps for the new session
1. Confirm with the user: SwiftUI vs AppKit for the new UI, and get the `Neuracoust DAW.dc.html` (export the file here, paste it, or connect the Claude Design MCP) so you can map it component-by-component.
2. Copy the reusable engine dirs from `../DAW` into this project and get a minimal build (engine-only) compiling.
3. Stand up the design-token layer + a shell window, then implement panels one at a time against the engine API.
