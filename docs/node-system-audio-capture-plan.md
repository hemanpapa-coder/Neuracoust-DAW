# Node System-Audio Capture & Mic Return — Technical Plan (#2)

Status: PLAN (no code yet). Author: overnight autonomous session, 2026-07-21.
Goal owner: user wants to stop relying on Waves **SoundGrid/DigiGrid** on the Intel Mac and drive
everything from **our own DSP/node stack** instead.

## 1. What the user asked for

> "로컬 네트워크 내 다른 컴퓨터(윈도우/맥)에 우리 노드 앱을 설치 → 그 컴퓨터에서 나오는 **시스템 사운드**를
> 이쪽으로 가져와서 **모니터 스테이션**에서 모니터링. 컴퓨터는 **선택적**으로. 그리고 이 맥미니의 **마이크를 그쪽으로
> 보낼** 수 있어야 (음성 명령 등에 활용). 지금 DigiGrid/SoundGrid를 연탄맥에 설치해 쓰던 걸 우리 DSP 중심으로.
> **Master/Slave** 개념으로 작동해도 좋음. 가장 효율적인 방법을 연구."

Three capabilities:
1. **Remote → here (audio):** capture the *whole machine's* output (browser, YouTube, a game, any app)
   on a networked PC/Mac and stream it to this DAW's Monitor Station.
2. **Here → remote (mic):** send the Mac mini's microphone to the selected node (talk-back / voice).
3. **Selection + roles:** pick which computer; Master (this DAW) ↔ Slave (node agent).

## 2. What already exists in this repo (reuse, don't reinvent)

- **NART UDP transport** — audio on **20000**, status/discover on **20001**, spoken by
  `neuracoust_remote_core_server` running on the node (Windows / Intel Mac / Linux). Verified live:
  the DAW opens a connected UDP socket to `<node>:20000` and the dock shows "원격 코어 연결됨" + round-trip.
  (See CLAUDE.md "Remote node address" and memory `dw-remote-dsp-nodes`.)
- **`ListenRoomSender`** — already encodes and pushes audio out over UDP (Listen Room). Same shape of
  problem (capture → encode → UDP → decode → mix), reusable as a codebase reference.
- **Process-tap capture on macOS** — de-risked and integrated: the "다른 앱" reference monitor captures
  another *app's* output via CoreAudio process tap + a private aggregate device (memory
  `dw-blackhole-process-tap`, `dw-reference-tap-first-press-silent`). The **local** half of "capture
  system audio" is already solved for macOS; the node needs the same for its own OS.
- **Monitor DSP source routing** — the engine already accepts monitor sources (internal / external /
  NDS / remote_external / auto) and the dock tracks a multi-select source set
  (`EngineController.dspSources`). A remote node's audio is just another monitor source to fold in.
- **Discovery** — `nc_dsp_discover_remote_host` broadcast-probes the LAN; `remoteDspHost` selects the
  node. Extend, don't replace.

**Net:** the transport, discovery, encode/decode, and macOS capture primitives all exist. The new work
is (a) a **node agent** that captures its OS's *system* output and plays back a returned mic stream,
and (b) wiring that agent's audio as a Monitor Station source + a mic-send path.

## 3. How the industry does it (survey, to fuse the best parts)

| Solution | Capture mechanism | Transport | Clock/latency | Notes for us |
|---|---|---|---|---|
| **Waves SoundGrid / DigiGrid** | proprietary driver, SoundGrid ASIO/CoreAudio | proprietary L2 Ethernet (not routable), SoundGrid protocol | hardware-clocked, ~1–2 ms | what we're replacing; needs their DSP server + driver. Overkill + lock-in. |
| **Dante (Audinate)** | virtual soundcard / hardware | IP (L3-routable), PTP clock | PTP-synced, sub-ms | gold standard but licensed; PTP is heavy. Good ideas: **PTP-style clock**, channel discovery. |
| **AES67 / Ravenna** | ASIO/CoreAudio | RTP + PTP | PTP | open standard; RTP payloads worth mimicking. |
| **VBAN (Voicemeeter)** | app captures device | plain UDP, tiny header | resampler absorbs drift | closest to us: **dead-simple UDP + a drift resampler**. Great model for "easy". |
| **NDI (audio)** | SDK captures | TCP/UDP, mDNS discovery | receiver-side buffer | mDNS discovery + auto-find is very user-friendly. |
| **Rogue Amoeba Loopback / Audio Hijack** | CoreAudio taps/HAL plugin | local only | n/a | best-in-class *system capture UX* on mac — model the capture-source picker after it. |
| **Steinberg VST Connect / Source-Connect** | app | Opus over UDP, jitter buffer | adaptive buffer | **Opus codec + adaptive jitter buffer** is the right call for LAN-with-jitter + optional WAN. |

**Fusion (our design):** VBAN's simplicity (plain UDP we already have) + NDI's mDNS auto-discovery +
Steinberg's Opus + adaptive jitter buffer + Dante's clock idea reduced to a **software drift
resampler** (no PTP hardware). Rogue-Amoeba-grade capture *picker* UX. All on our existing NART ports.

## 4. Proposed architecture

```
  ┌─────────────────────────── This Mac mini (MASTER = the DAW) ───────────────────────────┐
  │  Monitor Station                                                                        │
  │    source picker: [내부DSP][외부DSP][NDS][노드 오디오 ▾ (선택된 컴퓨터)]                 │
  │        └── decode(Opus) → jitter buffer → drift-resample → fold into monitor mix         │
  │  Mic send:  Mac mini mic → encode(Opus) → UDP :20000 → node                              │
  │        (reuses the existing process-tap / input-monitor capture on THIS side)            │
  └───────────────▲───────────────────────────────────────────────┬─────────────────────────┘
                  │ node→master: system audio (Opus/UDP :20000)    │ master→node: mic (Opus/UDP)
                  │ discovery/control (:20001)                      ▼
  ┌───────────────┴──────────── Networked PC/Mac (SLAVE = node agent) ───────────────────────┐
  │  neuracoust-node-agent  (extend neuracoust_remote_core_server)                            │
  │    ├── System capture:  Windows → WASAPI **loopback**;  macOS → CoreAudio **process-tap   │
  │    │                    + private aggregate** (same as our mac side);  Linux → PipeWire   │
  │    │                    monitor / PulseAudio .monitor source                              │
  │    ├── encode(Opus) → UDP :20000 → master                                                 │
  │    └── Mic playback:   receive master mic (Opus) → play to node's default output OR a      │
  │                        chosen device (so a person at the node hears the studio talk)       │
  └──────────────────────────────────────────────────────────────────────────────────────────┘
```

### Roles
- **Master** = this DAW. Initiates discovery, selects a node, pulls its system audio, pushes mic.
- **Slave** = the node agent. Advertises itself (mDNS + NART :20001 reply), captures on request,
  plays back the returned mic. Stateless-ish; the master drives.
- Multiple nodes may exist; the picker lists them; one active at a time in v1 (multi later).

## 5. System-audio capture, per OS (the only genuinely new low-level work)

- **Windows:** **WASAPI loopback** (`IAudioClient` on the render endpoint with
  `AUDCLNT_STREAMFLAGS_LOOPBACK`). Captures the full mix of the default output, no driver install,
  no admin. This is the standard, lowest-friction path (what OBS/Discord use). Format-convert to
  48 kHz f32 stereo.
- **macOS (node is a Mac):** the **CoreAudio process-tap + private aggregate device** we already
  ship on the master side (`dw-blackhole-process-tap`). Lift that code into the agent. No BlackHole.
- **Linux (node is Linux, e.g. the DSP appliance):** **PipeWire** `monitor` stream or PulseAudio
  `<sink>.monitor` source. Trivial, already how Linux screen-records audio.

All three normalize to **48 kHz / f32 / stereo** before encode, so the wire format is OS-agnostic.

## 6. Transport, codec, clock

- **Codec:** **Opus** (48 kHz, ~128–256 kbps stereo, 2.5–10 ms frames). Already a dependency
  candidate; low-latency, resilient, WAN-capable. PCM fallback for LAN (like Listen Room's 음질 cycle).
- **Ports:** reuse **20000 (audio), 20001 (control/discover)**. One more message type on 20001:
  `CAPTURE_START {mode: system|mic, rate, chans, codec}` / `CAPTURE_STOP` / `NODE_INFO {os, devices}`.
- **Jitter buffer + drift resampler:** receiver keeps an **adaptive jitter buffer** (target ~2–3
  packets, grows under loss) and a **fractional resampler** that nudges playback rate to track the
  sender's clock (VBAN/Dante idea in software — no PTP). This is what prevents the periodic
  clicks/drift SoundGrid solved in hardware.
- **Security:** LAN-only by default; a shared token (like the Listen Room token) if exposed to WAN.

## 7. Mic send (here → node)

- Source: the Mac mini mic via the **existing input-monitor capture** (the same AudioQueue/tap path
  that already feeds talkback and input monitoring — `pushInputMonitorInterleaved`, memory
  `dw-talkback-mic-channel`). Tap it → Opus → UDP :20000 → node.
- Node side: decode → play to its default (or chosen) output device. This is a person-at-the-node
  monitor of the studio, and the substrate for "voice command to that machine".
- Gate it behind a **talk/hold** control in the dock (reuse the Talkback key semantics).

## 8. UI (Monitor Station — keep it dead simple)

- Add **"노드 오디오"** to the source picker (next to 내부/외부/NDS). Clicking it:
  1. shows a small menu of discovered nodes (name, OS, round-trip) + "검색";
  2. on pick → `CAPTURE_START system` to that node → its system audio folds into the monitor mix.
- A **"🎙 → 노드"** toggle (talk to node) next to it for the mic send (momentary + latch, like Talk).
- Everything else (level, dim, A/B, EQ) already applies because the node is just another source.
- Design goes through the "Pro Tools 스타일 DAW 디자인" project first (per project rule), then here.

## 9. Phased implementation

1. **Agent skeleton** — extend `neuracoust_remote_core_server` with `NODE_INFO`, `CAPTURE_START/STOP`;
   Windows WASAPI-loopback capture → Opus → :20000. (Biggest new piece; do Windows first — that's the
   machine the user runs.)
2. **Master ingest** — decode Opus + adaptive jitter buffer + drift resampler → new monitor source
   `node`; wire the dock picker. Prove: hear the node's browser audio in the monitor.
3. **Mic send** — tap the local mic → Opus → node; node plays it back. Prove: talk from studio → heard
   at the node.
4. **macOS + Linux capture** in the agent (reuse our process-tap; PipeWire monitor).
5. **Polish** — mDNS auto-discovery, token for WAN, multi-node, PCM/quality cycle, packaging/installers.

## 10. Risks & decisions to confirm with the user

- **Opus dependency**: adds a small lib. OK? (Alternative: keep PCM-only for LAN, simplest, higher
  bandwidth — fine on a wired LAN.)
- **Agent packaging**: Windows installer + macOS pkg + the existing Linux appliance. The Windows agent
  is new surface area (signing, autostart).
- **Latency target**: monitoring another PC's audio is not sample-accurate to our transport — it's a
  *reference monitor*, so ~15–40 ms is fine. Confirm that's acceptable (it is for the stated use).
- **Clock**: software drift-resampler is enough for reference monitoring; we are explicitly NOT
  building PTP/Dante-grade sync.

## 11. Bottom line

We already own the transport (NART UDP), discovery, encode/decode reference, and the macOS capture.
The **new work is a node agent that loopback-captures its OS output (WASAPI/process-tap/PipeWire) and
plays back a returned mic**, plus folding that stream in as a Monitor Station source with an adaptive
jitter buffer + drift resampler. This replaces SoundGrid/DigiGrid with our own stack, stays on the
ports we already use, and the UX is one extra source button + one talk button.
