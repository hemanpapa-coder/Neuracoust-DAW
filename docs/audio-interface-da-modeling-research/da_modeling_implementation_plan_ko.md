# 오디오 인터페이스 D/A 모델러 — 구현 계획과 "이큐 외 요소" 적용 방법

이 문서는 `claude_handoff.md`의 절대 규칙(특히 #2, #5, #7)을 지키면서
오디오 인터페이스 D/A 시뮬레이터를 **어떻게** 만들지, 그리고
사용자가 물은 **"이큐 외에 다른 요소들은 어떤 방법으로 적용해야 하나"**에
답하는 구현 계획이다.

## 0. 지금 상태 (정직한 출발점)

- 카탈로그: 346개 모델, 브랜드/계보/스펙/측정 상태만 있음.
  (`src/audio/AudioInterfaceCatalog.generated.cpp`, `resources/audio_interface_catalog.json`)
- `independentMeasurement`: `available` 162 / `limited` 182 / `not_located` 2.
- **그러나 `available`조차 요약 스펙(DR dBA, THD+N dB, 임피던스 Ω)뿐이고,
  raw 임펄스/레벨별 고조파/노이즈 PSD/크로스토크 같은 원시 측정치는 하나도 없다.**
  예: RME Multiface I 는 `available`이지만 DR/THD+N/임피던스가 전부 비어 있고
  노트가 "measurement conditions need verification"이다.

결론: **현재 어떤 모델도 오디오를 착색할 수 없다.** 규칙 #5(스펙은 UI/검증 한계로만)와
#7(level-matched null test 통과)에 따라, 착색은 raw 프로필이 확보된 뒤에만 켜진다.
그래서 지금 인터페이스 선택은 **모니터 체인을 정의/표시**할 뿐, 소리를 바꾸지 않는다 —
그리고 그게 정답이다. (고급 인터페이스는 실제로 거의 변하지 않으며, 그 사실 자체가 측정 결과다.)

## 1. 두 가지 사용 목적 (사용자 요구)

1. **실물 인터페이스로 선택 + 플랫 환경** — 이미 구현됨
   (`project.physicalAudioInterfaceModel`, `nc_monitor_set_physical_audio_interface_model`).
   측정 데이터가 없으므로 "플랫"이 곧 정확한 동작이다.
2. **내 물리 인터페이스를 다른 인터페이스로 구현 (A→B 변환)** — 프레임만 만들고 게이트로 막는다.
   A(내 장비)의 서명을 벗기고 B(목표)의 서명을 입히는 변환은
   **A와 B 양쪽 모두 raw 측정 프로필이 있어야** 성립한다. 지금은 둘 다 없으므로 항상 null 변환.

## 2. 프로필 상태 모델 (게이트의 핵심)

핸드오프의 상태 사다리를 그대로 쓴다. 오디오 처리는 `raw_measurement_acquired` 이상에서만 켜진다.

```
catalog_only                     ← 지금 대부분
official_specs_only              ← 스펙 숫자만 있음 (UI/검증용, 오디오 금지)
third_party_measurement_reference← RMAA 등 참고자료 있음 (계수 추출 금지, 후보 선별용)
raw_measurement_acquired         ← 원시 IR/레벨별 THD/노이즈 확보 (여기서부터 오디오 가능)
profile_fitted                   ← 계수 피팅 완료
profile_validated                ← null residual / ABX 통과
distribution_cleared             ← 배포 허가 확인
```

게이트 규칙(코드로 강제):
- `status < raw_measurement_acquired` → 그 모듈은 **무조건 bypass**.
- 요약 스펙 숫자(THD+N 한 개, DR 한 개)로는 어떤 모듈도 합성하지 않는다.
  THD+N 한 숫자로 고조파 커널을, DR 한 숫자로 노이즈 PSD를 복원하는 것은 물리적으로 불가능하다.

## 3. "이큐 외 요소"를 적용하는 방법 (모듈별)

핸드오프의 DSP 모듈 순서를 그대로 파이프라인으로 삼는다. 각 모듈은
(a) 필요한 **원시 측정 데이터**, (b) **적용 방식(DSP)**, (c) **null-test 판정 기준**을 가진다.
원시 데이터가 없으면 그 모듈은 존재는 하되 bypass 된다.

### 3.1 입력 게인 / 레퍼런스 캘리브레이션
- 데이터: 기준 dBu, monitor knob 위치, dBFS→전압 관계.
- 적용: 단순 스칼라 게인. **오디오 착색 아님** — 레벨 정합용. 항상 안전.
- null: level-matched 비교의 전제 조건.

### 3.2 샘플레이트별 재구성(재샘플링) 필터  ← 이큐 외 요소 1순위, 가장 들림
- 데이터: SR별(44.1/48/88.2/96/176.4/192) **임펄스 응답** — linear/min phase,
  fast/slow roll-off, passband droop, pre/post-ringing.
- 적용: 측정 IR에서 뽑은 **짧은 FIR**(또는 min-phase면 IIR 근사)로 컨볼루션.
  MOTU M4 vs UltraLite mk5처럼 재구성 필터 형태가 다른 장비에서 실제로 다르게 들린다.
- null: 원 장비와의 impulse pre/post-ringing 차이, passband FR 차이가 측정 오차 이내.
- **지금:** raw IR 없음 → bypass.

### 3.3 채널 간 게인/위상 불일치
- 데이터: L/R 레벨차(dB), L/R 위상/지연차(deg, µs).
- 적용: 2×2 정적 게인 + 아주 짧은 fractional-delay(위상용).
- null: 채널 간 차이가 측정치와 일치.
- **지금:** 데이터 없음 → bypass.

### 3.4 레벨 의존 고조파 커널 (하모닉)
- 데이터: **레벨별(-1, -6, -12… dBFS) H2–H9 진폭+위상 스펙트럼.** THD+N 한 숫자로는 불가.
- 적용: 측정 스펙트럼에 맞춘 **정적/동적 웨이브셰이퍼**(Chebyshev 다항식 피팅).
  레벨 의존성은 입력 레벨로 다항식 계수를 보간.
- null: H2–H9 레벨별 오차가 기준 이내, 동일 loudness ABX.
- **지금:** 레벨별 고조파 원시 데이터 없음 → bypass. (이것을 스펙으로 지어내는 것이 규칙 #2 위반의 핵심.)

### 3.5 크로스토크 행렬
- 데이터: 주파수별 채널 간 누화(dB vs Hz).
- 적용: 주파수 의존 2×2 off-diagonal 필터.
- null: crosstalk vs freq 곡선 일치.
- **지금:** 데이터 없음 → bypass.

### 3.6 측정 노이즈/스퍼 프로필
- 데이터: 노이즈 **PSD** + 이산 스퍼(mains hum, 클럭 관련)의 주파수/레벨.
- 적용: 측정 PSD로 셰이핑한 컬러드 노이즈 + 스퍼 톤을 **레벨 정합** 후 가산.
  (가산은 파괴적이므로 실측 + 정합된 경우에만.)
- null: 출력 노이즈 PSD가 원 장비와 일치.
- **지금:** 데이터 없음 → bypass.

### 3.7 클리핑 전달 곡선
- 데이터: FS 근처 출력단 **전달 곡선**(입력 dBFS vs 출력 전압, 소프트/하드 니).
- 적용: 곡선을 LUT/다항식 웨이브셰이퍼로. 정상 레벨에선 사실상 선형.
- null: 클립 근처 하모닉/컴프레션이 측정과 일치.
- **지금:** 데이터 없음 → bypass.

### 3.8 출력 임피던스 / 부하 상호작용 (헤드폰 모드에서 의미)
- 데이터: 인터페이스 **Zout(Ω)** + 헤드폰의 **임피던스 곡선(Z vs Hz)**.
- 적용: 분압식 FR = `Zheadphone(f) / (Zout + Zheadphone(f))`. 저임피던스/멀티드라이버
  헤드폰에서 Zout이 크면 실제로 FR가 휜다.
- **부분적으로 가능한 유일한 요소:** 카탈로그에 Zout 스펙이 있고(43개 모델),
  헤드폰 임피던스 곡선을 실측하면 이 항목만은 스펙+실측 조합으로 정직하게 켤 수 있다.
- null: 부하 연결 시 FR 변화가 측정과 일치.
- **지금:** 헤드폰 임피던스 곡선 미보유 → bypass. (Zout만으로는 부족.)

## 4. A→B "다른 인터페이스로 구현" 변환
- 신호에서 A의 서명을 제거(역필터/역커널)한 뒤 B의 서명을 적용.
- 각 단계는 §3의 모듈을 A는 역방향, B는 정방향으로 쓴다.
- **A와 B 둘 다 `raw_measurement_acquired` 이상일 때만** 활성. 하나라도 미달이면 flat(null).
- UI는 활성/비활성 상태와 사유를 반드시 표시한다("raw 측정 데이터 없음 → 변환 비활성").

## 5. 코드 구조 (권장)
- `AudioInterfaceProfile` — 상태 사다리 + 옵셔널(null 가능) 측정 필드.
  빈 필드는 0이 아니라 **null 유지**(규칙 #1).
- `AudioInterfaceModeler` — §3 모듈 순서의 파이프라인. 각 모듈은 프로필에 원시 데이터가
  있을 때만 계수를 만들고, 없으면 bypass. 지금은 전 모델 bypass = 완벽한 null.
- 게이트를 한곳(`profileIsAudioReady`)에 두고, 스펙 숫자는 이 게이트를 절대 통과시키지 않는다.
- 회귀 테스트: 44.1/48/96/192 kHz에서 입력=출력(bypass일 때) sample-for-sample,
  프로필이 생기면 null residual/ABX.

## 6. 지금 당장 하는 것 (이 커밋 범위)
1. 이 문서(방법론) — 질문에 대한 답.
2. purpose-2 선택 UI: 인터페이스 메뉴에 "이 인터페이스를 다른 모델로 구현" 대상 선택 +
   상태 표시. 게이트가 항상 "카탈로그 전용 — 오디오 변환 없음"을 정직하게 보여준다.
3. 측정 상태 뱃지는 이미 있음(측정=available). 오디오 준비 상태(raw)와 구분해 표시.

측정 원시 데이터가 확보되면 §3의 모듈부터 순서대로 켠다. 그 전까지 이 시뮬레이터는
**정의하고 표시하되 소리를 바꾸지 않는다.**
