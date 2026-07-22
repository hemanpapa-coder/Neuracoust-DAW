# BlackHole 제거 → Core Audio 프로세스 탭 (핸드오프)

목표: 레퍼런스 모니터링(다른 앱 소리를 DAW 모니터로 듣기)이 지금은 **BlackHole 루프백을 입력으로
캡처**한다. 이걸 **Core Audio 프로세스 탭**(macOS 14.4+)으로 바꿔 드라이버 없이 다른 앱 오디오를
직접 탭한다. 부수 효과: BlackHole 의존 제거 + "BlackHole이 시스템 기본 출력" 웨지 함정 소멸.

## ✅ De-risk 완료 — 프로세스 탭이 이 기계(macOS 26)에서 동작함
`docs/process-tap-derisk.swift` 실행으로 확인. **정확히 이 레시피라야 IOProc가 발화한다:**

1. `CATapDescription(stereoGlobalTapButExcludeProcesses: [<우리 process AudioObjectID>])`
   - `.isPrivate = true`, `.muteBehavior = .unmuted`(탭해도 원래 앱은 계속 재생)
   - 우리 자신을 exclude해야 피드백 루프 안 생김(빈 배열=전부 탭, 테스트용).
2. `AudioHardwareCreateProcessTap(desc, &tapID)` → status=0. **권한 프롬프트 안 뜸**(로컬 서명 앱).
   tap UID = `desc.uuid.uuidString`.
3. 애그리게이트 디바이스로 탭을 읽는다:
   ```
   kAudioAggregateDeviceUIDKey, kAudioAggregateDeviceIsPrivateKey: 1 (★필수 — 0으로 하면 SR=0/start 실패),
   kAudioAggregateDeviceTapAutoStartKey: 1,
   kAudioAggregateDeviceTapListKey: [[ kAudioSubTapUIDKey: tapUID ]]
   ```
   `AudioHardwareCreateAggregateDevice(dict, &aggID)`.
4. **`AudioDeviceCreateIOProcIDWithBlock(&proc, aggID, <반드시 dispatch queue>, block)`**
   ★ queue=nil이면 IOProc가 **안 불린다**(blocks=0). 전용 큐를 주면 발화(blocks=280/3s, ch2/48kHz).
5. `AudioDeviceStart(aggID, proc)`. block의 `inInputData`에서 Float 샘플 읽어 모니터로.
6. 정리: `AudioDeviceStop`/`DestroyIOProcID`/`AudioHardwareDestroyAggregateDevice`/`AudioHardwareDestroyProcessTap`.

**함정 요약**: 애그리게이트는 **반드시 private**, IOProc는 **반드시 dispatch queue**.

## ⚠️ 남은 검증
de-risk에서 peak=0이었는데, 이는 그 순간 **시스템 기본 출력이 BlackHole 2ch(허공)**이라 `say`가
허공으로 가서일 가능성이 큼(파이프라인 자체는 blocks=280으로 확실히 돎). 실물 기본 출력 상태에서
peak>0 확인 필요. 포맷은 Float32 non-interleaved, ch=2 확인됨.

## 통합 계획 (다음 세션)
현재 캡처 경로: `CoreAudioRealtimeEngine.mm`의 `startInputMonitorIfNeeded()`가
`AudioQueueNewInput`으로 입력 장치(BlackHole)를 열고, `inputCallback`→`pushInputMonitorInterleaved`.
listenSource 게이팅은 `refreshInputMonitorForCurrentProject()`.

1. 새 `ProcessTapCapture` 컴포넌트: 위 레시피로 탭+애그리게이트+IOProc, block에서
   `dspEngine_.pushInputMonitorInterleaved(samples, frames, 2)` 호출(기존 모니터 믹스 경로 그대로 재사용).
2. **우리 process 제외**: pid→AudioObjectID 변환(`kAudioHardwarePropertyTranslatePIDToProcessObject`)해서
   `stereoGlobalTapButExcludeProcesses:[me]`. (안 하면 우리 모니터 출력을 다시 탭 → 피드백.)
3. listenSource 켜지면 BlackHole AudioQueue 대신 이 탭을 start, 꺼지면 stop. BlackHole 입력 코드 제거.
4. UI: 모니터 독의 "BlackHole" 소스 버튼 → "다른 앱(프로세스 탭)"으로 리라벨. 우클릭 장치 선택 제거.
5. 권한: 프로세스 탭은 오디오 캡처 TCC가 필요할 수 있음(로컬 서명에선 프롬프트 없이 됨을 확인, 배포 시
   Info.plist `NSAudioCaptureUsageDescription`/entitlement 검토).

관련: [[dw-blackhole-default-output-trap]], [[dw-coreaudiod-wedge-hang]], [[dw-latency-architecture]]
