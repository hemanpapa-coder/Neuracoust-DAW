# NDS 어플라이언스 USB

빈 PC 한 대를 **꽂고 전원만 켜면** Neuracoust NDS 노드로 만들어 주는 설치 USB를 만듭니다.
CMOS에서 USB 부팅만 지정하면 그 뒤로는 사람이 손댈 일이 없습니다 — 메뉴도, 질문도,
"계속하시겠습니까"도 없습니다.

```sh
tools/node/usb/make-appliance-usb.sh                     # ISO 만들기
tools/node/usb/make-appliance-usb.sh --list              # 이동식 디스크 확인
tools/node/usb/make-appliance-usb.sh --write /dev/disk4  # USB에 쓰기
```

설치가 끝나면 노드는 이런 상태입니다:

- 데비안 12 + **PREEMPT_RT 커널**, 상위 절반 코어를 DSP 전용으로 격리
- `heinhome` 계정 + 이 맥의 ssh 공개키 (비밀번호 없이 관리 가능)
- `neuracoust-nds.service` 가 부팅마다 엔진을 UDP **20002/20003** 으로 올림
- 엔진이 `NA_DISCOVER` 에 응답하므로 모니터 스테이션의 **검색**에 바로 잡힘
- 호스트 이름 `nds-<MAC 끝 4자리>` — 풀에 여러 대를 넣어도 구분됨

설치하는 동안만 **인터넷이 되는 랜**에 물려 있으면 됩니다(netinst 는 패키지를 미러에서
받습니다). 그 뒤로는 직결이든 오디오 허브든 상관없습니다.

## 기존 "DSP USB Creator" 와 무엇이 다른가

`/Volumes/Program Dev/Linux DSP Server/usb_creator` 의 제작기는 뼈대(preseed + 첫 부팅
스크립트 + ISO 재작성)가 이미 옳았습니다. 자동으로 끝까지 가지 못하게 막고 있던 것들:

| 막던 것 | 왜 멈추는가 | 여기서 |
|---|---|---|
| **UEFI 부팅 메뉴** | 자동 항목을 `grub.cfg` 뒤에 덧붙이기만 해서 기본값이 아니었음. 7세대 보드는 UEFI로 부팅 → 메뉴에서 사람을 기다림 | 두 메뉴를 통째로 교체, `timeout 0` + 유일 항목 |
| **설치 디스크 선택** | `partman-auto/disk` 미지정. 디스크가 둘 이상(부팅한 USB 자신 포함)이면 질문 | `early_command` 가 **비이동식** 첫 디스크를 골라 partman과 grub에 주입 |
| **부트로더 위치** | `grub-installer/bootdev` 미지정 → 질문 | 같은 디스크로 자동 지정 |
| **기존 파티션/LVM/RAID** | 쓰던 PC의 디스크면 확인 질문이 여러 개 | 전부 사전 응답 |
| **맥OS에서 만든 ISO** | `hdiutil makehybrid` 는 부팅 불가 이미지를 만들고, `xorriso` 호출은 소스 경로 자체가 빠져 있었음 | `xorriso -boot_image any replay` — 원본의 하이브리드 MBR·El Torito·EFI 이미지를 그대로 승계 |
| **preseed 전달** | 매체의 `/cdrom` 에만 의존 | 인스톨러 **initrd 안에도** 심음(cpio 이어붙이기). 매체가 마운트되기 전 질문까지 커버 |
| **ssh 키** | 없음 → 설치 후 비밀번호 로그인 필요 | 이 맥의 공개키를 `authorized_keys` 로 심음 |
| **계정 이름** | `dsp` (배포 스크립트는 `heinhome` 을 씀) | `heinhome` 으로 통일 |
| **커널** | 배너는 RT라고 하는데 실제로는 `linux-image-amd64` (일반 커널) | `linux-image-rt-amd64` |
| **설치되는 제품** | jackd/NetJACK2 (UDP 19000, 멀티캐스트) — **DAW가 말할 수 없는 프로토콜** | 우리 `neuracoust-rt-engine` (NART UDP 20002/20003) + 콘솔 스트립·525A 모듈 |
| **무선/Wine/yabridge** | 2 GB 가까운 무관한 패키지가 설치 시간과 실패 지점을 늘림 | 제거 |
| **주소가 없는 경우** | DHCP 없는 오디오 허브·직결에서 주소를 못 받아 조용히 사라짐 | NetworkManager `ipv4.link-local=enabled` + ifupdown 폴백 |
| **첫 부팅 로그** | `exec 2>&1 \| tee` 는 동작하지 않는 문법 | `exec > >(tee -a …) 2>&1`, `/var/log/nds-firstboot.log` |
| **서버 ID 배정** | 멀티캐스트 주소에 `ping` 을 쏴서 대수를 세는 방식(동작하지 않음) | 불필요 — 풀 관리는 DAW 쪽 `ndsPoolHosts` 가 함 |

## 동작 순서

1. **부팅** — isolinux(CSM) 또는 grub(UEFI)이 즉시 자동 설치 항목으로 들어감
2. **설치** — initrd 안의 `preseed.cfg` 가 모든 질문에 미리 답해 둠. 약 15분
3. **재부팅** → `nds-firstboot.service` 가 한 번 돌면서
   - 호스트 이름을 MAC 기반으로 지정
   - 실시간 limits, 코어 격리 커널 파라미터, governor
   - USB에서 온 소스로 **엔진과 모듈을 노드에서 직접 컴파일**
   - `neuracoust-nds.service` 등록 후 자기 자신을 삭제
4. **재부팅** → 엔진 가동, 검색에 응답

## 검증 상태

빌드된 ISO에 대해 확인한 것:

- 두 부팅 메뉴가 통째로 교체되었고 대기 시간이 0 (`isolinux.cfg`, `grub.cfg` 를 이미지에서
  다시 꺼내 확인)
- 하이브리드 MBR 서명(`55aa`), El Torito, `EFI/boot/{bootx64,grubx64}.efi` 가 원본 그대로
- 인스톨러 initrd 끝에 붙인 `preseed.cfg` 가 이미지 안에서도 온전하고 소유자가 root
- 페이로드에 첫 부팅 스크립트가 찾는 파일이 하나도 빠짐없이 들어 있음
- preseed 69개 지시어 형식·중복·필수 항목 검사 통과
- **디스크 선택 로직은 실제로 실행해서 검증** — `test-preseed-disk-pick.sh` 가 preseed에서
  스니펫을 꺼내 가짜 머신 5종(USB가 먼저 잡히는 경우 포함)에 돌립니다

확인하지 **못한** 것: 가상머신에서의 실제 부팅. 이 맥의 Homebrew 구성에서 qemu 의존성
(vde)이 컴파일되지 않습니다. 첫 실물 부팅이 곧 첫 부팅 테스트입니다 — 만에 하나 멈추면
화면에 무엇이 떠 있는지 알려주세요. 설치 중 화면은 그대로 두면 됩니다.

## 파일

- `make-appliance-usb.sh` — 이미지 내려받기·검증, 페이로드 꾸리기, initrd/메뉴 손질, 재조립, USB 쓰기
- `preseed.cfg` — 데비안 인스톨러 전 질문에 대한 답
- `firstboot.sh` — 최소 설치본을 어플라이언스로 바꾸는 1회성 스크립트
- `nds-firstboot.service` — 그 스크립트를 한 번만 돌리는 유닛
- `run-engine.sh` — 엔진 런처. `--foreground` 는 systemd 용, 인자 없이 부르면 DAW의
  업데이트 경로가 쓰던 동작(재시작 대기 또는 detached 실행)

`run-engine.sh` 를 systemd가 `Restart=always` 로 잡고 있기 때문에, DAW의 업데이트 버튼이
프로세스를 kill 하면 **새로 빌드된** 바이너리로 자동 복귀합니다. 노드에서 root 가 필요한
동작은 설치 이후 하나도 없습니다.
