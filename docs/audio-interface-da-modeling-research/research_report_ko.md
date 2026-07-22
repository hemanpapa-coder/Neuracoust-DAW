# 신뢰 브랜드 오디오 인터페이스 계보 및 D/A 모델링 조사

## 1. 조사 범위

컴퓨터 또는 오디오 네트워크에 연결되는 전문·프로슈머 오디오 인터페이스를 대상으로 했다. 1990년대 말 PCI/USB/FireWire 세대부터 2026년 현행 USB-C, Thunderbolt, Dante, RAVENNA/AES67 제품까지 포함한다.

포함 범위:

- 마이크 프리앰프가 포함된 데스크톱·랙 인터페이스
- 다채널 AD/DA 인터페이스와 모듈형 프로 시스템
- 과거 시장에서 널리 사용된 PCI, FireWire, USB 장비
- 독립 실측 자료가 풍부한 보급형 장비

별도 취급:

- 독립 DAC/헤드폰 앰프, DJ 믹서, 디지털 콘솔은 컴퓨터 인터페이스 기능이 명확할 때만 포함
- ADAT 전용 컨버터는 D/A 비교 가치가 큰 Ferrofish 계열만 보조 포함
- USB 마이크, 기타 페달, 방송 믹서는 일반 모니터 D/A 경로가 아니면 제외

카탈로그에는 30개 이상의 브랜드와 300개 이상의 모델/세대가 들어 있다. 다만 전 세계 모든 OEM, 지역 한정판, 색상 변형을 포함한다는 의미의 절대적 완전성은 보장하지 않는다. 구현 관점에서 의미 있는 신뢰 브랜드와 계보를 최대한 넓게 포착한 연구용 마스터 목록이다.

## 2. 브랜드별 주요 계보

### RME

- Digi96/Multiface → Fireface 400/800/UC → UCX/UFX → UCX II/UFX III
- Babyface → Babyface Pro → Babyface Pro FS
- MADIface/Digiface 계열

RME는 상세한 공식 전기 사양과 장기 드라이버 지원 때문에 기준군으로 가치가 높다. Babyface Pro FS는 XLR 출력 118 dBA, THD+N -102 dB, 300 Ω, +19/+4 dBu 모드를 공개한다. 동일 모델 내 DAC 부품 변경 가능성이 있어 시리얼과 PCB revision을 반드시 기록해야 한다.

### Focusrite

- Saffire FireWire
- Scarlett 1세대 → 2세대 → 3세대 → 4세대
- Clarett Thunderbolt/USB → Clarett+
- Red/RedNet 상위 계열

Scarlett는 세대별 성능 변화와 보급률 때문에 가장 중요한 모델군이다. 3세대 4i4는 라인 출력 DR 약 108.5 dBA, THD+N 약 -94 dB, 430 Ω인 반면 4세대 4i4는 120 dBA, -112 dB, 100 Ω로 공식 사양이 크게 바뀌었다. 세대명을 생략한 `Scarlett` 프로필은 만들면 안 된다.

### MOTU

- 828 → mkII → mk3 → 828x → 828es → 828(2024)
- UltraLite → mk3 → AVB → mk5
- Traveler, Audio Express
- M2/M4/M6
- 16A/24Ao/1248/8A 등 AVB 계열

M2/M4는 독립 측정과 분해 자료가 풍부하다. 초기 M4에서 ES9016S DAC가 보고됐지만 생산 revision에 따른 변경 가능성이 있다. M4와 UltraLite mk5는 서로 다른 재구성 필터 형태가 독립 측정에서 관찰되어 임펄스 기반 모델링 우선순위가 높다.

### Universal Audio

- Apollo FireWire → Apollo 8/8p/16 → Apollo X Gen 1 → Gen 2
- Apollo Twin → Twin MkII → Twin X Gen 1/2
- Arrow/Apollo Solo, Apollo x4
- Volt 1/2/4/176/276/476/476P → Volt 876

Apollo X Gen 2는 모델별 출력 사양을 분리해야 한다. Twin X Gen 2는 129 dB DR와 -120 dB THD+N이 발표됐고, x16 계열은 다른 경로와 더 높은 수치가 제시될 수 있다. Volt의 76 Compressor와 Vintage는 입력 경로 기능이므로 D/A 모델과 혼합하지 않는다. Volt 데스크톱 출력은 DC-coupled다.

### Apogee

- ONE, Duet FireWire → Duet 2/iOS → Duet 3
- Quartet, Ensemble FireWire/Thunderbolt
- Symphony I/O → Symphony I/O Mk II → Symphony Desktop/Studio

Symphony Desktop은 129 dBA, -114 dB, 50 Ω가 공개된 프리미엄 데스크톱 기준군이다. Symphony I/O는 카드/모듈에 따라 회로가 달라지므로 본체명만으로 하나의 프로필을 만들면 안 된다.

### Audient

- iD4/iD14/iD22/iD44 → MKII → iD24/iD48
- EVO 4/8/16
- ORIA immersive

iD4 MKII는 125.5 dBA, THD+N 0.0006%, +12 dBu를 발표한다. 구형 iD4의 115 dBA/-96.5 dB와 혼동하면 안 된다. ORIA는 16개 서라운드 출력과 별도 스테레오 출력을 가진 9.1.6 모델링 후보다.

### SSL

- SSL 2/2+ → MKII
- SSL 12 → SSL 18

SSL의 `4K`는 입력단 색채 기능이다. D/A 모델에는 출력 컨버터와 모니터 제어 경로만 포함해야 한다. 1 Ω 수준의 낮은 출력 임피던스와 세대별 DR 변화가 모델링 포인트다.

### Lynx / Prism / Merging / DAD

- Lynx Aurora → Aurora(n), Hilo → Hilo 2
- Prism ADA-8XR, Orpheus/Lyra/Titan/Atlas → ADA-128
- Merging Horus/Hapi/Anubis, Neumann MT 48
- DAD AX32/Core 256/AX Center 및 Avid MTRX 파생

이 그룹은 clean-reference군이다. Lynx Hilo 2는 127 dBA, -120 dB, ±0.03 dB를 공개한다. Neumann MT 48은 Merging 계열 기술을 사용하는 고성능 데스크톱 기준 후보다. 모듈형 제품은 카드 part number까지 저장해야 한다.

### Antelope Audio

- Orion32 → 32+ → Gen3 → Gen4
- Orion Studio 세대
- Zen Studio/Tour/Go/Q/Quadro
- Discrete 4/8 → Synergy Core → Pro
- Galaxy 32/64

공식 발표 DR가 매우 높지만 monitor output과 다채널 line output의 수치가 다를 수 있다. 클럭 마케팅 문구를 D/A 음색 파라미터로 번역하지 말고 실제 출력 측정을 사용한다.

### PreSonus / Steinberg / Arturia

- PreSonus AudioBox/VSL/Studio → Quantum Thunderbolt → Quantum ES/HD
- Steinberg CI/UR/UR-RT → UR-C/IXO/AXR
- Arturia AudioFuse/Studio/8Pre/16Rig 및 MiniFuse

보급형과 중급형 비교군이다. `32-bit` 표시는 실제 아날로그 다이내믹 레인지가 192 dB라는 의미가 아니다. USB 프로토콜과 컨버터 워드 길이도 구분한다.

### Avid / Digidesign

- Mbox → Mbox 2 → Mbox 3 → Mbox Studio
- 192 I/O → HD I/O → Carbon
- MTRX/MTRX Studio

192 I/O와 HD I/O는 지난 20년 스튜디오 제작물의 중요한 기준이다. 아날로그 카드 revision, calibration level, DigiLink 시스템 상태를 함께 캡처해야 한다. MTRX는 DAD 모듈 기반이므로 모듈 구성을 모델 키에 포함한다.

### 기타 신뢰 브랜드군

카탈로그에는 Metric Halo, Ferrofish, TASCAM, Zoom, Native Instruments, M-Audio, Roland/Edirol, ESI, Behringer, Mackie, IK Multimedia, Black Lion Audio, Lewitt, SPL도 포함했다. 이 그룹은 초기 디지털 오디오의 실제 보급 환경과 가격대별 차이를 재현하는 데 유용하다. 다만 오래된 RMAA loopback 자료는 D/A와 A/D가 합쳐진 결과이므로 계수 추출용이 아니라 후보 선별용으로만 사용한다.

## 3. 측정값 해석 규칙

### Dynamic range

- `A-weighted`와 unweighted를 같은 숫자로 비교하지 않는다.
- AES17 방식인지, -60 dBFS 신호 방식인지 확인한다.
- 제조사 DAC chip 수치와 완제품 line output 수치를 구분한다.

### THD+N

- 측정 주파수, 레벨, 대역폭, 부하를 함께 저장한다.
- `-1 dBFS @ 1 kHz, 20 kHz BW`와 `0 dBFS, 90 kHz BW`는 비교 불가다.
- `%`는 `20 log10(value/100)`으로 dB 변환하되 원문 값도 보존한다.

### 출력 레벨과 임피던스

- +24/+20/+18/+16/+12 dBu 기준이 다르면 동일 dBFS의 실제 전압이 달라진다.
- 동일 디지털 레벨 비교와 동일 아날로그 전압 비교를 모두 수행한다.
- monitor, line, headphone 출력을 별도 경로로 저장한다.

### 필터와 임펄스

- 44.1/48/88.2/96/176.4/192 kHz별로 기록한다.
- linear/minimum phase, fast/slow roll-off, pre/post-ringing, passband droop를 추출한다.
- 드라이버 또는 firmware가 필터를 변경할 가능성을 기록한다.

## 4. 구현 우선순위

### Tier A: 직접 확보·측정 우선

1. Focusrite Scarlett 2i2/4i4 3rd 및 4th Gen
2. MOTU M2/M4와 UltraLite mk5
3. RME Babyface Pro 및 Pro FS
4. Universal Audio Apollo Twin MkII/X Gen1/Gen2와 Volt 2
5. Audient iD4 MkI/MKII, iD14 MKII
6. SSL 2/2+와 MKII
7. Apogee Duet 2/3, Symphony Desktop
8. Lynx Hilo/Hilo 2 또는 Neumann MT 48

### Tier B: 스튜디오 표준 역사군

- Digidesign/Avid 192 I/O와 HD I/O
- RME Fireface 800/UCX/UFX 세대
- MOTU 828 세대
- Focusrite Saffire Pro 40/Clarett
- Apogee Ensemble/Symphony I/O
- Metric Halo 2882/ULN-8

### Tier C: 가격대·저성능 대비군

- Behringer UM2/UMC202HD
- M-Audio Fast Track/AIR
- PreSonus AudioBox USB 96
- Steinberg UR22C
- Arturia MiniFuse
- Native Instruments Komplete Audio

## 5. 모델 프로필에 반드시 포함할 메타데이터

```text
brand
model
hardware_revision
serial_range
firmware_version
driver_version
output_path
reference_level_dbu
monitor_knob_position
sample_rate
bit_depth_reported
load_ohms
measurement_bandwidth_hz
weighting
analyzer
calibration_date
temperature_c
source_url
raw_data_sha256
license_or_permission
```

## 6. 확인이 필요한 항목

- 2000년대 장비의 공식 페이지 소실 및 지역별 모델명 차이
- MOTU M2/M4, RME Babyface Pro FS 등 장기 생산 모델의 converter revision
- Antelope, Avid, Prism, Merging의 모듈별 실제 출력 카드 차이
- 제조사 발표치가 monitor out인지 fixed line out인지 불명확한 제품
- YouTube 측정 영상의 원시 데이터 사용 허가
- RMAA loopback 결과에서 D/A와 A/D 분리 가능 여부

## 7. 결론

제품명마다 임의의 EQ와 고조파를 붙이는 방식은 권장하지 않는다. 신뢰 가능한 모델은 실측 원시 데이터가 있는 경로만 활성화하고, 나머지는 `catalog_only` 상태로 유지해야 한다. 고급 인터페이스의 올바른 모델은 변화가 거의 없는 경우가 많으며, 그 사실 자체가 측정 결과다.

