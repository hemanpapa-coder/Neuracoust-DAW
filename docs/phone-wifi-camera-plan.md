# Smartphone Wi‑Fi Cameras → Monitor Station / Listen Room — Plan (#6)

Status: PLAN (no code). 2026-07-21 overnight.
User: spare/idle **smartphones as Wi‑Fi cameras**. Two uses:
1. **Listen Room aid** — show your face, or the keyboard you're playing, to the remote listener.
   Adaptive quality: HD when bandwidth allows, auto-drop to "recognizable" low-res when not.
2. **Video-lecture / YouTube capture** — several phones as Wi‑Fi cameras, **capturing audio AND video**,
   recording **split onto this computer or node computers**. **Critical:** the files must import into
   **Final Cut with matching timecode** so multicam edit lines up.
Add as a **Monitor Station feature**.

## 1. Two very different requirements (split the design)

| | Listen Room camera | Multicam capture |
|---|---|---|
| latency | low (live-ish, WebRTC) | irrelevant — quality + sync matter |
| quality | adaptive, can be ugly | best available, constant |
| output | live preview to listener | files on disk (this Mac / nodes) |
| sync | not critical | **timecode-accurate for Final Cut** |

Do **not** force one pipeline to do both. Live preview = WebRTC; capture = record-to-file with timecode.

## 2. How the industry does it (survey)

- **EpocCam / Camo / iVCam / DroidCam** — phone → app → becomes a webcam over Wi‑Fi/USB. Live, adaptive.
  Model the *live* path after these (they use RTSP/WebRTC + adaptive bitrate).
- **Apple Continuity Camera** — zero-config phone-as-camera; the UX bar to beat for iPhones.
- **NDI / NDI|HX (phone apps: NDI Camera)** — phones stream NDI over Wi‑Fi; a receiver records. This is
  the pro multicam standard and is *exactly* the "several phones as cameras, record separately" ask.
- **Final Cut multicam / timecode** — FCP syncs multicam by **timecode or audio waveform**. The reliable
  path is **embedded timecode**; the fallback FCP already does well is **audio-waveform sync** (record a
  common audio reference on every phone and FCP aligns them). Audio-sync is far easier for us to
  guarantee than genlocked TC.

**Fusion:** live path = **WebRTC** (adaptive, low-latency, browser-native — no app install; the phone
just opens a URL). Capture path = **WebRTC-recorded or NDI-style file-per-phone**, and we guarantee FCP
sync by (a) stamping a **shared session start timecode** into each file's metadata and (b) mixing a
**common inaudible/quiet sync reference** (or the studio click) into every phone's audio track so FCP's
audio-sync locks them even if TC drifts.

## 3. Why this fits our stack

- The **Listen Room already runs a relay** (Python daemon, `/api/*`, WebSocket, share URL, QR invite).
  A phone joining as a **camera** is the same "open a URL on your phone" flow we already ship for
  listeners — extend the relay page with a **camera-publisher** mode (getUserMedia → WebRTC).
- The share URL / QR / external DDNS path exists (`nc_listen_external_share_url`), so a phone anywhere
  can join.
- Nodes exist (from #2) as extra recording targets.

## 4. Proposed design

### 4a. Live camera (Listen Room)
- Relay page gains a **"카메라로 참여"** mode: phone opens the QR/URL, grants camera, publishes a WebRTC
  video track. Adaptive bitrate is **built into WebRTC** (it drops resolution under congestion → exactly
  the "auto-lower to recognizable" the user wants, for free).
- Studio side: the Monitor Station shows the incoming phone preview(s) in a small strip; the listener's
  Listen Room page shows them too. Face / keyboard cams are just labeled tiles.

### 4b. Multicam capture (lecture/YouTube)
- Same join flow, but the phone runs in **record mode**: capture full-quality audio+video locally OR
  stream to a recorder (this Mac / a node) that writes one file per phone.
- **FCP sync guarantee (the important part):**
  - Stamp each file with a **shared session TC** (all phones told the same start instant over the relay,
    `MediaRecorder` chunks tagged; or write a sidecar with the offset).
  - Fold the **studio click / a quiet sync tone** into every phone's recorded audio → FCP audio-sync
    aligns them robustly. This is the pragmatic, reliable path (genlock/TC-over-IP on phones is not
    realistic).
  - Deliver as **.mov/.mp4 per phone + a FCPXML** that places them on a multicam timeline pre-aligned.
- Record targets: this Mac (default) or a node (offload), chosen per phone.

## 5. Phasing

1. **Live face/keyboard cam** in Listen Room (WebRTC publisher mode on the relay page + preview tiles).
   Smallest, highest daily value, adaptive-for-free.
2. **Single-phone record-to-file** with shared TC + sync tone; verify it drops into FCP aligned.
3. **Multi-phone** capture + FCPXML multicam export; node record targets.

## 6. Risks / decisions

- iPhone Safari WebRTC + getUserMedia works but has quirks (autoplay, HTTPS required — the DDNS host has
  TLS, good). Test on the user's actual phones early.
- **Timecode-accurate** across consumer phones is not truly genlocked; commit to **audio-waveform sync
  in FCP as the guarantee**, TC as a coarse aid. Confirm the user accepts audio-sync (FCP does it well).
- Storage/bandwidth for multi-phone HD — offload to nodes; make quality per-phone selectable.
