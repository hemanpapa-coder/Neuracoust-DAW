# 기존 UI 계약서 (Legacy UI Contract)

출처: `../DAW/src/app/macos/DawWindowController.mm` (50,602줄) + 엔진 헤더
작성: 2026-07-10, 새 UI를 만들기 전에 버려질 파일에서 추출한 사실들.

> **이 문서의 목적**: `DawWindowController.mm` 을 삭제해도 잃으면 안 되는 것을 기록한다.
> 새 UI는 §1 의 엔진 계약을 지키고, §2 의 로직을 이식하고, §3 의 화면을 재현해야 한다.

---

## §1. 엔진 API 계약

### 한 줄 요약
`ProjectDocument` 를 UI가 소유 → `EditOperations` 자유 함수로 변경 →
`updateProject()` 시도 후 실패 시에만 `loadProject()` 폴백 →
화면은 `AudioEngineStatus status()` 스냅샷 하나를 ~30fps 폴링해서 그린다.

### UI가 실제로 include 하는 헤더
```
core/DawState.h                 core/Localization.h
audio/AudioInputRecorder.h      audio/MidiInputRecorder.h
audio/MixMath.h                 audio/OfflineBounce.h
audio/ProjectAudioRenderer.h    audio/RealtimeAudioEngine.h
audio/RemoteDspPluginCatalog.h  audio/WavFile.h
plugins/PluginScanner.h         plugins/Vst3HostFoundation.h
plugins/Vst3ModuleRuntime.h     plugins/Vst3SdkAdapter.h
plugins/Vst3RealtimeBridgeProtocol.h
project/AudioImportAnalysis.h   project/EditOperations.h
project/ProjectDocument.h       project/ProjectMediaPool.h
project/StemMagicBridge.h       project/TimelineExport.h
```

**UI가 절대 안 건드리는 엔진 내부** — 새 UI도 몰라도 된다:
`MixerGraph.h`, `NeuracoustDspEngine.h`, `Vst3RealtimeBridge.h`.
(`NeuracoustDspEngine` 은 `RealtimeAudioEngine` 뒤의 private Impl,
 realtime VST3 브리지는 엔진이 내부에서 스폰한다.)

### 컨트롤러가 값으로 소유하는 엔진 객체
```cpp
DawState            state_;          // 앱/설정 상태 미러
ProjectDocument     project_;        // 편집 모델 (UI 쪽 진실의 원천)
RealtimeAudioEngine audioEngine_;    // 실시간 재생 엔진
AudioInputRecorder  inputRecorder_;
MidiInputRecorder   midiInputRecorder_;
```
`RealtimeAudioEngine` 과 두 recorder 는 non-copyable PIMPL.
UI 가 `project_` 를 엔진으로 **밀어넣기만** 하고, 엔진은 `project_` 를 되돌려 쓰지 않는다.

### RealtimeAudioEngine — 유일하게 중요한 경계

**트랜스포트 (MUTATE)**
| 메서드 | 시그니처 |
|---|---|
| `start` | `bool start(const AudioEngineSettings& = {})` |
| `stop` | `void stop()` |
| `setTransportRunning` | `void setTransportRunning(bool)` |
| `setTransportRecordingActive` | `void setTransportRecordingActive(bool)` |
| `seek` | `void seek(double seconds)` |
| `rewind` | `void rewind()` |

**프로젝트 푸시 (MUTATE) — 핵심 데이터 흐름**
| 메서드 | 시그니처 | 비고 |
|---|---|---|
| `loadProject` | `bool loadProject(const ProjectDocument&, std::string& error)` | 그래프 전체 재구축 |
| `updateProject` | `bool updateProject(const ProjectDocument&, std::string& error)` | 증분, 그래프 재사용 |

> ⚠️ **반드시 지킬 패턴** (`reloadRealtimeProjectKeepingPlayhead:`, 원본 42605–42668):
> 편집 후 `normalizeProjectRouting(project_)` → `updateProject()` 먼저 시도 →
> false 일 때만 `loadProject()` 로 폴백. 재로드 경로에서는 `seek` 로 플레이헤드를 복원하고
> `setTransportRunning` 상태도 되돌린다.
> 이것이 CLAUDE.md 의 "매 편집마다 그래프를 통째로 재구축하지 말 것" 계약이다.

**세밀 실시간 뮤테이터** (핫 컨트롤에서 프로젝트 전체 푸시를 피하기 위함) — 모두 `bool` 반환, 트랙 **이름 문자열** 또는 슬롯 인덱스로 지정:
- `updateTrackMix(trackName, volumeDb, pan)`
- `updateTrackRealtimeState(trackName, recordArmed, inputMonitoring, muted, solo)`
- `updateTrackPlaybackState(trackName, muted, solo)`
- `updateTrackSendSlot(trackName, sendIndex, const TrackSendState&)`
- `updateTrackInsertBypassState(trackName, insertIndex, bypassed)`
- `updateMasterInsertBypassState(insertIndex, bypassed)`
- `updateClipGain(clipId, gainDb)` / `updateClipFades(clipId, fadeIn, fadeOut)`
- `updateTrackVst3Parameter(trackName, insertIndex, parameterId, displayName, normalizedValue)`
- `updateMasterVst3Parameter(insertIndex, parameterId, displayName, normalizedValue)`
- `updateMonitorSpeakerVst3Parameter(speakerSlot, insertIndex, parameterId, displayName, normalizedValue)`

**모니터 / 메트로놈 / 테스트톤 / 리슨룸 (MUTATE)**
- `setMonitorDspModules(const std::vector<MonitorDspModule>&, bool enabled)` — 원본에서 22회 사용
- `setMonitorDspPathMode(mode, const RemoteDspServerSettings&)`
- `setMonitorStationControls(...)`
- `setMetronomeEnabled(enabled, tempoBpm, tempoMap, tsNum, tsDen, groove, swing, tsMap, subdivision)`
- `setTestToneEnabled(bool)` / `setListenRoomSettings(const ListenRoomSettings&)` / `setPhysicalInputAccessAllowed(bool)`

**라이브 MIDI (MUTATE)**
- `queueLiveMidiEvents(trackName, const std::vector<Vst3MidiEvent>&)` — 미터 타이머에서 armed 인스트루먼트 트랙으로 주입.

**`status()` — 유일한 읽기 동사** (원본에서 57회)
```cpp
AudioEngineStatus status() const;
```
평평한 대형 스냅샷 구조체. UI가 표시하는 실행 중 상태는 **전부** 여기서 나온다:
`running`, `transportRunning`, `playbackSeconds`, `outputPeakLeft/Right`, `phaseCorrelation`,
`spectrumLow/Mid/High`, 트랙별 미터 병렬 배열(`trackMeterNames`, `trackPeakLeft/Right`),
인서트별 미터/출력 파라미터 배열, 레이턴시·지연보상, 실시간 텔레메트리
(`realtimeAverageWakeJitterUs`, `realtimeMaxRenderDurationUs`, `realtimeLateWakeCount`,
`realtimeCallbackCount`), 원격 DSP 텔레메트리, `deviceName`, `dspEngineName`, `message`.

> **새 UI의 실시간 뷰 상태는 이 구조체 하나에 바인딩하면 된다.**

### 비동기/실시간 상태 전달 방식 — **콜백 없음. 폴링이다.**

엔진 → UI 방향의 콜백, KVO, 노티피케이션, 델리게이트가 **전혀 없다.**
엔진은 원자적으로 갱신되는 스냅샷 getter 만 노출하고, UI 가 메인 런루프의 `NSTimer` 로 폴링한다.

```objc
engineMeterTimer_ = [NSTimer scheduledTimerWithTimeInterval:(1.0/30.0)   // 30 fps
    target:self selector:@selector(engineMeterTick:) userInfo:nil repeats:YES];
[[NSRunLoop mainRunLoop] addTimer:engineMeterTimer_ forMode:NSRunLoopCommonModes];
remoteDspProbeTimer_     = [NSTimer scheduledTimerWithTimeInterval:5.0 ...];
listenRoomChatPollTimer_ = [NSTimer scheduledTimerWithTimeInterval:1.0 ...];
```

`engineMeterTick:` (원본 43634–43748) 이 심장박동:
1. `cachedEngineStatus_ = audioEngine_.status();` — **1/24 초 유효 창**으로 캐시해
   한 프레임 안의 여러 UI 리더가 스냅샷 하나를 공유한다.
2. `midiInputRecorder_.consumePendingEvents()` 를 비워 `queueLiveMidiEvents(...)` 로 전달.
3. 스냅샷에서 미터·플레이헤드·트랜스포트 시간·믹서 값을 밀어넣는다.
4. **플레이헤드는 폴링 사이를 벽시계로 예측**하고
   (`transportWallClockStartSeconds_`, `transportWallClockBaseSeconds_`),
   드리프트가 **0.18초**를 넘을 때만 `status.playbackSeconds` 로 재동기화한다.
   → 이걸 빼면 플레이헤드가 30Hz 계단으로 튄다. 새 UI도 반드시 재현할 것.

새 UI는 자체 렌더/폴링 루프(`CVDisplayLink` 또는 `DispatchSourceTimer`)를 만들어야 한다.

### ProjectDocument + EditOperations — 편집 모델과 변경 동사

`ProjectDocument` 는 대형 POD 집합체: 프로젝트 설정, `tracks`, `clips`, `mediaSources`,
`clipDefinitions`, **`trackPlaylists` (렌더의 진실의 원천)**, `midiRegions`, `markers`,
`masterInserts`, `monitorModules`, 템포/박자 맵, 비디오, 리슨룸/모니터스테이션 설정.
중첩 구조체는 `DawState.h` (`TrackState`, `ClipState`, `TrackInsertSlot`,
`InstrumentSlotState`, `TrackSendState`, `MarkerState`, `InsertState`, 오토메이션) 와
`ProjectDocument.h` (MIDI/미디어/플레이리스트) 에 있다.

**변경은 엔진을 통하지 않는다.** `EditOperations.h` 의 약 200개 자유 함수
(`bool op(ProjectDocument& project, ...)`) 로 `project_` 를 제자리 변경한 뒤 위의 reconcile 로 재푸시.

- 클립: `moveClip`, `splitClip`, `trimClipStart/End`, `duplicateClip`, `deleteClip`,
  `setClipGainDb`, `setClipFades`, `setClipMuted`, `copyClipRange`/`cutClipRange`/`pasteClipRange`, `glueClipRange`
- 트랙: `addAudioTrack`/`addInstrumentTrack`/`addFolderTrack`/`addVcaTrack`/`addMidiTrack`,
  `setTrackVolumeDb`/`setTrackPan`/`setTrackMuted`/`setTrackSolo`/`setTrackRecordArmed`/`setTrackInputMonitoring`,
  `renameTrack`, `moveTrack`, `deleteTrack`, 인서트/샌드 슬롯, 오토메이션 포인트
- 플레이리스트: `createTrackPlaylist`, `activateTrackPlaylist`, `duplicateActiveTrackPlaylist`,
  `rebuildProjectClipsFromActivePlaylists`
- 마스터 인서트: `addMasterVst3Insert`, `toggleMasterVst3InsertBypass`, `moveMasterInsert`
- 상시 호출 정규화기: `normalizeProjectRouting` (14회), `normalizeProjectEditModel`,
  `rebuildProjectClipsFromActivePlaylists`
- 타임라인 헬퍼: `projectMusicalGridLines`, `snapProjectTime`, `projectTimecodeString`,
  `projectTempoAtSeconds`, `projectTimelineQuantumSeconds`

> ✅ **CLAUDE.md 가 경고했던 `trackPlaylists` ↔ `project.clips` 정합성은 엔진 안에 있다.**
> `copyPlaylistPlacementToActivePlaylist` (`EditOperations.cpp:4674`). UI를 버려도 안 잃는다.

### 지속성 (모두 엔진 밖 순수 함수, `ProjectDocument.h`)
`serializeProject` / `serializeProjectForPath` / `deserializeProject` / `deserializeProjectForPath` /
`saveProjectFileWithBackup` / `writeProjectAutosaveFile` / `loadProjectAutosaveFile` /
`collectProjectMedia` / `defaultProject` / `analyzeProjectHealth` / `summarizeProjectHealth`

### 레코더 (폴링, PIMPL, 자체 스레드 소유)
- `AudioInputRecorder`: `start(outputPath, sampleRate, channels, inputDeviceId, bitDepth)`,
  `stop(std::string& error)`, `status() → RecordingStatus{recording, durationSeconds, outputPath, message}`
- `MidiInputRecorder`: `availableInputs()/availableOutputs()`, `start(preferredInputId)`,
  `stop(vector<RecordedMidiEvent>&, error)`, `consumePendingEvents()`,
  `status() → MidiInputRecordingStatus{...}`

둘 다 `RealtimeAudioEngine` 과 독립. UI 가 결과를 엔진(라이브 MIDI)과 `project_`
(`appendRecordedTakeClip`, `appendRecordedMidiTakeRegion`) 로 배선한다.

### 플러그인 스캔 (두 갈래)
- `PluginScanner.h`: `scanKnownPluginLocations() → vector<PluginCandidate>`,
  `filterPluginCandidates`, `pluginCandidateMatchesCriteria`,
  `sortPluginCandidatesForDisplay`, `pluginCandidateFilterOptions`, `describeInstalledPluginCandidate`
- `Vst3HostFoundation.h`: `scanVst3PluginBundles(Vst3ScanMode)`,
  `sortVst3PluginDescriptorsForDisplay`, `describeVst3PluginBundle`,
  `resolveVst3PluginDescriptorForInsert(...)`, `readVst3ModuleClasses(descriptor)`, `vst3HostCapabilities()`

### 미디어 풀 / 오프라인 렌더 (엔진 밖 헬퍼)
- `ProjectMediaPool.h`: `buildProjectMediaPoolSummary`, `removeMediaSourceFromProject`,
  `relinkMediaSource`, `deleteUnusedMediaSources`
- `OfflineBounce.h`: `bounceProjectToWav(project, outputPath[, BounceOptions])` — 바운스는
  `audioEngine_` 이 아니라 오프라인 렌더러를 `project_` 위에서 직접 돌린다.

### ⚠️ 엔진 API를 우회하는 지점 (재작성 위험, 심각도 순)

1. **VST3 에디터 호스트가 UI 안에 in-process 로 박혀 있다 — 가장 큼.**
   원본 3274줄에 `class Vst3EditorHostSupport final : public Steinberg::Vst::IHostApplication`
   (raw Steinberg SDK 매크로 `FUNKNOWN_CTOR`, `IHostApplication` 사용).
   플러그인 **오디오 처리**는 엔진의 아웃오브프로세스 브리지가 하지만,
   **에디터 창과 파라미터 I/O 는 UI 소유의 SDK 통합**이다. 엔진 API가 전혀 안 덮는다.
   관련: `nativeVst3EditorHostPath()` (750), `Vst3EditorHostSupport*` (3393/3645).

2. **UI 쪽 VST3 SDK 프로빙.** `probeVst3ProcessorWithSdk(...)` (`Vst3SdkAdapter.h`),
   `readVst3ModuleClasses`, `describeVst3PluginBundle`, `scanVst3PluginBundles` 를
   엔진에 묻지 않고 UI가 직접 호출. 플러그인 발견/검증은 오늘날 UI 책임이다.

3. **오프라인 렌더러 직접 호출.** 원본 33336 → `printRecordedTakeThroughTrackDsp(project_, ...)`
   (`ProjectAudioRenderer.h`). `activeVst3MasterInsertCount` / `activeVst3TrackInsertCount`
   (50353–50374), `bounceProjectToWav` 도 `project_` 위에서 직접.

4. **엔진 상태 미러가 둘 — 어긋날 수 있다.** `DawState state_` 와 `ProjectDocument project_` 가
   따로 있고, `monitorDspEnabled`, `monitorModules`, DSP 경로 모드 같은 필드가 **양쪽에 존재**해
   `setMonitorDspModules` / `setMonitorDspPathMode` 호출마다 손으로 동기화한다.
   엔진에 "이 상태를 적용" 단일 진입점이 없어 새 UI가 이 부담을 물려받는다.

### 스레딩 규칙
- **엔진 공개 API 는 전부 메인 스레드에서만 호출된다.** `start`, `stop`, `loadProject`,
  `updateProject`, 모든 `update*`/`set*`, `status()` — 전부 UI 액션 핸들러와 메인 런루프
  `NSTimer` 에서 호출. `dispatch_get_global_queue` 에서 `audioEngine_` 을 만지는 코드는 없다.
  새 UI도 엔진 호출을 메인 액터에 묶을 것.
- **`status()` 는 realtime-safe / lock-light.** 오디오 스레드가 갱신하는 필드는 `std::atomic`
  (`NeuracoustDspEngine.h` 186–198). 실제 그래프 상태는 `mutex_`, `inputMonitorMutex_` 뒤에
  오디오 스레드가 소유하며 UI 는 보지 못한다. **UI 는 어떤 락도 쥔 채 엔진을 호출하면 안 된다.**
- **오디오 스레드 소유물**: `ProjectAudioRenderState`, 인서트 체인, 원격 DSP 스트림,
  아웃오브프로세스 VST3 브리지 — 전부 `NeuracoustDspEngine::Impl` 안 `mutex_` 뒤.
  `updateProject`/`loadProject` 는 느린 그래프 작업을 오디오 스레드 밖에서 하되
  렌더 콜백을 오래 막으면 안 된다 (우선순위 역전 → 드롭아웃).
- **레코더**는 자체 캡처 스레드를 소유. UI 는 메인에서 `start`/`stop`/`status`/`consumePendingEvents` 만.
- **라이브 MIDI 주입** (`queueLiveMidiEvents`) 이 UI가 직접 하는 유일한 메인→오디오 스레드 전달.
  30fps 틱마다 유계 배치로 1회.

---

## §2. UI 파일에 갇힌 비-뷰 로직 (버리면 잃는 것)

> **먼저 안심할 것:** 무거운 모델 변경 알고리즘은 **이미 엔진에 있다** —
> `pasteClip`, `copyClipRange`, `cutClipRange`, `pasteClipRange`,
> `copyPlaylistPlacementToActivePlaylist`, `snapProjectTime`, `quantizeClipStartsInRange`,
> `moveTrack`, `deleteTrack`, `splitVideoClip`, `serializeProject`, `deserializeProject`,
> `saveProjectFileWithBackup`, `writeProjectAutosaveFile`, `normalizeProjectRouting`
> 는 전부 `EditOperations.cpp` / `ProjectDocument.cpp` 소속.
>
> 갇힌 것은 그 함수들을 **구동하는 오케스트레이션** — 스택/상태/정책/수명주기다.

### 1. 실행취소·다시실행 + dirty 추적 + 자동저장 트리거 — 🔴 치명적, 엔진에 없음
- 멤버: 원본 19454–19460
  (`undoSnapshots_`, `redoSnapshots_`, `undoStepNames_`, `redoStepNames_`,
   `savedProjectSnapshot_`, `currentProjectSnapshot_`, `pendingUndoStepName_`)
- 핵심: **`setProjectDirty:` (50542–50583)** — 실행취소 모델 전체.
  매 편집마다 프로젝트를 재직렬화해 `currentProjectSnapshot_` 과 비교하고,
  이전 스냅샷을 단계 이름과 함께 undo 스택에 push, **최대 100개로 캡**, redo 스택 클리어,
  그리고 **자동저장의 유일한 트리거** (`writeProjectAutosaveFile`, 50574).
- 적용: `undoClicked:` / `redoClicked:` (35448–35498), `restoreProjectSnapshot:` (~35419)
- 단계 이름: `undoStepNameForEditMode()` (1221–1268), `pendingUndoStepName_` (44261–45554)
- 디바운스: `scheduleProjectDirtySoon` / `deferredProjectDirtyTimerFired:` (50524–50540),
  `flushDeferredProjectDirty` (~35440)
- **행선지: 엔진 또는 app/document 레이어.** 엔진에는 `serializeProject` 만 있고
  undo 스택도, dirty 플래그도, 자동저장 트리거도 없다.
  이식하지 않으면 실행취소·자동저장·"저장 안 됨" 감지가 통째로 사라진다.

### 2. DSP 실행 모드 정책 — 🟠 높음, 엔진에 없음
- 위치: 자유 함수 1270–1531 — `normalizedInsertDspExecutionMode` (1270),
  `defaultPluginInsertDspExecutionMode` (1337–1345),
  `normalizeThirdPartyTrackInsertToNative` (1347–1380),
  `effectiveInsertDspModeBadge` (1330), 인서트별 모드 수집 (1416/1465/1513/1531)
- 각 인서트를 `native` / `internal` / `remote_internal` / `external` 중 무엇으로 돌릴지를
  `appleSiliconCoreIsolationEnabled`, 전역 DSP 활성화, 플러그인 타입으로 결정.
  낡은 서버 기반 모드를 native 로 되돌리는 정규화 포함.
- **행선지: 엔진.** 모드 **문자열**은 프로젝트 모델에 저장되지만 **판단 규칙**은 여기에만 있다.
  잃으면 코어 격리 / 아웃오브프로세스 플러그인 라우팅이 깨진다.

### 3. 플러그인 에디터 프로세스 수명주기 + 파라미터 브리지 — ✅ 이식 완료
> `src/app/swift/PluginEditor.swift` + 브리지의 `nc_track_set_vst3_parameter` /
> `nc_track_set_instrument_vst3_parameter` / `nc_track_insert_observer`.
> 트랙 인서트와 인스트루먼트 슬롯(슬롯 인덱스 `-1`)을 옮겼다. 마스터 인서트와 모니터
> 스피커 슬롯은 **브리지에도 UI에도 없어서** 아직 열 것이 없고, Waves RS124 미러링도 없다.
> 프로토콜(실측): 호스트→앱 `READY`, `PARAM <id> <normalized>`, `HOST_STAGE`, `HOST_ERROR`;
> 앱→호스트 `PARAM_SET <id> <normalized>`. 호스트는 `PARAM_SET` 으로 받은 값을
> `lastPolledParameterValues_` 에 기록하므로 복원한 값이 `PARAM` 으로 되돌아오지 않는다(핑퐁 없음).

<details><summary>원본 위치</summary>

- 멤버: `nativeVst3Editors_` (19479), `nativeVst3HostTasks_` (19481),
  `mp4RenderTask_`, `stemMagicTasks_`, relay tasks (19482–19493)
- 스폰/호스트 관리: AU 호스트 33754–33890, VST3 호스트 33898–34322
  (이미 떠 있는 에디터 중복 방지, `terminationHandler` 배선, stdin/stdout 파이프)
- 파라미터 동기화: `sendStoredNativeVst3ParametersToTask:` (34381–34460) —
  호스트 프로세스를 `project.tracks[].inserts[].parameters` / 인스트루먼트 슬롯 /
  마스터 인서트 / 모니터 스피커 슬롯으로 되돌리는 **인코딩된 인덱스 체계**
  (`nativeVst3IndexIsInstrumentSlot`, `instrumentSlotIndexFromNativeVst3Index`,
   모니터 슬롯은 `masterIndex <= -101` 인코딩);
  `applyNativeVst3ParameterEdit:` (35075–35180) — 편집된 파라미터를 프로젝트 모델에 upsert,
  Waves RS124 파라미터 미러링, `audioEngine_` 으로 푸시
- **행선지: app/controller 레이어 (엔진 측 헬퍼 동반).**
  파라미터 *저장소*는 엔진 모델이지만 프로세스 수명주기, 인덱스 인코딩, 크래시 복구,
  Waves 미러링은 여기에만 있다. 잃으면 에디터가 안 열리고 파라미터 편집이 저장되지 않는다.
</details>

### 4. 클립보드 버퍼 + 붙여넣기 정책 — 🟡 중상
- 상태: `clipClipboard_` (ClipState), `rangeClipboard_` (vector<ClipState>), 유효 플래그 (19369–19371)
- 핸들러: `copyClipClicked:` / `copyLoopRangeClicked:` (35860–35882), `cutLoopRangeClicked:` (35886),
  `cutClipClicked:` (36078), `pasteClipClicked:` (36138–36189)
- 알고리즘은 엔진 위임. 여기 있는 건 **정책**: 외부 오디오 파일 붙여넣기 폴백 (36141–36149),
  플레이헤드 스냅 붙여넣기 위치 (`snappedPlayheadSeconds`), 대상 트랙 결정, 범위 vs 단일 분기.
- **행선지: app/controller 레이어.**

### 5. 선택 모델 + 규칙 — 🟡 중
- 멤버: `selectedClipId_`, `selectedClipIds_` (순서 있는 집합), `selectedMidiRegionId_`,
  `selectedMidiNoteId_`, `selectedTrackName_`, `selectedTrackNames_` (7620–7625)
- 세터/규칙: `setSelectedClipIdValue:` (8314–8331), `setSelectedMidiRegionId:noteId:` (8333),
  `setSelectedTrackName(s):` (8359–8382), `selectedClipIdsSnapshot` (8355)
- 상호 배타 규칙: 클립 선택 시 MIDI 선택 해제, 그 역도 성립. 단일 선택이 다중 선택 집합과 동기화.
- **행선지: app/controller (얇게).** 대부분 재유도 가능한 뷰 상태지만 배타 규칙은 동작이다.
  참고: **범위 선택**(`editSelectionEnabled`, `editSelectionStartSeconds`)은 이미
  엔진 모델(`ProjectDocument.h:197`)에 있다. 객체/트랙 선택만 뷰 쪽.

### 6. 타임라인↔픽셀 변환, 줌·스크롤 — 🟢 대부분 뷰 전용
- `secondsAtX:visibleStart:visibleDuration:` (10501–10507),
  `visibleTimelineDurationSeconds` / `visibleTimelineStartSeconds` (8598–8615),
  follow 모드 자동 스크롤 (8604–8615), `snapSeconds:` (10509–10517, 엔진 `snapProjectTime` 래핑)
- 줌: `timelineZoomFactor_` 클램프 **0.02–16.0** (8477–8482),
  수직 스크롤 클램프/썸 계산 (8670–8715), 히트테스트 `clipAtPoint:` (11697) / `editModeAtPoint:` (11766)
- **버리고 새로 써도 안전.** 단 두 가지는 동작 스펙으로 베껴올 것:
  follow 모드의 "페이지 vs 연속" 스크롤 규칙 (8604–8615), 그리고
  `snapSeconds` 가 이미 엔진에 위임한다는 사실 (스냅을 재발명하지 말 것).

### 7. 프로젝트 열기/저장 + 자동저장 복구 — 🟡 중
- `openProjectAtPath:` (32874–32964) — 엔진 `deserializeProjectForPath` 로 로드 후
  **자동저장 복구 정책** (32895–32941): `projectAutosaveIsNewerThanProject` 면
  자동저장본을 제안/로드, 아니면 제거.
- `saveProjectToPath:` (32259–32285), `saveProjectForConfirmation` / `saveProjectAsClicked:` (32285–32316),
  닫기 확인 + "이게 기본 빈 프로젝트인가?" 검사 (29438–29458)
- **행선지: app/controller.** 파일 I/O 는 엔진, **복구 판단·다이얼로그·닫기 정책**은 여기.

### 8. 오디오 임포트 오케스트레이션 — 🟡 중
- `importAudioPaths:...` (30561–30700+), `chooseImportTargetTrackWithRequestedTrack:`,
  `temporaryImportAudioFilesDirectory()` (6062), `isSupportedImportAudioExtension` (6045)
- 파일별 정책: 비-WAV 를 `convertAudioFileToProjectWav` / `...TemporaryWav` 로 변환,
  프로젝트 "Audio Files" 미디어 폴더 vs 임시 디렉터리 복사, 샘플레이트/비트뎁스 변환 추적,
  음악적 분석, 대상 트랙 선택, 순차 클립 배치
- **행선지: app/controller.** 변환 프리미티브는 헬퍼/엔진, **워크플로와 미디어 폴더 정책**은 여기.

### 9. NSUserDefaults 뷰 설정 지속성 — 🟢 낮음~중
- 키: `kCurrentSettings*` 20개 (3851–3871) + 모니터/믹스 키 (3848, 19582–19586)
- 로드: init 안 인라인 (20672–20740). 저장: `saveCurrentSettingsClicked:` (20743+),
  글로벌 레인/룰러 (9040–9091)
- 타임라인 줌, follow 모드, 트랙 높이, 그리드 단위, 편집 모드, 오토메이션 해상도,
  재생 시작점, 코어 격리, DSP 코어 수, 룰러 포맷, 패널 표시/폭
- **행선지: app/controller.** 대부분 뷰 취향이지만 **`gridUnit`, `editMode`, `coreIsolation`,
  `dspCoreCount` 는 모델/오디오 동작에 영향** — 이 키들은 반드시 살릴 것.

### 10. 백그라운드 헬퍼 프로세스 — 🟢 낮음~중 (기능별)
- 스템 분리 (`stemMagicTasks_`, ~31391–31474), 리슨/스트리밍 릴레이 + 터널
  (`listenRelayTask_`, `listenExternalTunnelTask_`, 21438–21545),
  MP4/비디오 렌더 (`mp4RenderTask_`, 32044–32109),
  `afconvert` 바운스 전달 (5678–5701, `writeBounceDeliveryManifest` 5853)
- **행선지: app/controller.** 각각 자체 종료/오류 처리를 가진 NSTask 워크플로.
  해당 기능이 새 UI에 살아남는 경우에만 필요.

### 11. 트랜스포트/루프/메트로놈/프리롤 배선 — 🟢 대부분 뷰
루프 오버레이 그리기 (13174–13259, 뷰 전용). `metronomeChanged:` (48191) 과 `preRoll*` 핸들러는
`audioEngine_.setMetronomeEnabled(...)`, `project_.preRollSeconds` 를 감싸는 얇은 래퍼.
루프 지점(`loopStartSeconds`, `loopEndSeconds`, `loopEnabled`)은 엔진 모델 소속.
**숨은 상태 없음 — 버리고 새로 써도 안전.**

---

## §3. UI 표면 목록 (재현 대상)

### 구조
- `DawWindowController` 는 `NSWindowController`,
  `<NSWindowDelegate, NSSplitViewDelegate, NSMenuItemValidation>` 채택.
- 모든 UI 라벨이 **한국어**. XIB/Storyboard 없이 코드로 명령형 구축 (Auto Layout VFL + 수동 프레임).
- 루트: `buildInterface` (24196) → 단일 `root` NSView =
  **트랜스포트 패널 (상단, 72pt)** + **트랙 패널 (나머지)**.
  `mainSplitView_` 는 이제 `nil` — 좌측 인스펙터 스플릿은 제거되고
  트랙 인스펙터가 타임라인의 트랙 헤더 컬럼 안으로 접혀 들어갔다.
- 파일 안에 커스텀 클래스 **26개** 정의. 지배적인 뷰는 `ArrangementTimelineView`
  (구현 7618–17130, 약 9,500줄) — arrange/타임라인 전체 + 트랙별 헤더/인스펙터를 직접 그린다.
- 미터/노브/페이더/타임라인은 `engineMeterTimer_` 와 self-invalidation 으로 애니메이션.
- **모든 키보드 단축키는 `keyDown:` 오버라이드에서 처리.** `NSMenuItem` 308개 전부
  `keyEquivalent` 가 `@""`. `NSMenu` 79개는 전부 **동적 컨텍스트/팝업 메뉴**이며,
  앱 메뉴 바 자체는 이 파일이 아니라 `Main.mm` 에 있다.

### 커스텀 클래스 (뷰/컨트롤 24 + 헬퍼 2)

| 줄 (iface/impl) | 클래스 | 베이스 | 역할 |
|---|---|---|---|
| 139 / 146 | `NATalkbackButton` | NSButton | 모니터 톡백; 순간 누름(153/171) + 우클릭 래치(178) |
| 188 / 191 | `NATransportTimeField` | NSTextField | 편집 가능 트랜스포트 시간 표시 (mouseDown 198) |
| 217 / 220 | `NAHitTransparentView` | NSView | 통과 컨테이너 |
| 229 / 238 | `NADraggableToolIconButton` | NSButton | 드래그/도킹 가능 트랙 툴 아이콘 (keyDown 318, mouseDown 311) |
| 337 / 340 | `NAMixerCenteredTextFieldCell` | NSTextFieldCell | 믹서용 가운데 정렬 셀 |
| 370 / 378 | `NAAutoFadeTimeField` | NSTextField | 오토 페이드 시간 입력 (mouseDown 398, 우클릭 423) |
| 417 / 421 | `NAContextActionButton` | NSButton | 인서트/샌드 추가 "+" 버튼 (컨텍스트 메뉴) |
| 3010 / 3015 | `NAAutoFadeButton` | NSButton | **커스텀 드로잉** 페이드 버튼 (drawRect 3045, rightMouseDown 3075) |
| 6149 / 6152 | `NAFlippedPanelView` | NSView | 뒤집힌 좌표 패널 컨테이너 (행/스트립에 광범위 사용) |
| 6179 / 6185 | `NALevelMeterView` | NSView | **커스텀 레벨 미터** (drawRect 6255) |
| 6361 / 6364 | `NAMixerFaderScaleView` | NSView | **커스텀 dB 페이더 스케일** (drawRect 6379) |
| 6426 / 6443 | `NARemoteDspServerCardView` | NSView | **커스텀** 원격 DSP 서버 상태 카드 (drawRect 6800, mouseDown 6738) |
| 6936 / 6940 | `NAChannelStripPreviewView` | NSView | **커스텀** 채널 스트립 프리뷰 (drawRect 6988) |
| 7058 / 7062 | `NASpectrumMiniView` | NSView | **커스텀** 미니 스펙트럼 분석기 (drawRect 7091) |
| 7124 / 7135 | `NARotaryKnob` | NSControl | **커스텀 로터리 노브** (drawRect 7263, 드래그 7238/7248, 우클릭 7255) |
| 7370 / 7382 | `NAMetalTimelineBackdropRenderer` | NSObject | **CAMetalLayer / MTL** 타임라인 GPU 백드롭 (셰이더 파이프라인) |
| 7549 / 7618 | `ArrangementTimelineView` | NSView | **메인 커스텀 타임라인** (아래 참조) |
| 7592 / 7598 | `NABlockActionTarget` | NSObject | 블록→셀렉터 트램폴린 |
| 17131 / 17137 | `NAMediaPoolRowButton` | NSButton `<NSDraggingSource>` | 드래그 가능 미디어 풀 행 (mouseDown 17167) |
| 17212 / 17219 | `NAMixTrackTitleButton` | NSButton | 믹서 스트립 타이틀 버튼 (mouseDown 17226) |
| 17259 / 17308 | `NAInsertSlotButton` | NSButton | **커스텀** 인서트/샌드/인스트루먼트 슬롯 (drawRect 17505, mouseDown 17438, 트래킹 17391) |
| 17652 / 17660 | `NASendBusPopupButton` | NSPopUpButton | **커스텀** 샌드 버스 팝업 (drawRect 17694, mouseDown 17669, 우클릭 17685) |
| 17761 / 17774 | `NAResettableSlider` | NSSlider | **커스텀 페이더/팬 슬라이더** (drawRect 17929), 더블클릭 리셋 |
| 18278 / 18283 | `NAMidiPianoRollView` | NSView | **커스텀 피아노 롤** (drawRect 18577, 전체 마우스 편집) |

### 3.1 트랜스포트 바 (상단 72pt)
`fillTransportPanel:` (29044–~30000). `NSAppearanceNameAqua`, `ncToolbarPanel` 배경.

- 트랜스포트 키 (`styleTransportKeyButton`, SF Symbol):
  Rewind `|<` (`rewindClicked:`), 이전 경계 `|◀` (`goToPreviousClipBoundaryClicked:`),
  셔틀 뒤 `◀◀`, 셔틀 앞 `▶▶`, 다음 경계 `▶|`,
  재생 `▶` (`playClicked:`), 정지 `■` (`stopClicked:`),
  녹음 `●` (`recordClicked:`, `rebuildRecordButtonMenu` 로 팝업 메뉴 부착)
- `가져오기` (`importWavClicked:`), `영상` (`importVideoClicked:`),
  `보기` (`showVideoPreviewClicked:`), `바운스` (`bounceClicked:`)
- 루프 토글 `L` (`loopButtonChanged:`), `시작` (`loopInClicked:`), `끝`,
  `selectionInField_` / `selectionOutField_` (편집 가능 NSTextField)
- 토글: 코어 격리 `D` (`coreIsolationChanged:`) + `dspCoreCountMenu_` (1/2/4/8),
  테스트톤 `T`, 메트로놈 `C`, 프리롤 `Pre` + `preRollField_`, 포스트롤 `Post` + `postRollField_`
- `playbackStartMenu_` (삽입/선택/처음/복귀), `tempoMenu_`
- 창 런처: `Mix`, `Mon`, `AI`, `?` (퀵헬프), `Log`
- **`NATransportTimeField transportTimeLabel_`** — 편집 가능 2줄
  (bars|beats + time·bpm·sig), 고정폭, 라운드 레이어, `transportTimeEdited:`
- 상태 라벨: `outputLevelLabel_` (Pk/RMS), `loopStatusLabel_`, `engineStatusLabel_`

### 3.2 트랙 패널 툴바
`fillTracksPanel:` (27382–~27650). VFL 배치.

- `NSPopUpButton` 로컬 메뉴 3개 (`localToolbarMenuChanged:`):
  - **편집** — 자르기(B) / 접합(G) / 페이드(F) / 구간선택(P) / 복사 / 붙이기
  - **기능** — 플러그인 브라우저 / 미디어 풀 / 스템 분리 / 템포 분석 / 코드 분석 /
    프로젝트 설정 / 오디오 드라이버 / 미디어 수집 / 배치 변환
  - **보기** — 트랙 높이 / 글로벌 레인 / 파형 확대 / 믹서 / 모니터 스테이션 / 빠른 도움말
- **드래그 가능 트랙 툴 아이콘** (`NADraggableToolIconButton`):
  `+` 오디오, `M` MIDI, `I` 인스트루먼트, `F+` 폴더, `Bus+` 버스 폴더, `VCA`,
  `Up`/`Dn` 이동, `Del`, `Ren`.
  → **타임라인 안에 떠 있는 서브뷰(5행 그리드)로 도킹**되며 드래그로 재배치
    (`dockedTrackToolIconMoved:`)
- 줌 `줌-`/`맞춤`/`줌+`, 높이 `H-`/`H+`, 높이 범위 전체/개별, 파형 `W-`/`W+`
- `trackSelectorMenu_`, `tempoMasterTrackMenu_`, `timelineFollowMenu_` (페이지/스크롤/고정),
  `editModeMenu_` (Slip/Grid/Shuffle/Spot),
  **`editModePad_`** (2×2 Pro Tools 스타일 SHUFFLE/SPOT/SLIP/GRID 패드),
  `gridUnitMenu_` (0.1s … 1/16 박),
  `automationResolutionMenu_` (smart/free/fine/smooth/grid/frame),
  `gridSnapControl_` (세그먼트 Grid/Beat)

### 3.3 타임라인 / Arrange — `ArrangementTimelineView` (7618–17130) ★ 최대 재구현 대상

단일 `NSView` 가 도킹된 툴 버튼을 서브뷰로 얹고 **나머지 전부를 직접 그린다.**

**`drawRect:` (11864–~14878, 약 3,000줄)** — 위에서 아래로:
- **룰러** + 디스클로저 삼각형 (11908), 동시 최대 4개 포맷 —
  Bars|Beats, Minutes|Seconds, Timecode (SMPTE), Samples (12344–12347);
  점선 루프/선택 경계, 템포/마커 포스트와 탭
- **그리드 선** (minor/major), `niceGridStep` (12361), 음악적 vs 선형
- **마커 레인**, **코드 이벤트 알약** (`drawChordEventTag` 12888),
  **가사 알약** (`drawLyricPill` 12987)
- **트랙별 레인 헤더 (인라인 인스펙터)**: 타입 알약 + 모드 알약 (`drawHeaderPill` 13551),
  오토메이션 표시/모드/타임베이스/일래스틱 모드 셀렉터 (`drawHeaderSelector` 13597),
  트랙 이름 (편집 가능 NSTextField 오버레이 `inspectorTrackNameField_`)
- **오디오 파형** (`WaveformCacheEntry` 로 캐시, `waveformForPath` 11806,
  `setWaveformGainScale` 로 게인 스케일)
- **클립** (`rectForClip` 10333) + **페이드 인/아웃 핸들과 곡선** (`drawFadeHandle` 14379),
  크로스페이드, 클립 게인 라인
- **오토메이션 레인** — 볼륨·팬 포인트와 선 (`drawAutomationPoints` 13832;
  히트테스트 `automationPointAtPoint` 11389 / `automationLineAtPoint` 11450)
- **MIDI 리전/노트** 인라인 렌더
- **비디오 참조 썸네일** (`thumbnailForVideoClip` 11204, `videoClipAtPoint` 11238)
- **플레이헤드**, **편집/루프 선택 범위**, 템포 마커
- **Metal 백드롭** 언더레이 (`NAMetalTimelineBackdropRenderer`,
  `installMetalTimelineBackdropIfAvailable` 7917)

**상호작용:**
- 마우스: `mouseDown:` (15209), `mouseDragged:` (16324), `mouseUp:` (16672),
  `mouseMoved:` (8172, 호버 커서), `mouseExited:` (8256),
  `scrollWheel:` (8281, 수평 스크롤 + 줌). 커스텀 커서 `cursorForEditToolHint` (8050).
  클립 이동/트림/페이드/분할, 마커·코드·가사 드래그, 오토메이션 포인트 드래그,
  템포 마커 드래그, 편집 선택 러버밴드,
  **커스텀 수직 스크롤바** (썸 사각형 8691, `setVerticalScrollOffsetFromThumbY` 8705)
- 드래그 앤 드롭 대상: `draggingEntered/Updated` (8985/8989), `performDragOperation:` (8993)
  — 미디어 풀 페이로드와 오디오 파일 수용
- 컨텍스트 메뉴: `menuForEvent:` (9178) — 색상 / 편집 / 오디오 처리 / 템포 동기화 /
  선택 구간 편집 / 클립 게인 / 페이드(곡선 서브메뉴) / 선택·로케이트.
  트랙 헤더 팝업 (`showTrackHeaderPopupMenuForHit:` 14971,
  `showTrackHeaderPlaylistMenuForTrackName:` 14909),
  룰러 표시 메뉴 (`showTimelineRulerDisplayMenuAtPoint:` 10800)
- 키보드: `keyDown:` (15988–16322)

### 3.4 트랙 인스펙터 (이제 트랙 헤더 안에 인라인)
레거시 인스펙터 행은 `fillTracksPanel:` (27654+) 에 남아 있으나 스플릿 페인은 사라짐
(`mainSplitView_ = nil`, 27776).
유지되는 참조: `inspectorTitleLabel_`, `inspectorTrackNameField_`,
`inspectorTrackFormatLabel_` (오디오/모노/스테레오/폴더/버스/MIDI/악기/마스터 알약),
인서트·샌드 추가 버튼, 플레이리스트 컨트롤 (`trackPlaylistMenu_`,
new/duplicate/rename/delete/promote/lane), IO 메뉴 (`trackInputMenu_`,
`trackOutputMenu_`, `trackControlMasterMenu_`).
갱신: `refreshSelectedTrackInspectorInline` (38770),
`refreshInspectorForTrackSelectionImmediately` (28609)

### 3.5 믹스 창 (플로팅 `NSPanel`, 1040×610)
`showMixWindowPanel:` (24025) → `rebuildMixWindowContent` (22306) → `fillMixPanel:` (24382).
채널 스트립의 수평 `NSScrollView`.

- 헤더: `RTG` 라우팅 매트릭스, `VIS` 표시 설정,
  6세그먼트 옵션 바 `IO / 인서트 / 샌드 / MST / NAR|MIN / 전체` (`mixOptionButtonChanged:`)
- **각 채널 스트립** (`NAFlippedPanelView`, ~24751–25700):
  이름/타이틀 버튼, 타입 라벨, 오토메이션 모드 버튼,
  **L/R `NALevelMeterView` + 게인 리덕션 미터**, **`NAMixerFaderScaleView` dB 스케일**,
  **`NAResettableSlider` 페이더**, **`NAResettableSlider` 팬**,
  `M` 뮤트 / `S` 솔로, 채널 포맷 버튼 (M/L/R/ST), DSP 정책 버튼,
  **인서트 슬롯 스택** (`NAInsertSlotButton`), **인스트루먼트 슬롯 스택**,
  **샌드 슬롯 스택** (각각 게인 `NAResettableSlider`),
  폴더/버스/VCA 용 폴더 셸. 마스터 스트립 + `NAChannelStripPreviewView`
- 메뉴: `showMixRouteMenu:` (22399), `showMixAutomationModeMenu:` (22615),
  `showMixTrackDspPolicyMenu:` (22672), `showMixMasterDspPolicyMenu:` (22871),
  `showMixTrackMenu:` (23051), `showMixSendSlotMenu:` (23789)
- 서브 패널: 라우팅 매트릭스 (720×520, 23397), 믹서 표시 설정 (560×520, 23599)

### 3.6 모니터 스테이션 (플로팅 `NSPanel`, 420×680)
`showMonitorStationPanel:` (24140), `fillMonitorPanel:` (28638),
`refreshMonitorStationInlineControls` (49377).

- 모니터 DSP 토글, 모듈별 토글 (`monitorModuleButtons_`),
  스피커/헤드폰 인서트 컨트롤 (scan/load/probe/add/clear/open/move/bypass/remove),
  `showMonitorSpeakerInsertMenu:` (48678)
- **`NARotaryKnob monitorVolumeKnob_`**, **`NALevelMeterView` monitorMeterLeft_/Right_**,
  **`NASpectrumMiniView monitorSpectrumView_`**,
  DSP 코어 미터 (`monitorDspCorePeakMeter_`, `dspCorePeakMeter_`, `dspCoreRmsMeter_`,
  `systemRiskPeakMeter_/RmsMeter_`), **`NARemoteDspServerCardView`**, `NATalkbackButton`
- 스테이션 버튼: Stereo / Mono / Phase / Dim / M-S / Mute
  (`monitorStationButtonChanged:` 태그 1–7)
- 사용자 설정 **전역 단축키** (`handleMonitorGlobalShortcutEvent:` 49159,
  `performMonitorShortcutCommand:` 49181 —
  `monitor.stereo/mono/phase/dim/ms/mute/talkback` + 모듈 토글, NSUserDefaults 저장)
- 모니터 DSP 패널 `showMonitorDspPanel:` (20879)

### 3.7 리슨 룸 (협업)
토글/링크복사/품질/레이턴시/리셋/핑/채팅 버튼 (19249–19255).
초대 팝오버 `showListenInvitePopover:` (21881), QR 패널 `listenInviteQrPanel_` (21831),
**채팅 패널** `showListenRoomChatPanel:` (22186; `NSTextView` + 입력 필드, 폴 타이머)

### 3.8 미디어 풀 (도킹 + 플로팅 `NSPanel`)
`showMediaPoolClicked:` (32319), 도크 뷰 `makeDockedMediaPoolView` (27216, 타임라인 아래 64pt),
`refreshMediaPoolPanel` (32507).
`NSSegmentedControl` 모드 + 필터, `NSSearchField`,
**`NAMediaPoolRowButton`** 드래그 가능 행의 `NSScrollView` (타임라인으로 드래그 소스), 요약 라벨

### 3.9 MIDI 피아노 롤 — `NAMidiPianoRollView` (플로팅 `NSPanel`, 780×430)
`showMidiEditorPanelForRegionId:` (20801), 패널 20808.

- **`drawRect:` (18577)**: 타이틀 바; 좌측 피아노 **건반** (58pt);
  비트/마디 선이 있는 노트 **그리드**; **노트 사각형**; **벨로시티 레인** (하단 82pt);
  드럼 vs 키 모드; 퀀텀 표시
- 상호작용: `mouseDown:` (18812, 노트 추가/선택), `rightMouseDown:` (18929),
  `mouseDragged:` (18978, 이동/리사이즈/마키), `mouseUp:` (19022),
  `scrollWheel:` (18338), `mouseMoved:` (18260)
- **`keyDown:` (19082)**: Delete = 노트/CC 삭제, `Q` 퀀타이즈, `T` +12 전조,
  `H` 휴머나이즈, `L` 리전 루프 토글

### 3.10 그 밖의 패널/다이얼로그
- **AI 어시스턴트** (플로팅 `NSPanel` 620×520, `showAiAssistantPanel:` 20894):
  `aiPromptField_`, `aiContextLabel_`, `aiAssistantStatusLabel_`,
  `aiAssistantResponseView_` (`NSTextView`)
- **비디오 프리뷰** (`showVideoPreviewClicked:` 30380, 패널 30272):
  `AVPlayer` + `AVPlayerLayer` (`videoPreviewLayer_`), 플레이헤드 추종 타이머
- **퀵 헬프** 오버레이 (`showQuickHelpPanelNearWindowCenter` 20533;
  마우스 이동 모니터로 Logic 스타일 문맥 도움말)
- **진단 로그** "DAW 진단 로그" (`showDiagnosticLogPanel:` 28996, `NSTextView`)
- **라이선스 상태** (20828), **오디오 설정** (20838), **프로젝트 설정** (20858),
  **번들 플러그인** (22270)
- **Stem Magic 진행률** (`NSProgressIndicator`, 31552)
- **VST3 빠른 컨트롤** (`showVst3ParameterPanelForPluginName:` 34473) +
  네이티브 VST3 에디터 호스트 창 (`NativeVst3EditorWindow`, `NSTask` 로 스폰)
- **범용 유틸리티 패널 팩토리** `showUtilityPanelWithTitle:size:fill:cleanup:` (20786)
- `NSAlert` / `NSSavePanel` / `NSOpenPanel` / `beginSheetModal` / `runModal` **약 165회**
  (확인, 임포트/익스포트, 프로젝트·WAV·비디오·EDL/FCPXML/AAF/OMF/MIDI·스템·MP4 저장/열기)

### 3.11 플러그인 / 인서트 메뉴 (동적 팝업)
`showMidiInsertMenu:` (39490), `showMasterInsertCompactMenu:` (39757),
`showInstrumentSlotMenu:` (39877),
`showTrackInsertPluginSearch:` (40104 — `vst3SearchField_`, `vst3BrandMenu_`,
`vst3CategoryMenu_`, `vst3PluginMenu_`),
`showTrackInsertSlotMenu:` (40224), `showTrackSoloModeMenu:` (42236).
갱신: `refreshVst3Menu` (49943), `refreshVst3BrandMenu…` (50153),
`refreshVst3CategoryMenu…` (50192), `refreshInsertMenu` (50228)

### 3.12 키보드 단축키

**타임라인 (`ArrangementTimelineView keyDown:` 15988) + 전역 트랜스포트 모니터**

| 키 | 동작 |
|---|---|
| Space | 재생/정지 토글 (전역은 `transportKeyMonitor_` 19730, 텍스트 필드에서는 억제) |
| `6` | 펀치 녹음 모드 토글 |
| ⌘Z / ⌘⇧Z | 실행취소 / 다시실행 |
| ⌘⇧/ 또는 `?` | 빠른 도움말 |
| ⌘A / ⌘⇧A | 전체 범위 선택 / 편집 선택 해제 |
| ⌘C / ⌘X / ⌘V / ⌘D | 복사 / 잘라내기 / 붙여넣기 / 복제 (범위 인식: 클립 vs 루프 범위) |
| `M` | 마커 추가 |
| `1`/`2`/`3`/`4` 또는 F1–F4 | 편집 모드 Shuffle / Slip / Spot / Grid |
| ⌘← / ⌘→ | 타임라인 줌 아웃 / 인 |
| ⌘⇧↓ / ⌘⇧↑ | 파형 줌 다운 / 업 |
| ⌃↓ / ⌃↑ | 글로벌 레인 높이 다운 / 업 |
| ⌘↓ / ⌘↑ | 트랙 높이 다운 / 업 |
| ↑ / ↓ (템포 마커 선택 시) | BPM ±1 |
| Tab / ⇧Tab | 다음 / 이전 클립 경계 (⌥ 로 선택 확장) |
| `A` / `S` | 플레이헤드로 클립 시작 / 끝 트림 |
| `F` | 페이드 / 크로스페이드 적용 (다중 클립 인식) |
| Delete / Backspace | 선택된 클립·MIDI 노트·리전·마커·코드·오토메이션 범위 삭제, 또는 마지막 빈 트랙 삭제 |
| `,` / `.` | 클립 앞으로 / 뒤로 넛지 |
| `P` (⌥) | 선택→클립 / 루프 재생→클립 |
| `B` | 플레이헤드에서 클립 분할 |
| `E` | 루프 범위 분리 |
| `G` (⇧) | 루프 범위 접합 / 인접·선택 클립 접합 |
| `D` (범위) | 루프 범위 복제 |
| `Q` | 루프 범위 퀀타이즈 |
| `X` (⇧) | 루프 범위 지우기 / 셔플 삭제 |

**피아노 롤 (`NAMidiPianoRollView keyDown:` 19082)**
Delete 노트/CC 삭제 · `Q` 퀀타이즈 · `T` +12 전조 · `H` 휴머나이즈 · `L` 리전 루프 토글

**모니터 전역 단축키** — 사용자 설정 (NSUserDefaults),
`monitor.stereo/mono/phase/dim/ms/mute/talkback` + 모듈 토글

> 메뉴 항목이 제목/툴팁에 단축키를 광고하지만 (자르기 (B), 접합 (G), 페이드 (F), 구간 선택 (P))
> 실제 처리는 전부 위의 `keyDown:` 핸들러. `keyEquivalent` 는 쓰지 않는다.

### 3.13 진짜 재구현이 필요한 커스텀 드로잉 뷰

1. **`ArrangementTimelineView`** (11864) — 룰러(4포맷), 그리드, 파형, 클립,
   페이드/크로스페이드, 오토메이션 레인, 마커/코드/가사, MIDI 노트, 비디오 썸네일,
   플레이헤드, 선택 범위, 인라인 트랙 헤더 인스펙터, 커스텀 수직 스크롤바
2. **`NAMidiPianoRollView`** (18577) — 건반, 노트 그리드, 노트, 벨로시티 레인, 드럼/키 모드
3. **`NALevelMeterView`** (6255) — 세그먼트(초록/노랑/빨강) 수직·수평 미터, 눈금,
   피크 홀드, 클립 표시, RMS+피크 듀얼 모드, 중력 릴리즈 밸리스틱
4. **`NARotaryKnob`** (7263) — 로터리 바디, 값 아크, 포인터, 글래스 노브, 원형 미터
5. **`NAResettableSlider`** (17929) — 커스텀 페이더/팬 트랙과 썸, 더블클릭 리셋
6. **`NAMixerFaderScaleView`** (6379) — dB 눈금
7. **`NASpectrumMiniView`** (7091) — FFT 스펙트럼 바
8. **`NAChannelStripPreviewView`** (6988) — 미니 스트립 프리뷰
9. **`NARemoteDspServerCardView`** (6800) — 서버 상태 카드
10. **`NAInsertSlotButton`** (17505) / **`NASendBusPopupButton`** (17694) — 커스텀 슬롯/팝업 크롬
11. **`NAAutoFadeButton`** (3045) / **`NADraggableToolIconButton`** — 커스텀 버튼 글리프
12. **`NAMetalTimelineBackdropRenderer`** (7382) —
    **CAMetalLayer + MTLRenderPipelineState** GPU 타임라인 백드롭 (Metal 셰이더 포팅 필요)

> 모든 색상은 테마 헬퍼에서 나온다 —
> `ncBackground`, `ncPanel`, `ncText`, `ncMutedText`, `ncAccent`, `ncToolbarPanel` 등.
> 새 UI는 이 토큰 집합을 재현해야 한다 (그리고 Claude Design 토큰으로 교체한다).

---

## §4. 이 문서에서 나오는 결론

### 가져올 것 (수정 없이)
`src/audio`, `src/core`, `src/project`, `src/plugins`, `src/license`, `src/nuclust`, `src/ai`
→ `neuracoust_daw_core` 정적 라이브러리. 엔진은 UI를 **전혀 참조하지 않는다** (역방향 의존 0건).
보조 실행 파일 `NeuracoustVst3EditorHost`, `NeuracoustAuEditorHost`, `NeuracoustDspManager` 도 유지.

### 버릴 것
`src/app/macos/DawWindowController.mm` (+ `.h`), `src/app/macos/Main.mm`
— 단, §2 의 1·2·3 번(실행취소/자동저장, DSP 모드 정책, 플러그인 에디터 수명주기)은
**먼저 새 레이어로 이식한 뒤에** 버릴 것.

### 새로 만들 레이어
1. **design tokens** — `ncBackground`/`ncPanel`/`ncAccent`… 를 Claude Design 값으로 대체
2. **app/controller** — 실행취소 스택, dirty/자동저장, 선택 모델, 클립보드 정책,
   임포트 워크플로, 프로젝트 열기/저장 정책, 플러그인 에디터 수명주기, NSUserDefaults
3. **view** — SwiftUI 셸 + `NSViewRepresentable` 로 감싼 무거운 것들
   (타임라인, 피아노 롤, 미터, 노브, 페이더, 플러그인 에디터 창)

### SwiftUI vs AppKit — 권고
**SwiftUI 껍데기 + 무거운 부분 AppKit/Metal 임베드.**
근거: VST3 에디터는 `NSView` 에 네이티브 창을 임베드해야 하고 (SDK 직접 사용),
타임라인은 이미 Metal 백드롭 + 3,000줄 `drawRect` 이며,
미터/노브/페이더는 30fps 재그리기가 필요하다.
반면 트랜스포트 바, 인스펙터, 믹서 스트립 레이아웃, 각종 패널/다이얼로그는
SwiftUI 가 디자인 재현에 압도적으로 빠르다.
