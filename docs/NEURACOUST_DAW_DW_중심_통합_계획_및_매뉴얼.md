# Neuracoust DAW: DW 중심 통합 계획 및 매뉴얼

작성 기준: 2026-07-12  
상태: 제품 방향 및 구현 기준 초안  
주 프로젝트: `/Volumes/Program Dev/DW`

## 1. 결정

새 DAW는 별도 프로젝트를 다시 만드는 방식이 아니라 **DW를 Neuracoust DAW의 주 제품으로 승격**하여 완성한다.

프로젝트별 역할은 다음과 같다.

| 위치 | 역할 |
|---|---|
| `DW` | 현재 개발 본체. 새 UI, 사용자 경험, 브리지, 제품 조립과 출시의 기준 |
| `DAW` | 검증된 C++ 오디오 엔진, 기존 기능, 과거 UI 동작의 참고 원본 |
| `DAW3` | Pro Tools, Nuendo, Pyramix, Bitwig, Studio One, Logic Pro 매뉴얼 연구 자료실 |

기존 `DAW`를 즉시 폐기하지 않는다. 엔진과 기능을 `DW`가 충분히 흡수하고 검증할 때까지 형제 프로젝트로 유지한다. 새 기능의 최종 사용자 표면과 제품 판단은 `DW`에서 결정한다.

## 2. 제품 목표

Neuracoust DAW는 Pro Tools의 세션·편집·믹싱 신뢰성을 중심축으로 삼고, 다음 장점을 하나의 일관된 작업 흐름으로 결합한다.

| 참고 DAW | 가져올 핵심 |
|---|---|
| Pro Tools | 세션 중심 구조, 빠른 편집, 플레이리스트, 클립 게인, 안정적인 믹서와 I/O |
| Nuendo | Control Room, 영상·포스트, 마커, ADR, 납품 및 다중 포맷 작업 |
| Pyramix | 고채널 녹음, 네트워크 오디오, 마스터링, 정밀 미터링과 신뢰성 |
| Bitwig | 유연한 모듈레이션, 장치 체인, 창작 실험, 비파괴적 변형 |
| Logic Pro | 음악가 친화 MIDI, Smart Tempo 계열 보조, 악보·패턴·세션 플레이어 경험 |
| Studio One | 드래그 중심의 빠른 흐름, 프로젝트/마스터링 연결, 간결한 납품 |

그러나 기능을 그대로 복제하지 않는다. Neuracoust DAW의 고유한 중심은 다음 세 가지다.

1. 녹음부터 편집, 믹스, 송출, 검수, 납품까지 하나의 세션에서 끝난다.
2. Native, Internal DSP, External DSP를 같은 세션과 자동화 체계에서 운용한다.
3. Master 출력과 청취용 Monitor/송출 처리를 명확히 분리한다.

## 3. DW에서 이미 확보한 기반

DW는 이미 다음 제품 기반을 갖고 있다.

- SwiftUI 기반 기본 셸과 AppKit/Metal 타임라인
- 검증된 C++ 오디오 엔진을 연결하는 C 브리지
- Edit/Mix 전환, Transport, Timeline, Mixer, Plugin Browser
- Monitor Dock과 Monitor Station 계열 제어
- Listen Room, 채팅, 송출 릴레이 기반
- 별도 프로세스 VST3 실행과 플러그인 편집기 호스트
- 오디오/MIDI 녹음, 프로젝트 입출력, Bounce 연결 기반
- Native/Internal/External DSP 실행 모델의 엔진 기반
- UI가 엔진 상태를 약 30Hz로 읽는 안정된 상태 갱신 계약
- 자동화, 마커, 스냅, 선택, 범위 편집, 파형 표시 기반
- 현재 30개 테스트로 구성된 회귀 검증 기반
- 디자인 토큰과 레거시 UI 계약 문서

따라서 DW의 다음 과제는 화면을 다시 설계하는 일이 아니라, 기존 기반을 **세션 전체에서 일관되게 연결하고 사용자에게 완성된 흐름으로 제공하는 일**이다.

## 4. 제품 구조

### 4.1 Edit

녹음과 편집의 중심 화면이다.

- Smart, Move, Select, Trim, Split, Fade, Zoom 도구
- 샘플, 시간, 박자, 프레임 단위 그리드
- 오디오와 MIDI 클립 편집
- 클립 게인과 페이드
- 플레이리스트/테이크 레인과 컴핑
- 트랙 및 클립 자동화
- 마커, 메모리 위치, 범위 선택
- 영상 트랙과 타임코드

### 4.2 Mix

라우팅과 음향 처리의 중심 화면이다.

- Audio, Instrument, Aux, Folder, VCA, Master 트랙
- Inserts, Sends, Pan, Mute, Solo, Record, Input Monitor
- Pre/Post Fader Send
- Sidechain과 다중 출력
- Plugin Delay Compensation
- Native/Internal/External DSP 상태 표시
- 트랙 순서와 라우팅을 반영하는 실제 믹서 그래프

### 4.3 Monitor / 송출

Master와 독립된 청취 및 전달 계층이다. DW의 상시 Monitor Dock을 제품의 핵심으로 사용한다.

- Speaker Set A/B/C
- Headphone/Cue 출력
- Stereo, Left, Right, Mono, M/S, Phase 확인
- Dim, Mute, Talkback
- Reference Level과 개별 스피커 보정
- Downmix와 Loudness 확인
- Listen Room 송출
- Monitor Print
- 송출 전용 Limiter/보호 처리

### 4.4 Browser

미디어와 플러그인을 빠르게 찾고 작업에 넣는 공간이다.

- 세션 미디어와 외부 파일
- VST3 플러그인과 Neuracoust 모듈
- 프리셋, 템플릿, 트랙 프리셋
- 드래그 앤 드롭 배치
- 형식, 제조사, 종류, 즐겨찾기 필터

### 4.5 Deliver

Bounce를 하나의 창이 아니라 반복 가능한 납품 작업으로 만든다.

- Mix, Stem, Selected Track, Clip, Monitor Print
- WAV/AIFF 및 필요한 납품 형식
- Sample Rate, Bit Depth, Dither
- Offline/Realtime 선택
- Loudness와 True Peak 검사
- 파일명 규칙과 버전
- Render Queue와 Render Manifest

### 4.6 Assistant

Assistant는 사용자의 명시적 명령을 돕는 비파괴 보조 계층이다.

- 세션 정리와 이름 제안
- 라우팅 및 납품 점검
- 마커/메모 작성 보조
- 오류 원인 설명
- 실행 전 변경 내역 미리보기
- Undo 가능한 명령만 수행

Assistant가 오디오 엔진의 실시간 스레드나 세션 파일을 임의로 변경해서는 안 된다.

## 5. 신호 흐름의 기준

```text
Input
  -> Record/Input Monitor
  -> Clip and Track Processing
  -> Track Inserts
  -> Track Fader/Pan
  -> Sends and Buses
  -> Aux/Group/VCA-controlled Mix
  -> Master Processing
  -> Master Print / Deliver

Master Output
  -> Monitor Source Select
  -> Monitor DSP
  -> Speaker/Headphone/Cue Routing
  -> Listen Room / Broadcast Protection
  -> Physical Outputs or Remote Listener
```

핵심 규칙은 **Master는 작품이고 Monitor는 듣는 방법**이라는 것이다.

- 스피커 보정, Dim, Talkback, 청취 Mono, M/S 확인은 일반 Master Bounce에 포함하지 않는다.
- 사용자가 명시적으로 Monitor Print를 선택한 경우에만 Monitor 체인을 파일에 포함한다.
- Listen Room은 Master 신호를 받아 별도 송출 보호와 지연·상태 관리를 적용한다.
- 출력 장치 변경과 Monitor 라우팅 변경은 클릭이나 팝을 만들지 않도록 안전한 블록 경계에서 적용한다.

## 6. DSP 실행 모델

모든 인서트는 다음 실행 위치 중 하나를 가진다.

| 모드 | 의미 |
|---|---|
| Native | 현재 컴퓨터에서 실행 |
| Internal DSP | DAW 내부 저지연 DSP 경로에서 실행 |
| External DSP | Nuclust DSP Server에서 실행 |

세션은 인서트마다 실행 모드, 서버, 모듈 ID, 보고 지연, 사용 가능 상태, 마지막 오류를 보존한다.

운영 원칙:

- External DSP가 없어도 세션은 정상적으로 열린다.
- 서버가 없는 슬롯은 소리를 몰래 바꾸지 않고 Waiting/Offline으로 표시한다.
- Native 대체는 사용자가 확인하는 명시적 동작이어야 한다.
- 같은 서버에 연속 배치된 External DSP는 가능한 한 한 번의 왕복 체인으로 묶는다.
- 실행 위치를 바꿀 때 새 경로를 준비한 뒤 안전한 블록 경계에서 전환한다.
- 모든 병렬 경로는 모드별 실제 지연을 PDC에 반영한다.

## 7. 현재 부족한 핵심과 우선순위

| 우선순위 | 기능 | 현재 판단 | 완료 기준 |
|---|---|---|---|
| P0 | 세션 저장/복구 일관성 | 레거시 계약 일부 이식 필요 | 자동저장, 백업, 충돌 복구, 미디어 재연결 검증 |
| P0 | Mixer Graph/PDC | 기반 있으나 실제 전 경로 검증 필요 | Insert/Send/Aux/Master/병렬 경로 샘플 정렬 |
| P0 | Monitor/송출 안전성 | DW 강점, 제품화 연결 필요 | Master 분리, A/B/C, Cue, Talkback, Listen Room, Monitor Print |
| P0 | 녹음 신뢰성 | 기능 기반 존재 | 장시간·다채널·펀치·프리롤·파일 복구 테스트 |
| P1 | I/O Setup | 장치 선택 기반 존재 | 입력/출력/버스/Monitor/Cue 매핑 저장과 재호출 |
| P1 | Playlist/Take/Comp | Pro Tools급 작업에 부족 | 녹음 테이크 생성, 승격, 구간 컴핑, Undo |
| P1 | Render Queue | Bounce 기반 존재 | Mix/Stem/Monitor Print 반복 납품과 Manifest |
| P1 | 플러그인 슬롯 완성 | 트랙 인서트 중심 구현 | Instrument, Master, Monitor Speaker 슬롯 편집 통일 |
| P2 | Picture/Post | 일부 기반 | 영상, 타임코드, ADR/마커, 납품 프리셋 |
| P2 | MIDI/창작 계층 | 피아노롤/녹음 기반 | 표현 편집, 패턴, 모듈레이션, 장치 체인 |
| P3 | 마스터링/네트워크 오디오 | 장기 과제 | 앨범 흐름, 메타데이터, RAVENNA/AES67 검토 |

## 8. 개발 계획

### Phase 0. DW 기준선 고정

목표: 현재 작업 중인 DW를 안전한 출발점으로 확정한다.

- 현재 수정 파일과 미완성 기능 목록 기록
- 30개 테스트의 기준 결과 저장
- 앱 실행과 주요 화면 스크린샷 기준 저장
- `DAW`에서 가져오는 코드의 소유 경계 확정
- 프로젝트 문서의 DW/DAW 경로 혼동 제거

완료 조건: 새 작업자가 문서만 보고 DW를 빌드하고, 앱을 열고, 테스트 기준을 재현할 수 있다.

### Phase 1. Pro Tools급 세션 코어

목표: 중요한 녹음 세션을 맡길 수 있는 기본 신뢰성을 만든다.

- Session Folder, Audio Files, Bounces, Backups 표준화
- Save As, Save Copy, Auto Backup, Recovery
- 미디어 누락/이동/재연결
- 트랙 생성, 복제, 비활성화, 숨김, 그룹, 폴더
- Playlist/Take Lane/Comp
- 클립 그룹, 클립 게인, 페이드, 비파괴 편집
- 메모리 위치와 선택 범위 저장

완료 조건: 녹음, 편집, 저장, 종료, 재실행 뒤 세션과 소리가 동일하다.

### Phase 2. Mixer Graph와 DSP 완성

목표: 화면상의 믹서와 실제 렌더 결과가 항상 일치하게 만든다.

- Track/Aux/Bus/Master 라우팅 그래프 통합
- Sends와 Sidechain
- Insert reorder/bypass와 상태 복원
- Native/Internal/External DSP 전환
- PDC와 병렬 경로 정렬
- Offline Bounce와 Realtime Playback의 결과 비교
- Master, Instrument, Monitor 슬롯 편집기 통일

완료 조건: 재생과 Bounce가 허용 오차 안에서 일치하고, 모든 DSP 모드의 지연이 보상된다.

### Phase 3. Monitor DSP와 송출 Monitor

목표: Neuracoust DAW만의 가장 강한 제품 차이를 완성한다.

- Master와 Monitor 경로의 완전 분리
- Speaker A/B/C와 개별 Calibration
- Headphone/Cue와 Artist Mix
- Stereo/L/R/Mono/M/S/Phase 검수
- Dim/Mute/Talkback 상태 머신
- Loudness, True Peak, Downmix 검사
- Listen Room 연결, 인증, 지연, 재접속, 상태 표시
- 송출 보호 Limiter
- Monitor Print와 보고서

완료 조건: Monitor 조작이 Master Bounce를 바꾸지 않으며, Monitor Print만 의도한 처리를 포함한다.

### Phase 4. Post와 Picture

목표: 음악뿐 아니라 영상과 방송 후반 작업을 수행한다.

- 영상 재생과 프레임 정확도
- Timecode, Feet+Frames 검토, Pull Up/Down 정책
- ADR 마커와 대사/장면 메모
- Field Recorder 및 다채널 파일 매칭
- AAF/OMF 교환 범위 결정
- Loudness 규격별 납품 프리셋

완료 조건: 영상 기준점과 최종 오디오 파일의 시작/길이/프레임이 일치한다.

### Phase 5. 창작 및 음악가 기능

목표: 정밀 편집을 해치지 않으면서 빠른 작곡과 실험을 지원한다.

- MIDI 표현과 MPE
- Step/Pattern 편집
- Tempo/Chord 감지 보조
- Clip Launcher 또는 대안적 아이디어 스케치 영역 검토
- 장치 체인과 제한된 모듈레이션
- Render in Place/Freeze

완료 조건: 창작 기능으로 만든 결과가 일반 Edit/Mix 구조로 안정적으로 확정된다.

### Phase 6. 하이엔드 녹음과 마스터링

목표: Pyramix급 사용 사례를 단계적으로 수용한다.

- 고채널 장시간 녹음과 복구
- 앨범/트랙 순서와 곡간 처리
- DDP 및 메타데이터 범위 검토
- 고해상도/DSD 정책 연구
- RAVENNA/AES67 네트워크 오디오 검토

완료 조건: 지원한다고 명시한 포맷과 워크플로는 실제 납품 검증까지 통과한다.

## 9. 첫 구현 묶음

다음 작업 묶음을 첫 제품 마일스톤으로 한다.

1. 현재 DW 기준선과 테스트 결과 고정
2. Session Folder, Auto Backup, Recovery 완성
3. I/O Setup과 Monitor/Cue 출력 맵 완성
4. Playlist/Take Lane/Comp 구현
5. Mixer Graph/PDC 전체 경로 검증
6. Master와 Monitor 경로 분리 검증
7. Listen Room과 Monitor Print 완성
8. Render Queue와 Render Manifest 구현

이 순서는 “기능 수”보다 녹음물을 잃지 않고, 잘못된 소리를 납품하지 않는 것을 우선한다.

## 10. 사용자 매뉴얼 초안

### 10.1 새 세션 만들기

1. 새 세션에서 이름, 위치, Sample Rate, Bit Depth, Frame Rate를 정한다.
2. 음악, 포스트, 라이브 녹음, 마스터링 템플릿 중 하나를 선택한다.
3. Audio Files, Bounces, Backups 폴더는 자동 생성된다.
4. I/O Setup에서 입력, 출력, Bus, Monitor, Cue를 확인한다.

### 10.2 녹음하기

1. Audio Track을 만들고 Input과 Output을 지정한다.
2. Record Arm과 필요 시 Input Monitor를 켠다.
3. Monitor Dock에서 연주자 Cue와 Talkback을 확인한다.
4. Pre-roll, Count-off, Punch 범위를 설정한다.
5. Record를 실행한다. 반복 녹음은 새 Playlist/Take에 저장된다.
6. 녹음 후 파일 상태와 세션 백업 표시를 확인한다.

### 10.3 편집과 컴핑

1. Smart 도구로 선택, 이동, Trim, Fade를 상황에 맞게 사용한다.
2. Grid와 Slip을 작업에 맞게 전환한다.
3. Playlist/Take Lane을 열고 사용할 구간을 메인 플레이리스트로 승격한다.
4. Clip Gain과 Crossfade로 연결을 정리한다.
5. 모든 편집은 원본 파일을 보존하며 Undo할 수 있다.

### 10.4 믹스하기

1. 트랙을 Folder, Bus, Aux로 정리한다.
2. Inserts와 Sends를 배치한다.
3. Neuracoust 플러그인은 필요에 따라 Native, Internal, External을 선택한다.
4. DSP 상태 배지가 Ready인지 확인한다.
5. 자동화를 작성하고 PDC 상태를 확인한다.

### 10.5 Monitor와 송출

1. Monitor Source와 Speaker A/B/C를 선택한다.
2. Stereo, Mono, L/R, M/S, Phase로 믹스를 검수한다.
3. Calibration과 Reference Level을 사용한다.
4. Talkback은 Cue/Listen Room 정책에 따라 Main Monitor를 Dim한다.
5. Listen Room에서 청취자를 초대하고 송출 상태, 지연, 보호 Limiter를 확인한다.
6. 일반 Bounce에는 Monitor DSP가 들어가지 않는다.

### 10.6 납품하기

1. Deliver에서 Mix, Stem 또는 Monitor Print를 선택한다.
2. 파일 형식, Sample Rate, Bit Depth, Dither를 설정한다.
3. Loudness와 True Peak 목표를 선택한다.
4. 여러 결과는 Render Queue에 넣는다.
5. 완료 후 파일과 함께 Render Manifest를 확인한다.

## 11. 검증 기준

각 기능은 화면에 보이는 것만으로 완료하지 않는다.

- 세션: 저장/재실행/복구 후 프로젝트와 소리가 동일해야 한다.
- 녹음: 중단이나 장치 오류 뒤에도 이미 기록된 파일을 복구할 수 있어야 한다.
- 믹서: Realtime과 Offline 결과가 정해진 허용 오차 안에서 일치해야 한다.
- PDC: 병렬 경로와 혼합 DSP 모드에서 샘플 정렬을 검증해야 한다.
- Monitor: A/B/C, Mono, M/S, Phase, Dim, Talkback이 Master Bounce를 변경하지 않아야 한다.
- Listen Room: 연결 끊김과 재접속이 세션 재생을 멈추거나 엔진을 막지 않아야 한다.
- UI: macOS 기준 화면 크기와 주요 상태에서 글자 잘림과 조작 요소 겹침이 없어야 한다.
- 접근성: 키보드 운용, VoiceOver 이름, 색상 외 상태 표시를 제공해야 한다.
- 지역화: 한국어, 영어, 일본어, 중국어 기본 계층을 유지해야 한다.
- 회귀: 기존 DW 테스트와 Neuracoust DAW Validation Harness를 통과해야 한다.

## 12. 문서 운영 규칙

- 이 문서는 제품 방향과 구현 우선순위의 기준이다.
- 세부 엔진 계약은 `AGENTS.md`와 `docs/legacy-ui-contract.md`를 우선한다.
- 화면 수치와 색상은 `docs/design-tokens.md`를 우선한다.
- 새 기능은 구현 전 “어느 참고 DAW의 장점을 해결하는가”보다 “Neuracoust 작업 흐름에서 어떤 문제를 해결하는가”를 먼저 적는다.
- 사용자 소리를 바꾸는 기능은 신호 흐름, 저장 상태, Undo, Bounce, Monitor Print에 미치는 영향을 함께 명시한다.
- 단계 완료 시 이 문서의 현재 판단과 완료 기준을 갱신한다.

## 13. 최종 방향

DW는 이미 새 DAW를 만들기에 충분히 좋은 출발점이다. 앞으로의 핵심은 기능을 계속 넓히는 것보다 다음 순서를 지키는 데 있다.

**세션 신뢰성 -> 실제 믹서 그래프 -> Monitor/송출 -> 납품 -> 포스트 -> 창작 확장 -> 하이엔드 포맷**

이 순서를 따르면 Pro Tools처럼 믿고 편집할 수 있고, Nuendo처럼 송출과 포스트에 강하며, Pyramix처럼 정밀하고, Bitwig·Logic·Studio One의 빠른 창작 흐름까지 담는 Neuracoust 고유의 DAW로 발전할 수 있다.
