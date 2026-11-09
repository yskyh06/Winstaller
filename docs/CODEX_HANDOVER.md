# LiteWin (Winstaller) 전체 프로젝트 인수인계 문서 (Codex Handover Specification)

## 1. 프로젝트 개요 및 핵심 목표 (Mission & Scope)
- **프로젝트 명**: LiteWin (구 Winstaller)
- **핵심 목표**: 900MB 미만의 초경량 부팅 ISO(Arch Linux Live 기반)에서 구동되는 **OS 무인 자동 설치(Zero-Touch Deployment) 도구**.
- **지원 대상 OS**:
  1. **Windows 11** (Home / Pro, 다국어 지원, Microsoft 공식 ESD 스트리밍 다운로드 -> WIM 적용 -> ESP 내 WinPE 스테이징)
  2. **Linux (Ubuntu 26.04 LTS Desktop & Server CLI)** (공식 ISO 다운로드 -> OverlayFS 루트 파일시스템 추출 -> Casper 제거 및 로컬 initramfs 생성 -> UEFI GRUB 2 부트로더 구성 -> 호스트 OpenSSL 기반 계정/암호 주입)
- **현재 작업 브랜치**: [`feature/v1.1-multios`](https://github.com/yskyh06/Winstaller/tree/feature/v1.1-multios) (원격 저장소 푸시 완료, PR 생성 가능)
- **현재 검증 상태**:
  - 단위 테스트: **89/89 전체 통과** (`ctest --test-dir build --output-on-failure`)
  - 실기/가상머신 검증: Windows 11 무인 설치 및 Ubuntu 26.04 LTS 설치·부팅·로그인 인증 정상 동작 확인 완료

---

## 2. 코드베이스 구조 및 핵심 파일 (Repository Map)

```text
litewin/
├── CMakeLists.txt                # 전체 빌드 및 CTest 단위 테스트 타겟 정의
├── src/
│   ├── main.cpp                  # 진입점 및 TUI 단계 제어 (Wifi -> Edition -> Disk -> Account -> Progress)
│   ├── ui/
│   │   ├── ansi_terminal.hpp/.cpp # 외부 라이브러리 없는 순수 ANSI 이스케이프/VT100 터미널 제어
│   │   └── wininstaller_tui.hpp/.cpp # 30ms 비동기 입력 스레드, 모달 다이얼로그, 계정/디스크/에디션 TUI
│   ├── core/
│   │   ├── linux_catalog.hpp/.cpp # 리눅스 배포판 카탈로그 (다운로드 URL, SHA256, 압축 모드, 레이어 목록)
│   │   ├── esd_catalog.hpp/.cpp   # Windows 11 버전/언어별 ESD 카탈로그
│   │   ├── unattend_generator.hpp/.cpp # Windows unattend.xml 자동 생성기
│   │   └── hash_verifier.hpp/.cpp # 스트리밍 SHA-256 검증기
│   └── platform/
│       ├── linux_installer.hpp/.cpp # 리눅스 7단계 무인 설치 파이프라인 전체 구현체
│       ├── winpe_bootstrapper.hpp/.cpp # Windows WinPE 부트스트래핑 파이프라인
│       ├── disk_partitioner.hpp/.cpp # sfdisk/mkfs.fat/mkfs.ext4 파티셔닝 및 마운트 제어
│       ├── disk_inspector.hpp/.cpp   # NVMe/SATA/SCSI 디스크 인벤토리 검사
│       ├── network_configurator.hpp/.cpp # iwd/wpa_supplicant Wi-Fi 및 이더넷 연결 제어
│       ├── http_downloader.hpp/.cpp  # libcurl 기반 재개 지원 스트리밍 다운로더
│       └── process_runner.hpp/.cpp   # 비동기 프로세스 실행, 표준 입출력 스트리밍, 에러 캡처
├── packaging/archiso/
│   ├── packages.x86_64           # Archiso Live 환경에 포함될 패키지 목록
│   ├── profiledef.sh             # ISO 빌드 프로필 및 파일 권한
│   ├── grub/grub.cfg             # Live ISO 부팅 GRUB 메뉴 및 커널 옵션
│   └── airootfs/etc/systemd/system/
│       └── litewin.service       # 부팅 시 tty1에서 litewin 바이너리 자동 시작
├── scripts/
│   └── build_iso_docker.sh       # Docker(archlinux:latest) 기반 클린 ISO 재빌드 스크립트
├── tests/
│   ├── linux_installer_tests.cpp # Linux 파이프라인 단계별 유닛/드라이런 테스트
│   ├── linux_catalog_tests.cpp   # 리눅스 카탈로그 URL 및 해시 무결성 테스트
│   └── tui_tests.cpp             # TUI 키 네비게이션, 모달, 계정 입력 단위 테스트
└── docs/                         # 핵심 가이드 및 아키텍처 문서
```

---

## 3. 핵심 아키텍처 및 해결된 난제 (Critical Lessons Learned)

### 3.1 리눅스 파일시스템 추출 (ExtractRootFS)
- **문제**: `unsquashfs`로 여러 SquashFS 레이어를 동일 디렉터리에 덮어쓰면 화이트아웃(Whiteout) 캐릭터 디바이스 충돌로 `unlink_file: failed` 치명적 오류 발생.
- **해결책**: 커널 네이티브 OverlayFS(`mount -t squashfs -o loop,ro` + `mount -t overlay ... merged` + `cp -a merged/. target/`)를 적용하여 100% 무결점 병합 추출 구현.

### 3.2 Casper 라이브 부팅 탈출 및 로컬 커널화
- **문제**: 우분투 라이브 서버 ISO에는 `casper`가 포함되어 있어 디스크 부팅 시 `(initramfs) Unable to find a medium containing a live file system` 및 `stdin: Invalid argument` 무한 루프 발생.
- **해결책**:
  1. `casper`, `lupin-casper`, `live-boot` 패키지 및 스크립트 완전 제거(`apt-get purge`, `rm -rf /usr/share/initramfs-tools/scripts/casper*`).
  2. `/etc/initramfs-tools/conf.d/litewin-local-boot.conf`에 `BOOT=local` 강제 기록.
  3. `/lib/modules/*`에 존재하는 커널 버전에 대해 `update-initramfs -c -k <kver>`로 로컬 initramfs 재성성.
  4. GRUB 커널 부팅 인자에 `boot=local` 명시.

### 3.3 단독 디스크 부팅 (`No Media` / GRUB 2.16)
- **문제**: GNU GRUB 2.16에서 제거된 레거시 모듈(`efi_uga`)을 `grub-install --modules`로 전달하면 GRUB 설치가 조용히 중단되어 `/boot/efi/EFI/ubuntu/grubx64.efi`가 미생성됨.
- **해결책**: 불필요한 `--modules`를 제거하고, `grub-mkstandalone` 폴백 및 UEFI 기본 경로인 `/EFI/BOOT/BOOTX64.EFI` 복사를 보장.

### 3.4 로그인 인증 실패 (`Login incorrect`) 원천 해결
- **문제**: chroot 내부에서 실행되는 데비안 `chpasswd`는 활성화된 PAM 세션이 없어 `Authentication token manipulation error`가 발생하며, 계정이 `!`(잠금) 상태로 방치됨.
- **해결책**:
  1. 호스트의 OpenSSL로 단방향 솔트 암호화(`openssl passwd -6 <password>`) 수행.
  2. C++ 함수 [`ensure_unix_user_and_shadow()`](file:///home/yskyh06/linux-workspace/litewin/src/platform/linux_installer.cpp#L606-L745)를 구현하여 `/etc/passwd`, `/etc/shadow`, `/etc/group`을 직접 읽고 안전하게 파싱하여 SHA-512 해시 주입 및 셸(`/bin/bash`), 홈 디렉터리, `sudo` 그룹을 보장.
  3. 모든 chroot 명령에 `export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin;`를 주입하여 바이너리 경로 유실 방지.

### 3.5 부팅 지연(2분 대기) 제거
- 우분투 서버 기본 설정인 `systemd-networkd-wait-online.service` 및 `NetworkManager-wait-online.service`를 chroot 단계에서 마스크(`systemctl mask`)하여 네트워크 미연결 시에도 1초 만에 로그인 화면으로 진입.

---

## 4. 빌드, 테스트 및 릴리스 커맨드 가이드

### 4.1 네이티브 C++ 빌드 및 유닛 테스트
```bash
# 빌드 디렉터리 생성 및 컴파일
cmake -B build -S .
cmake --build build -j$(nproc)

# 89개 단위 테스트 전체 실행
ctest --test-dir build --output-on-failure

# TUI 드라이런 동작 확인 (하드웨어 미수정)
./build/litewin --dry-run
```

### 4.2 부팅 ISO 재빌드 (Docker 기반)
```bash
# Arch Linux 공식 최신 컨테이너를 이용한 Archiso 빌드
./scripts/build_iso_docker.sh

# 산출물 확인 (build/iso_out/litewin-2026.11-x86_64.iso)
ls -lh build/iso_out/*.iso
sha256sum build/iso_out/*.iso
```

---

## 5. Codex에게 넘길 후속 과제 (Next Tasks / Roadmap)

1. **TUI 다국어 및 한글 폰트 렌더링 (`kmscon`)**:
   - 현재 리눅스 텍스트 콘솔은 비트맵 폰트(`ter-v24b`)를 사용하여 한글이 표시되지 않음.
   - `kmscon` 데몬과 `ttf-nanum` 폰트(+10~15MB)를 `packaging/archiso/`에 도입하고, [`src/ui/ansi_terminal.cpp`](file:///home/yskyh06/linux-workspace/litewin/src/ui/ansi_terminal.cpp)의 문자열 폭 계산을 UTF-8 전폭(2칸) 기준으로 보정.
2. **리눅스 지원 배포판 확대**:
   - [`src/core/linux_catalog.cpp`](file:///home/yskyh06/linux-workspace/litewin/src/core/linux_catalog.cpp)에 Arch Linux (`SingleSquashFS` / `airootfs.sfs`), Fedora / Rocky Linux (`Erofs` 또는 `LiveOS/squashfs.img`) 타겟 추가 및 파이프라인 연동.
3. **원격 카탈로그 동적 갱신**:
   - ISO 재빌드 없이 최신 리눅스 및 Windows ESD URL/해시를 원격 HTTPS JSON에서 불러오는 동적 카탈로그 다운로더 구현.
