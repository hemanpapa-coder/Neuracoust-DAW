# 독립 실측 데이터 확보 현황과 사용 규칙

## 주요 출처

| 출처 | 자료 형태 | 장점 | 제한 | 구현 사용 등급 |
|---|---|---|---|---|
| Audio Science Review | APx555/AP 계측 그래프, FFT, DR, THD+N, 멀티톤 | 조건이 비교적 명확하고 제품 간 비교 용이 | 원시 WAV/AP 프로젝트가 공개되지 않는 경우가 많음 | B; 수치 검증 및 후보 선정 |
| Julian Krause | 대량의 인터페이스 정규화 측정 | 보급형·세대 비교가 매우 풍부 | 영상 그래프 중심, 원시 데이터 이용 권한 별도 | B |
| Sound On Sound | 기술 리뷰, 제조사 확인, 일부 계측 | 모델 계보와 회로 변경 설명에 강함 | 일관된 전 모델 계측 데이터베이스는 아님 | B/C |
| 제조사 AES17/AP 사양 | 공식 기준과 측정 조건 | 법적·기술적 출처가 명확 | typical/최소치 혼재, 조건이 생략되기도 함 | A for metadata; B for model coefficients |
| RMAA 사용자 자료 | 구형 제품 커버리지 | 단종품의 유일한 실측인 경우가 많음 | loopback이므로 ADC+D/A 합성, 셋업 신뢰도 편차 | C |
| teardown/부품 사진 | DAC/op-amp/revision 확인 | 부품과 PCB revision 단서 | 동일 모델 전체 생산분을 대표하지 않음 | C until corroborated |

## `independent_measurement` 판정

- `available`: 구체적인 모델에 대해 FFT/THD+N/DR/주파수 응답 등 하나 이상의 실측 확인.
- `limited`: 리뷰 또는 일부 수치만 존재하거나 조건/원시 데이터 부족.
- `not_located`: 이번 조사에서 찾지 못함. 측정이 존재하지 않는다는 뜻은 아님.

## 원시 데이터로 확보할 파일

- 32-bit float WAV sweep/impulse/multitone/noise
- analyzer project 또는 CSV export
- calibration certificate/loopback validation
- DUT 사진, serial, PCB revision
- firmware/driver 버전 스크린샷
- 측정 조건 JSON

## 라이선스

공개 그래프를 보고 수치를 수기로 참고하는 것과, 그래프 이미지·원시 데이터를 제품에 포함하는 것은 다르다. 제3자 원시 데이터를 DSP 프로필로 배포하려면 작성자의 명시적 허가 또는 이용조건 확인이 필요하다.

