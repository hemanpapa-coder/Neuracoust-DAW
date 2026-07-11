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
| Mixer | `src/app/swift/{MixerView,MixerControls}.swift` | Strips, faders, meters, inserts, master meter |
| Plugin browser | `src/app/swift/PluginBrowser.swift` | Facets, search, insert chain |
| Timeline | `src/app/swift/TimelineView.swift` | AppKit NSView: ruler, grid, clips, waveforms, playhead, select/move/trim/split/delete |

**SwiftUI shell + AppKit/Metal embeds** is the agreed architecture. VST3 plugin editors must embed a native window in an `NSView` using the Steinberg SDK directly, so pure SwiftUI is impossible; the timeline already has a Metal backdrop and a 3,000-line `drawRect`. Everything else (transport, inspector, mixer layout, panels, dialogs) is far faster to build in SwiftUI.

## Two documents that must not be lost

- **`docs/legacy-ui-contract.md`** — what the discarded 50,602-line `DawWindowController.mm` did: the engine API it consumed, its threading rules, every panel and keyboard shortcut, and the non-view logic that exists nowhere else.
- **`docs/design-tokens.md`** — exact colors, spacing, radii, typography, gradients and shadows from `design/Neuracoust DAW v2.dc.html`, plus the mapping from the old `nc*()` theme helpers.

## Engine contract — the parts that bite

- **The engine pushes nothing.** No callbacks, no KVO, no notifications. It exposes one `AudioEngineStatus status()` snapshot; the UI polls it at ~30 Hz. Predict the playhead from the wall clock between polls and resync only past ~0.18 s of drift, or it steps visibly.
- **All engine calls are main-thread-only.** Never hold a lock while calling in.
- **Never rebuild the whole graph on an edit.** Try `updateProject()` first, fall back to `loadProject()` only if it returns false.
- **Playlist model is the render source of truth, and almost nothing maintains it.** `makeProjectAudioRenderPlan` throws away `project.clips` and rebuilds them from `trackPlaylists`. Of all the clip edit operations, **only `appendAudioClipAt` calls `rebuildProjectEditModelFromClips`**. `moveClip`, `trimClipStart/End`, `splitClip`, `deleteClip`, `pasteClip` and `duplicateClip` all leave the placements stale — the picture changes and the audio does not. Every clip edit in the bridge therefore calls `rebuildProjectEditModelFromClips` before reconciling. `project_io_smoke` guards this by bouncing the document and measuring where the sound actually starts; it fails without the rebuild.
- **VST3 realtime hosting is out-of-process** over shared memory; editor GUIs run in a separate editor-host process.
- **Waves SoundGrid delivers CoreAudio callbacks in bursts** → wake jitter reads ~1 buffer period even when idle. Judge severity by render headroom against a learned baseline, never by raw jitter.
- Thread priority hierarchy: audio render (RT time-constraint) > worker processing (USER_INITIATED) > worker load + editor observer (UTILITY).

## Nothing is left in the old UI

`../DAW/src/app/macos/DawWindowController.mm` no longer holds anything this project
needs. Ported:

- ~~Plugin editor process lifecycle + parameter bridge~~ → `src/app/swift/PluginEditor.swift`
  plus `nc_track_set_vst3_parameter` / `nc_track_insert_observer`. **Track inserts only** —
  instrument slots, master inserts and monitor speaker slots (the old encoded-index
  scheme) and the Waves RS124 parameter mirroring are still unported, and nothing in the
  new UI asks for them yet.
- ~~DSP execution-mode policy~~ → `src/plugins/InsertDspPolicy.{h,cpp}`, pinned by `tests/InsertDspPolicyTest.cpp`. The mode strings are persisted in the project model, so the rules belong to the engine.
- ~~Undo/redo + dirty tracking + autosave~~ → `src/project/ProjectHistory.{h,cpp}`, pinned by `tests/ProjectHistoryTest.cpp`. Snapshot-based, capped at 100 steps. Autosave lives in the bridge, which owns the project path.

## Project I/O and audio import

`src/project/AudioImport.{h,cpp}` holds the import policy ported out of the old UI: where converted media goes (the project's `Audio Files` folder, or a temp folder when the document has no path yet), and that conversion shells out to `/usr/bin/afconvert`. No AppKit — `posix_spawn`, so it is testable. `tools/project_io_smoke.cpp` builds its own fixtures and runs in ctest.

Audio files drag onto the timeline: the `TimelineNSView` is a dragging destination
for `.fileURL`, maps the drop to (lane, seconds), and imports there via
`importAudio(intoTrack:at:from:)`. The drop target is highlighted in **CALayers**
(a band + insertion line), not `draw(_:)` — a layer-backed view does not flush
`draw(_:)` during the drag-tracking run loop, but a layer change commits immediately,
the same reason the playhead lives in a layer. Those layers use the view's own
top-down y directly: the backing layer of a flipped `NSView` is geometry-flipped by
AppKit, so re-flipping sends them off screen.

The app also accepts files from Finder (`application(_:open:)`): a `.ndaw` opens as a project, audio imports onto track 0. That is how import and open were verified — the open/save panels belong to a system XPC service that automation cannot click.

New, Open and Quit all prompt before discarding unsaved work (`confirmDiscardingChanges`). Autosave is a safety net, not a save.

## Monitor listen buttons

The monitor station's listen buttons are **cycles, not exclusive modes**, ported from
the old UI's `monitorStationButtonChanged:`. The engine keeps a listen-mode string
("LR"/"L"/"R"/"M"/"S"), a mono flag, and independent L/R phase inverts;
`normalizeMonitorStationProjectState` keeps them consistent. The bridge owns the state
machine (`nc_monitor_cycle_stereo` / `_cycle_mono` / `_toggle_mid_side` / `_cycle_phase`)
because it manipulates the project model:
- **Stereo** cycles Stereo → Left-only → Right-only (Mid in M/S).
- **Mono** cycles Mono → L-into-both → R-into-both (Side in M/S).
- **M/S** toggles Mid/Side monitoring.
- **Ø** cycles phase Off → ØL → ØR → ØLR.

The earlier `ListenMode` enum modelled these as four exclusive segments, which did not
match the engine and dropped phase and the L/R solos entirely.

## Keyboard shortcuts must match on key code, not characters

This machine types Korean. With a Korean input source active, `charactersIgnoringModifiers` for the Z key is not `"z"`, so any shortcut compared against a character silently stops working — including SwiftUI's `.keyboardShortcut("z")`. Match `NSEvent.keyCode` instead (Z is 6). The old UI already knew this: all 308 of its `NSMenuItem`s carry an empty `keyEquivalent` and every shortcut runs through an `NSEvent` monitor.

Menu items still declare their shortcuts so they are discoverable; the `NSEvent` monitor in `EngineController` is what actually delivers all of them (⌘Z, ⌘⇧Z, ⌘N, ⌘O, ⌘S, ⌘⇧S, ⌘I).

The **spacebar** (key code 49, no modifier) toggles play/stop through the same monitor. It is consumed (`return nil`) so it does not also click whatever button holds focus. The `firstResponder is NSTextView` guard at the top keeps it typing a space in a search field.

**Note for verification:** the computer-use tool cannot press Z here. It maps the character `"z"` to a key code using the active layout, which under Korean input yields key code 0 (the A key). Shortcut behaviour has to be checked by hand.

## Bounce

`nc_bounce_to_wav` renders the engine's document and blocks — about 25x realtime with no plug-ins, so a three-minute song would freeze the UI for seconds. The app instead serializes the document (`nc_project_serialize`) and renders that snapshot on a background task through `nc_bounce_snapshot_to_wav`, which touches no shared state. `project_io_smoke` proves the two paths are sample-for-sample identical; if they ever diverge, a background export would be quietly lying.

## The timeline scrolls itself

Lanes scroll under a fixed ruler, so the view owns the offset rather than sitting in
an NSScrollView — a scroll view would carry the ruler and the marker strip away with
the lanes. `laneTop()` accumulates the offset; everything that hit-tests or draws a
lane goes through it.

## Selection and range editing

Two selections, and they do different work:

- **Clip selection** (`selectedClipIds`) — shift-click adds, a marquee drag from empty
  lane space sweeps. Every edit over it goes through one `nc_clip_*_many` call so it
  records **one** undo step, and so the move delta is clamped once against the earliest
  clip: pushing a selection into zero stops it instead of piling the clips together.
- **Range** — the loop range doubles as the edit range, dragged along the top 12 pt of
  the ruler. `nc_range_*` slices clips at the range edges; clearing 2–3 s out of a
  ten-second clip leaves the head and the tail playing. It is not an edit, so it records
  no history.

Fades and clip gain stay single-clip: their handles hide when more than one clip is
selected, rather than offering a grab that would move the selection instead.

**Clip drag has two refinements** (`TimelineView.mouseDown`/`mouseDragged`):
- **Axis-lock** — a clip's horizontal position is pinned to its start time until the
  cursor leaves a small dead zone (`axisLockThreshold`), so a straight up/down lane
  move does not smear it sideways. Crossing the zone re-baselines the grab so it
  continues from the clip rather than jumping. Lane is resolved from the cursor Y at
  drop, as before.
- **Option-drag = copy** — holding ⌥ on mouse-down duplicates the clip in place
  (`onBeginCopyDrag` → `duplicateClipForDrag`, which `nc_clip_duplicate`s then moves
  the copy onto the original's start) and drags the copy, leaving the original put.

**Loop range handles** — with a range set, the top strip carries a triangle grab at
each edge (`.rangingEdgeStart`/`.rangingEdgeEnd`) and a middle grab to slide the whole
range (`.movingRange`), the Pro-Tools loop bar. An empty strip still sweeps a fresh
range. None of these record history.

## Solo blink

When any track is soloed, the silenced tracks' **Mute** buttons blink, the Pro-Tools
tell that they are held down by the solo rather than off. `EngineController.soloBlinkOn`
is a ~1 Hz phase toggled from the poll tick (only while `anyTrackSoloed`, and only
published on change); `MixerView` pulses a strip's M fill when
`anyTrackSoloed && !track.solo`.

## Talkback key

The monitor **Talk** button is a console talkback key, not a toggle: momentary while
held (and it pulls **Dim** in with it, restoring Dim's prior state on release),
double-click to latch on, a click on a latched key releases it. SwiftUI press gestures
do not fire for a static press inside the dock's `ScrollView`, so it is a self-drawing
`NSView` (`TalkbackKeyView`) reading `mouseDown`/`mouseUp`/`clickCount` directly.
`EngineController.setTalkbackEngaged` remembers Dim's state across the press.

## Automation

The renderer honours **two** parameters and no others: a track's `volumeAutomation`
and its `"track.pan"` lane. Both go through `mixTimelineFrame`, which the realtime
mixer and the offline bounce share — so an automation curve is heard, not just drawn.
Anything else written into `automationLanes` is stored by the project and ignored by
the sound, which is why `nc_automation_parameter_supported` refuses it rather than
letting the UI draw a curve that does nothing.

A lane folds out from the "A" chip in its timeline header; the parameter name below
it is also the parameter picker. Clicking empty space adds a point, dragging moves
one (continuous — the view commits the gesture), double-clicking removes it.

## VST3 parameters at load

A freshly instantiated plug-in has two halves: the **controller** is initialised to
whatever it means by "no preset loaded", and the **processor component** is not told
about it. `Vst3RealtimeProcessor::prepare` now reads the controller once and sends
its values in with the first process block; project-stored values override them.

It has to be `controller->getParamNormalized(id)`, **not**
`ParameterInfo::defaultNormalizedValue` — FabFilter declares 0 there and then
initialises the controller to 0.7. Before this, every insert processed with every
parameter at zero: a 1 kHz tone through FabFilter Micro came out at 0.0001 of its
level, and FabFilter One as an instrument made no sound at all.

`vst3_initial_parameters_smoke` guards it by measuring the tone. A test that read the
parameters back would have agreed with the bug.

## MIDI

`midiRegions` go into the render plan verbatim — unlike audio clips there is no
playlist to rebuild. Region times are seconds on the timeline; note times are beats
from the region's start.

A note only makes a sound if its track carries an **instrument plug-in**: the
renderer turns notes into VST3 events and mixes whatever the instrument returns. A
region on a bare midi track is silent by design. `nc_track_set_instrument` is what
loads one.

Both the realtime path and the offline bounce render instruments through the same
`renderProjectAudioBlockWithStateAndMeters`, so a MIDI part that plays also exports.

Double-click an instrument lane to make a region, double-click the region to open the
piano roll under the timeline. In the roll a click adds a note, a drag moves it, the
right edge resizes it, a double-click deletes it. A click that lands just short of an
existing note does **not** stack a second note on top of it.

The roll draws all 128 MIDI pitches, not a comfortable middle range: transposing an
octave up used to walk a note off the top of the view, where it kept playing unseen.
It scrolls to follow the part, but only when the part's **pitch range changes** —
scrolling on every update would drag the view back from wherever the user left it.

Region tools (quantize, transpose, humanize, duplicate, split) sit in the roll's
header and each records one undo step for the whole region. Humanize takes a seed, so
the same call twice gives the same answer; a random result could not be tested.

A MIDI region is selected by clicking it, and then Delete, ⌘D and B act on it instead
of on the clip selection — the two selections are exclusive, so one Delete key always
has exactly one target.

The roll draws all 128 MIDI pitches, not a comfortable middle range: transposing an
octave up used to walk a note off the top of the view, where it kept playing unseen.
It scrolls to follow the part, but only when the part's **pitch range changes** —
scrolling on every update would drag the view back from wherever the user left it.

Region tools (quantize, transpose, humanize, duplicate, split) sit in the roll's
header and each records one undo step for the whole region. Humanize takes a seed, so
the same call twice gives the same answer; a random result could not be tested.

A MIDI region is selected by clicking it, and then Delete, ⌘D and B act on it instead
of on the clip selection — the two selections are exclusive, so one Delete key always
has exactly one target.

The velocity lane is pinned under the roll rather than scrolling with the keyboard —
inside the scroll view it sat 730 pt down and was never on screen. Dragging a bar is
continuous, like a fader: `nc_midi_note_set_velocity` records nothing and the view
commits the gesture.

An instrument dropped on an instrument track from the plug-in browser fills its
instrument slot rather than an insert, and clicking its chip opens its editor — the
editor host addresses the instrument slot as insert index `-1`.

An insert's parameters are pushed into the live chain (`updateTrackVst3Parameter`);
an instrument's live in the render plan, so a knob turn there reconciles the project
instead. There is no fine-grained push for instruments.

## Master inserts

A chain of its own, applied to the mix on its way out. The engine keeps it in
`project.masterInserts` — **not** on the Master track, whose `inserts` stay empty.
The plug-in browser and the editor host address it with the sentinel track id `-1`
(`EngineController.masterInsertTargetId`), the way the instrument slot uses insert
index `-1`. `addMasterVst3Insert` refuses a second copy of the same plug-in; that is
the engine's rule.

## There is no recording

The transport's round button is **not** a take recorder. `nc_engine_set_recording` →
`setTransportRecordingActive` only flips the monitor path for record-armed tracks;
its own status message says so ("Tape record monitor path active"). Nothing in the
engine writes captured audio to disk, and the old UI never did either.

Input samples do reach the engine: the CoreAudio input **AudioQueue** hands them to
`NeuracoustDspEngine::pushInputMonitorInterleaved`, which drops them unless input
monitoring or talkback is on, and then only into a mutex-guarded monitor buffer.

The transport buttons carry Pro-Tools-style right-click menus: **Play** picks the
playback mode (일반 / 루프 재생 — the loop is the existing loop toggle, so the two
always agree), **Record** picks the record mode (새 테이크 / 루프 / 펀치 레코딩,
stored in `EngineController.recordMode`). Loop record turns the loop on; punch reads
its range from the loop range. The record modes are staged configuration only — the
menu says so — because there is no capture engine yet:

Real recording is therefore engine work, not UI work, and it is not a small change:
the input arrives on an AudioQueue that is separate from the output AudioUnit's
render callback, so captured frames carry no sample-accurate relationship to the
playhead. Punch-in that lands where the picture says it does needs input on the
render clock first.

## Markers

Nothing in the audio path reads them. They already existed and nothing showed them:
`AudioImportAnalysis` writes section markers ("Intro", "Verse", …) on every import,
so an imported project has markers before the user makes any.

⌘M drops one at the playhead. In the ruler, below the range strip: drag a flag to
move it (continuous — one undo step), double-click to delete it, shift-click to set
the edit range to the stretch between the markers on either side.

The engine addresses a marker by *time within a tolerance*, not by index — the list
re-sorts on every move. A drag therefore has to pass the marker's current time each
frame, not the time the drag started from.

## Waveforms

Peaks are cached per path at a **fixed sample resolution** — 256 samples per peak,
~5 ms — not a fixed bucket count, so their number scales with the file's length. A
fixed 2048 buckets smeared a long clip; at 256 samples/peak a 5-minute file has
~55,000 peaks and stays crisp at any zoom. `nc_waveform_peak_count` gives the count
(analyzing on first call); `nc_waveform_peaks` copies that many.

A clip plays a window of the file — `sourceOffsetSeconds` to
`sourceOffsetSeconds + durationSeconds` — so the timeline maps each on-screen column
into the window, not the file, and takes the **min/max over the peaks that column
covers** so a transient is never skipped between samples. Drawing the file across the
clip made every trimmed or split clip show audio it would never play.

`nc_waveform_duration_seconds` returns 0 until the file's peaks have been read; the
view needs it to do the mapping at all.

## DSP core allocation

The engine reserves performance cores for its realtime DSP as a QoS hint applied to
the render thread at `start()` — `AudioEngineSettings.performanceCoreIsolationEnabled`
and `.requestedPerformanceCoreCount`, fed from the project's
`appleSiliconCoreIsolationEnabled` / `requestedDspCoreCount` (default 4, clamped 1–16,
floored at 4 while isolation is on). The bridge wires these in `buildEngineSettings`;
`nc_dsp_set_core_count` / `nc_dsp_set_core_isolation` change the project value and
**restart the audio engine** to apply, since the hint is only read at start — a brief
dropout, the way a buffer-size change is. The dock's Remote Core / DSP section carries
the isolation toggle and a − / count / + stepper.

The external DSP core count (the networked DSP Manager / NuclustDspManager) is a
separate reserve — `RemoteDspServerSettings.totalCoreHint`, persisted as the project's
`externalDspCoreCount` (default 4, clamped 1–16, **no** isolation floor). The bridge
feeds it through `buildRemoteDspSettings`, and `nc_dsp_set_external_core_count` re-applies
the monitor DSP path to push the new hint **live** (no audio restart, unlike the internal
reserve). It is only a hint: a connected node's own reported `core_count` wins inside
`makeRemoteDspCorePlan`, so the manager can set the count too and its report takes
precedence. The dock's Remote Core / DSP section carries both steppers — 내부 코어 격리
(internal) and 외부 DSP 코어 (external).

**Remote node address.** The engine only ever streamed to the hardcoded `studio.local`;
now the target node is the project's `remoteDspHost` (default `studio.local`), plumbed
into `buildRemoteDspSettings` (`settings.host = remoteDspHost`, `nodes` cleared so the
host is the effective target). `nc_dsp_set_remote_host` re-applies the monitor path live
(no restart); `nc_dsp_discover_remote_host` broadcast-probes the LAN. The dock's Remote
Core / DSP section has a 노드 field + 검색 button. **NDS is not a separate server** — it,
External, and remote_external all speak the same NART UDP protocol (audio on 20000,
status/discover on 20001) to `neuracoust_remote_core_server`, which runs on the node
(Windows / Intel Mac / Linux). Verified live: with the host set to a real node the DAW
opens a connected UDP socket to `<node>:20000` and the dock shows 원격 코어 연결됨 with
the round-trip. The stream only runs on monitor signal (a silent/empty block is skipped)
and reconfigures when the host changes.

## Monitor DSP path

The three sources — **내부 DSP**, **외부 DSP**, **NDS** — are **multi-select**, not
one-of-three. The engine has a single routing string, so the UI tracks the selected
set itself (`EngineController.dspSources`) and derives the mode: a lone source maps to
`internal` / `remote_external` / `nds`, any combination maps to `auto` (the engine's
use-everything-available mode). Tracking the set separately means clicking NDS lights
only NDS, not all three; the last selected source cannot be turned off. The engine
accepts `internal` / `external` / `nds` / `remote_external` / `auto` and falls back to
`internal` for anything else — the earlier two-way toggle sent `remote_internal`,
which silently resolved to internal, so External and NDS were never reachable.

## Transport mode icons

The Play and Record buttons carry a small corner **badge** that reflects their mode,
so the glyph changes as the mode does — designed in the Claude Design project
("Pro Tools 스타일 DAW 디자인", file `Transport Mode Icons.dc.html`) and implemented
in `TransportBar.transportKey(badge:)`. Play shows a green `repeat` badge while
`loopEnabled`; Record shows a red badge keyed to `recordMode` — none for 새 테이크,
`repeat` for 루프, `arrowtriangle.down.fill` for 펀치. All future UI glyphs go through
that design project first, then get reproduced here (the app uses SF Symbols, so the
spec names the symbol + overlay rather than shipping an SVG).

## Output device selection

Right-clicking either monitor output tab (스피커 / 헤드폰) opens a device menu:
**시스템 기본** (empty `outputDeviceId`, follows the OS default) plus every physical
device with output channels, enumerated via `enumerateAudioDevices()`. The choice is
`NCEngine.outputDeviceId`, wired into `buildEngineSettings` as `outputDeviceId` and
read only at `start()` — so `nc_set_output_device` **restarts the engine**, same
brief dropout as a core-count change. `nc_active_output_device_name` reports the
device the engine actually opened (`status().deviceName`), which is what the 시스템
기본 line shows; a specific pick carries its own checkmark. Bridge cache
(`outputDeviceCache`) is filtered to `outputChannels > 0` and refreshed on each count
query.

## Snapping

`snapProjectTime` **always** snaps — it has no "snap enabled" flag inside. The default project's timeline quantum is 0.1 s. Deciding whether to snap at all is the caller's job; `EngineController.snap` consults the transport's Snap toggle first.

## Undo granularity

Continuous gestures (fader, pan, monitor knob) must record **one** step, not one per frame. The bridge deliberately records no history for `nc_track_set_volume_db` / `nc_track_set_pan`; the view calls `nc_history_record_gesture` when the drag ends. Everything else records itself.

## Listen Room

The engine encodes and pushes audio (`ListenRoomSender`); it does not run the relay. The relay is a Python daemon from the sibling **Neuracoust Listen** project, bundled into the app by CMake from `NEURACOUST_LISTEN_SOURCE_DIR`.

- `start-relay.sh` is a **launcher**: it `nohup`s the daemon and exits 0 once the daemon answers `/api/stats`. The script exiting is normal — do not read it as the relay dying. A non-zero exit is the only failure signal; the log is at `/tmp/neuracoust-listen-relay.log`.
- Because the daemon is detached, the app reaps it explicitly (`pkill -f listen_relay.py`) on stop and on `willTerminate`. Without that it outlives the app. The same applies to the reverse ssh tunnel.
- Chat rides the relay's `/api/chat` endpoint: GET with `since=<lastId>` polled once a second, POST to send. `sender` is `studio` for us.
- The rest of the old UI's controls are ported: **음질** (quality) cycles
  PCM → Bal → High → Max, **지연** (latency) cycles Low → Std → Sync, **새 링크** (reset)
  regenerates the token via `nc_listen_reset_token` and restarts the relay so the old
  link is dropped, and **Ping** posts `{"sender":"studio", "text":"Studio is checking
  the Listen Room feed."}` — just `sendChat` reused. Quality and latency ride into the
  share URL, so changing them changes the link.
- The **share URL must point at a reachable address, not loopback.** The relay ingests
  on `127.0.0.1` but binds its HTTP/WebSocket to `0.0.0.0`, so a listener needs the
  machine's LAN IP. `listenRoomShareHost` finds it by asking the routing table which
  local address a socket bound for the gateway would get (a UDP `connect` that sends
  nothing) — that lands on the default-route interface, not whichever `getifaddrs`
  lists first (often a disconnected `169.254.` link-local one). Falls back to any
  private-range interface, then link-local, then loopback. An explicit `relayHost`
  (a tunnel hostname) is used as-is.
- **Copy and QR hand out the external invite link, not the LAN one.** The LAN URL
  (`nc_listen_public_share_url`, a 192.168 address) only works on the same network;
  the shareable link is the external page plus the token, built by
  `nc_listen_external_share_url` exactly as the old UI did: a base URL — env
  `NEURACOUST_LISTEN_EXTERNAL_URL`, then `~/.neuracoust/listen_external_url`, then the
  `https://neuracoust.tplinkdns.com/listen/index.html?external=1` default — with
  `external=1&profile=external&session&quality&latency&transport&connect&token`
  appended (reserved keys the base already carries are dropped so they never double).
- The QR invite is `CIQRCodeGenerator` (Core Image, no dependency), popped over from
  the Listen Room's QR button. It encodes the external link, so it reaches a listener
  anywhere the DDNS host and the router's port-forward do.

## Open design questions

The design has no home for several existing features: MIDI piano roll, media pool, AI assistant, diagnostic log, chord/lyric lanes, the 4-format ruler, the edit-mode pad, the routing matrix, and the settings dialogs. Decision deferred: build the new shell first, place these once it runs.

The design also *changes* structure — the mixer moves from a floating `NSPanel` to an Edit/Mix tab view, and the monitor station from a floating panel to a fixed 392px right dock.
