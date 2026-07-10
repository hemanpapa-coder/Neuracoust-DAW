# Neuracoust DAW — New UI build (DW)

This project builds a **new UI** for the Neuracoust DAW while **reusing the existing, battle-tested C++ audio engine** from the sibling project at `/Volumes/Program Dev/DAW`.

## Goal
Reproduce the Claude Design look as a **native macOS app**, driven by the **same realtime audio engine** we already built. Keep engine and UI as separate layers so both can keep evolving independently.

## Build

Everything — engine, tools, tests, and the SwiftUI app — builds from one directory:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
(cd build && ctest --output-on-failure)
open "build/Neuracoust DAW.app"
```

Swift needs a Swift-capable generator. Ninja and Xcode both qualify; under any other generator (e.g. Unix Makefiles) the app target is skipped with a message and everything else still builds.

Three things this tree forces, all of which cost time to rediscover:
- The source path contains a space (`/Volumes/Program Dev`). The bridging header and include dir must reach the compiler as separate, individually-quoted arguments — a `"SHELL:-import-objc-header <path>"` string gets split at the space. Under the Xcode generator they go through `XCODE_ATTRIBUTE_*` instead.
- A Swift file holding `@main` must not be named `main.swift`.
- The VST3 SDK is **referenced in place** at `../DAW/third_party/vst3sdk`, not copied — that tree also carries `depot_tools` and `libwebrtc-src` (~460k files). **DW therefore depends on the DAW folder existing.** WebRTC is off by default and not needed.

## Layers

| Layer | Path | Status |
|---|---|---|
| Engine (do NOT rewrite) | `src/{audio,core,project,plugins,license,nuclust,ai,ui}` | Imported verbatim, builds standalone |
| Editor hosts (engine spawns these) | `src/app/macos/*EditorHostMain.mm` | Kept verbatim |
| C facade for Swift | `src/bridge/NeuracoustEngineBridge.{h,cpp}` | Transport + status; grows per panel |
| Design tokens | `src/app/swift/Theme.swift` | Complete — from `docs/design-tokens.md` |
| SwiftUI shell | `src/app/swift/{NeuracoustApp,TransportBar,EngineController}.swift` | Titlebar, transport, status strip, Edit/Mix tabs |
| Monitor dock | `src/app/swift/{MonitorDock,MonitorControls}.swift` | Level, listen modes, A/B/C sets, meters, DSP modules, remote core |
| Listen Room | `src/app/swift/{ListenRoom,ChatPanel}.swift` | Relay process, ssh tunnel, chat over `/api/chat` |

**SwiftUI shell + AppKit/Metal embeds** is the agreed architecture. VST3 plugin editors must embed a native window in an `NSView` using the Steinberg SDK directly, so pure SwiftUI is impossible; the timeline already has a Metal backdrop and a 3,000-line `drawRect`. Everything else (transport, inspector, mixer layout, panels, dialogs) is far faster to build in SwiftUI.

## Two documents that must not be lost

- **`docs/legacy-ui-contract.md`** — what the discarded 50,602-line `DawWindowController.mm` did: the engine API it consumed, its threading rules, every panel and keyboard shortcut, and the non-view logic that exists nowhere else.
- **`docs/design-tokens.md`** — exact colors, spacing, radii, typography, gradients and shadows from `design/Neuracoust DAW v2.dc.html`, plus the mapping from the old `nc*()` theme helpers.

## Engine contract — the parts that bite

- **The engine pushes nothing.** No callbacks, no KVO, no notifications. It exposes one `AudioEngineStatus status()` snapshot; the UI polls it at ~30 Hz. Predict the playhead from the wall clock between polls and resync only past ~0.18 s of drift, or it steps visibly.
- **All engine calls are main-thread-only.** Never hold a lock while calling in.
- **Never rebuild the whole graph on an edit.** Try `updateProject()` first, fall back to `loadProject()` only if it returns false.
- **Playlist model is the render source of truth**: playback renders from `trackPlaylists` placements, not the flat `project.clips` list. (This reconciliation already lives in the engine — `copyPlaylistPlacementToActivePlaylist` in `EditOperations.cpp`.)
- **VST3 realtime hosting is out-of-process** over shared memory; editor GUIs run in a separate editor-host process.
- **Waves SoundGrid delivers CoreAudio callbacks in bursts** → wake jitter reads ~1 buffer period even when idle. Judge severity by render headroom against a learned baseline, never by raw jitter.
- Thread priority hierarchy: audio render (RT time-constraint) > worker processing (USER_INITIATED) > worker load + editor observer (UTILITY).

## Not yet ported out of the old UI — do this before anyone deletes it

One piece is left in `../DAW/src/app/macos/DawWindowController.mm`:

1. Plugin editor process lifecycle + the parameter bridge

Already ported:
- ~~DSP execution-mode policy~~ → `src/plugins/InsertDspPolicy.{h,cpp}`, pinned by `tests/InsertDspPolicyTest.cpp`. The mode strings are persisted in the project model, so the rules belong to the engine.
- ~~Undo/redo + dirty tracking + autosave~~ → `src/project/ProjectHistory.{h,cpp}`, pinned by `tests/ProjectHistoryTest.cpp`. Snapshot-based, capped at 100 steps. Autosave lives in the bridge, which owns the project path.

## Project I/O and audio import

`src/project/AudioImport.{h,cpp}` holds the import policy ported out of the old UI: where converted media goes (the project's `Audio Files` folder, or a temp folder when the document has no path yet), and that conversion shells out to `/usr/bin/afconvert`. No AppKit — `posix_spawn`, so it is testable. `tools/project_io_smoke.cpp` builds its own fixtures and runs in ctest.

The app also accepts files from Finder (`application(_:open:)`): a `.ndaw` opens as a project, audio imports onto track 0. That is how import and open were verified — the open/save panels belong to a system XPC service that automation cannot click.

**Not yet done:** nothing prompts to save before New, Open or Quit. Unsaved work is only protected by autosave.

## Keyboard shortcuts must match on key code, not characters

This machine types Korean. With a Korean input source active, `charactersIgnoringModifiers` for the Z key is not `"z"`, so any shortcut compared against a character silently stops working — including SwiftUI's `.keyboardShortcut("z")`. Match `NSEvent.keyCode` instead (Z is 6). The old UI already knew this: all 308 of its `NSMenuItem`s carry an empty `keyEquivalent` and every shortcut runs through an `NSEvent` monitor.

Menu items still declare their shortcuts so they are discoverable; the `NSEvent` monitor in `EngineController` is what actually delivers all of them (⌘Z, ⌘⇧Z, ⌘N, ⌘O, ⌘S, ⌘⇧S, ⌘I).

**Note for verification:** the computer-use tool cannot press Z here. It maps the character `"z"` to a key code using the active layout, which under Korean input yields key code 0 (the A key). Shortcut behaviour has to be checked by hand.

## Undo granularity

Continuous gestures (fader, pan, monitor knob) must record **one** step, not one per frame. The bridge deliberately records no history for `nc_track_set_volume_db` / `nc_track_set_pan`; the view calls `nc_history_record_gesture` when the drag ends. Everything else records itself.

## Listen Room

The engine encodes and pushes audio (`ListenRoomSender`); it does not run the relay. The relay is a Python daemon from the sibling **Neuracoust Listen** project, bundled into the app by CMake from `NEURACOUST_LISTEN_SOURCE_DIR`.

- `start-relay.sh` is a **launcher**: it `nohup`s the daemon and exits 0 once the daemon answers `/api/stats`. The script exiting is normal — do not read it as the relay dying. A non-zero exit is the only failure signal; the log is at `/tmp/neuracoust-listen-relay.log`.
- Because the daemon is detached, the app reaps it explicitly (`pkill -f listen_relay.py`) on stop and on `willTerminate`. Without that it outlives the app. The same applies to the reverse ssh tunnel.
- Chat rides the relay's `/api/chat` endpoint: GET with `since=<lastId>` polled once a second, POST to send. `sender` is `studio` for us.

## Open design questions

The design has no home for several existing features: MIDI piano roll, media pool, AI assistant, diagnostic log, automation lanes, marker/chord/lyric lanes, the 4-format ruler, the edit-mode pad, the routing matrix, and the settings dialogs. Decision deferred: build the new shell first, place these once it runs.

The design also *changes* structure — the mixer moves from a floating `NSPanel` to an Edit/Mix tab view, and the monitor station from a floating panel to a fixed 392px right dock.
