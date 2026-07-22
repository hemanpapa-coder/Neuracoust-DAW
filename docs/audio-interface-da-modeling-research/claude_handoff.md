# Claude 구현 전달문

## 목표

`audio_interface_catalog.csv`를 읽어 오디오 인터페이스 D/A 모델러의 카탈로그와 측정 프로필 레지스트리를 구현한다.

## 절대 규칙

1. 빈 수치 필드를 0으로 변환하지 않는다. `null`로 유지한다.
2. `independent_measurement != available`인 제품에 DSP 계수를 추정 생성하지 않는다.
3. 브랜드/모델/세대/hardware revision/output path/sample rate를 복합 키로 사용한다.
4. line, monitor, headphone 출력을 한 프로필로 합치지 않는다.
5. manufacturer spec은 UI 정보와 검증 한계로만 사용한다. 실측 IR/비선형 계수가 아니면 오디오 처리에 직접 넣지 않는다.
6. 모든 실제 DSP 프로필은 raw data SHA-256과 측정 조건을 가져야 한다.
7. dry/wet 효과처럼 과장하지 말고 level-matched null test를 통과하도록 만든다.

## 권장 상태 모델

```text
catalog_only
official_specs_only
third_party_measurement_reference
raw_measurement_acquired
profile_fitted
profile_validated
distribution_cleared
```

## DSP 모듈 순서

```text
input gain/reference calibration
-> sample-rate-specific reconstruction response
-> inter-channel gain/phase mismatch
-> level-dependent harmonic kernel
-> crosstalk matrix
-> measured noise/spur profile
-> clipping transfer curve
-> output impedance/load interaction (optional)
```

## 검증

- 원 장비와 모델의 FR 차이
- H2-H9 레벨별 오차
- noise PSD 차이
- crosstalk 차이
- impulse pre/post-ringing 차이
- 동일 loudness ABX 및 null residual
- 44.1/48/96/192 kHz 회귀 테스트

제품명은 연구용 UI에서 설명적으로만 사용하고, 상표 로고나 실제 제품 외관을 복제하지 않는다.

