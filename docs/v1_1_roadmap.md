# LiteWin (Winstaller) Version 1.1 개발 로드맵 및 기능 명세서

> **목표**: 기존 Windows 10/11 클린 설치 엔진의 안정성을 유지하면서, **5대 리눅스 배포판(Ubuntu, Debian, Fedora, Rocky/Alma, Arch) 다이렉트 설치 엔진**과 **상단 탭 기반 멀티 OS 선택 TUI**를 통합한 멀티 OS 인스톨러(v1.1)로 확장합니다.

---

## 1. 핵심 추가 기능 요약

| 구분 | 기능명 | 상세 내용 | 우선순위 |
| :--- | :--- | :--- | :---: |
| **Core 1** | **리눅스 배포판 설치 엔진** | 5대 배포판(정규 + 연장 지원 버전) 공식 이미지 다운로드 및 3대 포맷 전개 | **P0** |
| **Core 2** | **멀티 OS 파이프라인 추상화** | Windows(NTFS/WIM/WinPE)와 Linux(ext4/SquashFS·EROFS/chroot) 설치 흐름 분리 | **P0** |
| **UX 1** | **상단 탭 + 단일 리스트 TUI** | `[Windows]` \| `[Ubuntu]` \| `[Debian]` \| `[RHEL/Fedora]` \| `[Arch]` 탭 및 `(Expired)` 표기 | **P1** |
| **UX 2** | **리눅스 초기 환경 자동 구성** | 호스트네임, 기본 계정/비밀번호, 로케일/타임존, 3대 계열별 UEFI GRUB 자동 세팅 | **P1** |

---

## 2. 리눅스 배포판 후보 선정 및 이미지 적용 방식 설계

베어메탈(일반 PC/노트북) 환경에서는 커널(`vmlinuz`), 하드웨어 드라이버(`linux-firmware`), 부트로더(`GRUB`), 네트워크 데몬이 모두 포함되어야 설치 직후 인터넷과 화면이 정상 작동합니다.

### 2.1 이미지 적용 방식 3가지 비교 및 최종 선정

| 적용 방식 | 동작 원리 | 장점 | 단점 (베어메탈 기준) | 채택 여부 |
| :--- | :--- | :--- | :--- | :---: |
| **방식 A: 공식 Live SquashFS 전개**<br>*(Windows WIM과 동일 원리)* | 공식 ISO 내 루트 이미지(`filesystem.squashfs` / `airootfs.sfs`)를 `unsquashfs`로 타겟 파티션에 직접 전개 | • **커널, `linux-firmware`(Wi-Fi/GPU), GUI, GRUB이 이미 완비됨**<br>• 단일 대용량 파일 스트리밍으로 속도가 매우 빠르고 균일함 | • 이미지 용량이 1.2GB ~ 3.5GB 수준 | **주력 채택 (권장)** |
| **방식 B: Cloud / Generic 이미지 전개** | 클라우드용 `.tar.xz` / `.qcow2` 이미지를 파티션에 풀고 설정 | • 다운로드 용량이 작음 (300~600MB) | • 가상화 커널 위주라 노트북 Wi-Fi 펌웨어(`linux-firmware`)가 빠져 있어 `chroot` 후 추가 설치 필요 | 보조 (Server용) |
| **방식 C: Bootstrap (`debootstrap` / `pacstrap`)** | 최소 베이스만 풀고 `apt` / `pacman`으로 수천 개 패키지를 실시간 설치 | • 설치 시점 최신 패키지 보장 | • 수천 개의 소형 파일 다운로드로 인한 속도 저하 및 의존성 오류 가능성 | 미채택 |

> [!IMPORTANT]
> **결론**: Windows의 `install.wim` 전개 방식과 구조적으로 1:1 대응되는 **[방식 A: 공식 Live SquashFS 추출 전개]** 방식을 표준 파이프라인으로 채택합니다.

---

### 2.2 2026년 11월 기준 지원 대상 배포판 라인업 (일반 지원 + 연장 지원 포함)

2026년 11월 현재 **정규 지원(Standard/Active)** 중인 버전뿐만 아니라, ROS·구형 SDK·엔터프라이즈 호환성을 위해 **연장 지원(ESM / LTS / ELTS / Maintenance)**이 살아있는 버전까지 모두 카탈로그에 포함합니다.

| 배포판 분류 | 포함 버전 (정규 + 연장 지원) | 지원 상태 및 기한 (2026.11 기준) | 주요 타겟 및 호환 용도 |
| :--- | :--- | :--- | :--- |
| **1. Ubuntu LTS** | • **26.04 LTS** (Resolute)<br>• **24.04 LTS** (Noble)<br>• **22.04 LTS** (Jammy)<br>• **20.04 LTS** (Focal)<br>• **18.04 LTS** (Bionic) | • 정규 ~2031.04 (ESM ~2036)<br>• 정규 ~2029.04 (ESM ~2034)<br>• 정규 ~2027.04 (ESM ~2032)<br>• **연장(ESM) ~2030.04**<br>• **연장(ESM) ~2028.04** | • 최신 데스크톱/서버 환경<br>• ROS 2 Jazzy / 현대 AI 환경<br>• **ROS 2 Humble** / 산업용 표준<br>• **ROS 1 Noetic** / 레거시 SDK<br>• **ROS 1 Melodic** / 구형 장비 |
| **2. Debian** | • **Debian 13** (Trixie)<br>• **Debian 12** (Bookworm)<br>• **Debian 11** (Bullseye) | • 정규 ~2028.08 (LTS ~2030.06)<br>• **LTS 지원 ~2028.06** (ELTS ~2033)<br>• **연장(ELTS) ~2031.06** | • 현행 최신 Stable 서버/데스크톱<br>• 검증된 안정형 서버 환경<br>• 구형 패키지 의존성 유지 환경 |
| **3. RHEL 계열**<br>*(Rocky / Alma)* | • **Rocky / AlmaLinux 10**<br>• **Rocky / AlmaLinux 9**<br>• **Rocky / AlmaLinux 8** | • 정규 지원 ~2035.05<br>• 정규 지원 ~2032.05<br>• **유지보수(보안) 지원 ~2029.05** | • 최신 엔터프라이즈 표준<br>• 현행 주류 데이터센터/워크스테이션<br>• CentOS 7/8 마이그레이션·레거시 호환 |
| **4. Fedora** | • **Fedora 45**<br>• **Fedora 44** | • 정규 지원 ~2027.12<br>• 정규 지원 ~2027.06 | • 최신 커널/최신 노트북 하드웨어<br>*(Fedora는 연장 지원 정책 없음)* |
| **5. Arch Linux** | • **Rolling Release** (`2026.11`) | • 상시 최신 지원 (단일 버전) | • 최신 패키지 및 커스텀 환경 |

---

### 2.3 공통 5단계 설치 파이프라인 및 계열별 어댑터 설계

모든 리눅스 배포판은 **단일 5단계 공통 파이프라인**으로 처리하되, 배포판 세대 및 계열(Family)에 따라 **3단계(이미지 추출 포맷)**와 **4~5단계(chroot 초기 설정 어댑터)**만 카탈로그 메타데이터 기반으로 자동 분기합니다.

| 단계 | Windows 파이프라인 (v1.0) | Linux 공통 파이프라인 (v1.1 추가) |
| :--- | :--- | :--- |
| **1. 파티셔닝** | GPT + ESP(FAT32) + MSR(16MB) + Windows(NTFS) + WinRE | GPT + ESP(FAT32, 512MB, `/boot/efi`) + Root(`ext4`, 나머지 전체, `/`) |
| **2. 다운로드** | MS 공식 CDN에서 `.esd` 다운로드 및 SHA-256 검증 | 공식 미러에서 Live/RootFS 이미지 다운로드 및 SHA-256 검증 |
| **3. 이미지 전개** | `libwim`으로 WIM Index 6(OS) 및 Index 2(WinPE) 전개 | **3대 압축 포맷 추출기**로 `/mnt/target`에 루트 파일시스템 전개 *(하단 2.4 참조)* |
| **4. 초기 구성** | `Unattend.xml`(OOBE 우회, 로컬 계정) 및 네트워크 드라이버 주입 | 공통 설정(`/etc/fstab`, 호스트네임, 로케일, `sudo` 계정) + **3대 계열별 어댑터 실행** |
| **5. 부트로더** | ESP에 WinPE 배치 및 UEFI NVRAM 부트 엔트리 등록 | 계열별 `initramfs` 갱신 후 UEFI GRUB(`grub-install` / `grub2-install`) 등록 |

---

### 2.4 배포판별 분기 상세 (3대 추출 모드 & 3대 계열 어댑터)

#### A. 3단계: 이미지 추출기 분기 (Image Extractor Mode)
1. **`Single-SquashFS` 모드** *(Ubuntu 18.04~22.04, Debian 11~13, Rocky/Alma 8~9, Arch Linux)*
   * 단일 `filesystem.squashfs` / `airootfs.sfs` / `squashfs.img`를 `unsquashfs -f -d /mnt/target`으로 1회 전개합니다.
2. **`Layered-SquashFS` 모드** *(Ubuntu 24.04 / 26.04 LTS)*
   * `/casper` 내 베이스 레이어(`minimal.squashfs` 또는 `ubuntu-server-minimal.squashfs`)를 먼저 전개한 뒤, 상위 레이어(`minimal.standard.squashfs` 등)를 순차적으로 덮어씌워 병합 전개합니다.
3. **`EROFS` 모드** *(Fedora 44 / 45, Rocky / AlmaLinux 10)*
   * 최신 LiveOS 표준인 `LiveOS/rootfs.erofs`를 `fsck.erofs --extract=/mnt/target` (또는 `mount -t erofs -o loop` 후 복사)으로 전개합니다.

#### B. 4~5단계: `chroot` 계열별 어댑터 분기 (Distro Family Adapter)

| 구분 | `Debian` 계열 어댑터<br>*(Ubuntu / Debian)* | `RedHat` 계열 어댑터<br>*(Fedora / Rocky / Alma)* | `Arch` 계열 어댑터<br>*(Arch Linux)* |
| :--- | :--- | :--- | :--- |
| **Live 패키지 정리** | `apt-get purge -y casper live-boot* calamares*` | `dnf remove -y anaconda* livesys-scripts` | Live 전용 systemd 훅 제거 및 호스트 커널(`vmlinuz-linux`) 복사 |
| **관리자 그룹** | `usermod -aG sudo <user>` | `usermod -aG wheel <user>` (및 SELinux `.autorelabel` 생성) | `usermod -aG wheel <user>` (`%wheel` sudo 활성화) |
| **Initramfs 갱신** | `update-initramfs -c -k all` | `dracut --regenerate-all --force` | `mkinitcpio -P` |
| **GRUB UEFI 설치** | `grub-install --target=x86_64-efi` + `update-grub` | `grub2-mkconfig -o /boot/grub2/grub.cfg` (Signed EFI Shim 활용) | `grub-install --target=x86_64-efi` + `grub-mkconfig` |

---

## 3. TUI 화면(Step) 및 OS 선택 UX 설계

TUI는 영문 전용(English-only) 인터페이스를 유지하며, **상단 탭 전환 + 단일 리스트 + 구버전 `(Expired)` 표기** 조합으로 가장 직관적이고 간결한 화면을 구성합니다.

1. **Step 1 (설치 대상 OS 언어/로케일 선택)**
   * 설치될 Windows 또는 Linux 내부에 적용할 타겟 언어/로케일 선택 (`Korean (ko-KR)` / `English (en-US)` 등)
2. **Step 2 (상단 탭 + 단일 리스트 기반 OS 및 버전 선택)**
   * **상단 가로 탭**: `[ Windows ]` | `[ Ubuntu ]` | `[ Debian ]` | `[ RHEL / Fedora ]` | `[ Arch ]` (좌우 방향키 전환)
   * **하단 단일 리스트**: 별도 배지 없이 배포판 이름과 버전만 깔끔하게 표시하되, 정규 지원이 종료(연장 지원 상태)된 버전만 이름 뒤에 **`(Expired)`** 문구를 붙입니다.
     * 예시 (`[ Ubuntu ]` 탭 선택 시):
       * `Ubuntu 26.04 LTS (Desktop GUI)`
       * `Ubuntu 26.04 LTS (Server CLI)`
       * `Ubuntu 24.04 LTS (Desktop GUI)`
       * `Ubuntu 24.04 LTS (Server CLI)`
       * `Ubuntu 22.04 LTS (Desktop GUI / Server CLI)`
       * `Ubuntu 20.04 LTS (Desktop GUI / Server CLI) (Expired)`
       * `Ubuntu 18.04 LTS (Desktop GUI / Server CLI) (Expired)`
3. **Step 3 (디스크 선택 및 계정 기본 설정)**
   * 설치 대상 디스크 선택 (VMD 미인식 시 안내 및 단일 QR 코드 제공)
   * Linux 선택 시 사용할 기본 사용자 아이디(`username`) 및 비밀번호 입력 모달 제공

---

## 4. 단계별 마일스톤 (구현 순서)

* **Phase 1**: Linux 카탈로그 스키마 정의 (`linux_catalog.json`) 및 공식 미러 해시 검증 파서 구현
* **Phase 2**: `ext4` 포맷터(`mkfs.ext4`) 및 3대 포맷 추출기(`Single-SquashFS`, `Layered-SquashFS`, `EROFS`) 모듈 구현
* **Phase 3**: 3대 계열별(`Debian`, `RedHat`, `Arch`) `chroot` 시스템 초기화(`fstab`, 계정 생성, GRUB UEFI 설치) 모듈 및 단위 테스트 작성
* **Phase 4**: 상단 탭 전환 + 단일 리스트 TUI 화면 연동 및 통합 검증
