# LiteWin v1.1 Linux UEFI 부팅 불가 원인 분석 및 종합 해결 방안

## 1. 개요 (Overview)
LiteWin v1.1에서 Linux(Ubuntu, Debian, Fedora, Rocky, AlmaLinux, Arch) 설치 파이프라인이 100% 완료된 후 시스템을 재부팅했을 때, 메인보드 UEFI BIOS에서 설치된 디스크를 부팅 옵션으로 감지하지 못하거나 부팅에 실패하는 현상에 대한 원인 분석 및 세부 해결 방안 보고서입니다.

---

## 2. 핵심 원인 분석 (Root Causes)

### 원인 1: 메인보드 표준 UEFI Fallback 부트로더(`BOOTX64.EFI`) 누락
- **현상**:
  - `grub-install` 실행 시 `--removable` 플래그가 누락되어 있습니다.
  - 현재 코드는 `--bootloader-id=linux` 또는 `--bootloader-id=Arch`만 사용하여 `/boot/efi/EFI/<id>/grubx64.efi`에만 바이너리를 설치하고 메인보드 NVRAM 등록(`efibootmgr`)에만 의존합니다.
- **문제점**:
  - 실제 PC 메인보드(ASUS, MSI, Gigabyte, Lenovo, Dell, HP 등) 및 가상머신(VMware, VirtualBox, QEMU)의 약 80% 이상은 라이브 세션에서 생성된 임시 NVRAM 부팅 엔트리를 무시하거나 신뢰하지 않습니다.
  - UEFI 사양의 표준 Fallback 경로인 **`/EFI/BOOT/BOOTX64.EFI`** 가 ESP 파티션에 없으면 BIOS가 해당 드라이브를 "부팅 불가능(No bootable device)"으로 처리하여 부팅 메뉴에 표시하지 않습니다.

### 원인 2: RHEL / Fedora 계열 부트로더 설치 명령(`grub2-install`) 완전 누락
- **현상**:
  - `src/platform/linux_installer.cpp`의 `LinuxBootFamily::RedHat` 어댑터에 부트로더 설치 코드가 누락되어 있습니다.
  ```cpp
  case LinuxBootFamily::RedHat:
      commands.push_back("dnf remove -y 'anaconda*' livesys-scripts || true");
      commands.push_back("usermod -aG wheel " + shell_quote(opts.username));
      commands.push_back("touch /.autorelabel");
      commands.push_back("dracut --regenerate-all --force");
      commands.push_back("grub2-mkconfig -o /boot/grub2/grub.cfg"); // <-- grub2-install이 없음!
      break;
  ```
- **문제점**:
  - `grub2-mkconfig`만 실행하고 `grub2-install`(또는 EFI 바이너리 복사)을 실행하지 않아, Fedora/Rocky/AlmaLinux 설치 시 `/boot/efi` 파티션에 `.efi` 부트로더 파일이 전혀 생성되지 않습니다.

### 원인 3: Live ISO 루트 파일시스템 추출 후 커널/initramfs 누락 (Ubuntu / Debian)
- **현상**:
  - Ubuntu 데스크톱/서버 ISO의 `casper/filesystem.squashfs`나 Debian Live의 `live/filesystem.squashfs`를 압축 해제하면, `/boot` 디렉토리에 커널(`vmlinuz-*`)이 포함되어 있지 않거나 initramfs가 live 환경용으로만 남아 있는 경우가 많습니다.
  - 라이브 ISO의 커널과 initrd는 squashfs 외부(`casper/vmlinuz`, `casper/initrd`)에 보관되기 때문입니다.
- **문제점**:
  - `update-grub` / `grub-mkconfig`가 실행될 때 `/boot/vmlinuz-*`를 검색하지만 실제 커널 파일이 없으면 **GRUB 메뉴 엔트리가 0개인 빈 `grub.cfg`** 가 생성됩니다.
  - 이 상태로 부팅하면 `grub>` 프롬프트로 떨어지거나 부팅할 OS가 없다는 에러가 발생합니다.

### 원인 4: 호스트 라이브 USB 재부팅 시 USB Handoff 부재 (부팅 루프)
- **현상**:
  - 10초 카운트다운 후 재부팅될 때 설치 미디어인 USB 드라이브가 계속 장착되어 있습니다.
  - `packaging/archiso/grub/grub.cfg`의 USB Bootloop Guard는 Windows 전용 플래그(`/EFI/Litewin/boot_stage.flag`)만 검사합니다.
- **문제점**:
  - 리눅스 설치 완료 후 재부팅 시 USB가 먼저 부팅 순서로 잡히면 설치된 리눅스로 넘어가지 않고 다시 LiteWin 라이브 환경으로 진입하게 됩니다.

### 원인 5: `chroot` 내부 `blkid` 실행 실패로 인한 `/etc/fstab` 오설정
- **현상**:
  - chroot 내부에서 `ROOT_UUID=$(blkid -s UUID -o value /dev/nvme0n1p2)`를 호출합니다.
- **문제점**:
  - chroot 내부에는 udev 데몬이 실행 중이지 않아 `blkid` 캐시가 비어 있거나 실패하여 디바이스 경로(`/dev/nvme0n1p2`)로 fallback 작성됩니다.
  - 시스템 부팅 시 디스크 컨트롤러 열거 순서가 바뀌거나 드라이브 명칭이 변경되면 루트 파티션을 마운트하지 못해 `emergency mode`로 진입합니다.

---

## 3. 종합 해결 방안 (Actionable Solutions)

### 해결책 1: 모든 배포판 공통 Fallback 부트로더(`BOOTX64.EFI`) 의무화
1. `grub-install` 실행 시 반드시 `--removable` 옵션을 부여합니다.
   - 예: `grub-install --target=x86_64-efi --efi-directory=/boot/efi --bootloader-id=BOOT --removable`
2. 설치 직후 안전망으로 ESP 내 생성된 모든 EFI 바이너리를 `/boot/efi/EFI/BOOT/BOOTX64.EFI`로 강제 동기화합니다.
   ```bash
   mkdir -p /boot/efi/EFI/BOOT
   if [ -f /boot/efi/EFI/*/*.efi ]; then
       cp -f $(find /boot/efi/EFI -name '*.efi' -not -path '*/EFI/BOOT/*' | head -n 1) /boot/efi/EFI/BOOT/BOOTX64.EFI || true
   fi
   ```
3. GRUB 설정 파일(`grub.cfg`)을 `/boot/grub/grub.cfg`뿐만 아니라 `/boot/efi/EFI/BOOT/grub.cfg` 및 `/boot/efi/EFI/*/grub.cfg` 위치에도 복사하여 어떤 경로로 진입하든 부팅 메뉴가 즉시 표시되도록 보장합니다.

### 해결책 2: 배포판 패밀리별 부트로더 및 커널 완벽화

#### A. Debian / Ubuntu 패밀리
```bash
# 1. 라이브 잔재 패키지 제거
apt-get purge -y --ignore-missing casper 'live-boot*' 'calamares*' || true

# 2. 커널 및 initramfs 검증 (ISO에서 casper/vmlinuz, casper/initrd를 /boot/로 복사하거나 커널 패키지 보장)
if [ ! -f /boot/vmlinuz* ] && [ -f /iso_mount/casper/vmlinuz ]; then
    cp -f /iso_mount/casper/vmlinuz /boot/vmlinuz-generic
    cp -f /iso_mount/casper/initrd /boot/initrd.img-generic
fi
update-initramfs -c -k all || true

# 3. UEFI 부트로더 및 폴백 생성
grub-install --target=x86_64-efi --efi-directory=/boot/efi --bootloader-id=ubuntu --removable
update-grub
mkdir -p /boot/efi/EFI/BOOT
cp -f /boot/efi/EFI/ubuntu/grubx64.efi /boot/efi/EFI/BOOT/BOOTX64.EFI || true
cp -f /boot/grub/grub.cfg /boot/efi/EFI/BOOT/grub.cfg || true
```

#### B. RedHat / Fedora / Rocky / AlmaLinux 패밀리
```bash
# 1. 라이브 패키지 제거
dnf remove -y 'anaconda*' livesys-scripts || true

# 2. SELinux relabel 플래그 및 initramfs 갱신
touch /.autorelabel
dracut --regenerate-all --force

# 3. 부트로더 설치 (누락된 grub2-install 추가)
grub2-install --target=x86_64-efi --efi-directory=/boot/efi --bootloader-id=fedora --removable || \
grub-install --target=x86_64-efi --efi-directory=/boot/efi --bootloader-id=fedora --removable || true

# 4. grub.cfg 생성 (RHEL 8/9 및 Fedora 호환 경로 모두 갱신)
grub2-mkconfig -o /boot/grub2/grub.cfg
grub2-mkconfig -o /boot/efi/EFI/BOOT/grub.cfg || true

# 5. 폴백 BOOTX64.EFI 보장
mkdir -p /boot/efi/EFI/BOOT
cp -f $(find /boot/efi/EFI -name '*.efi' -not -path '*/EFI/BOOT/*' | head -n 1) /boot/efi/EFI/BOOT/BOOTX64.EFI || true
```

#### C. Arch Linux 패밀리
```bash
# 1. 서비스 정리
rm -f /etc/systemd/system/multi-user.target.wants/pacman-init.service /etc/systemd/system/choose-mirror.service

# 2. 커널 위치 보장
if [ ! -f /boot/vmlinuz-linux ] && [ -f /usr/lib/modules/*/vmlinuz ]; then
    cp -f /usr/lib/modules/*/vmlinuz /boot/vmlinuz-linux
fi

# 3. initramfs 빌드
mkinitcpio -P

# 4. 부트로더 설치 및 폴백 보장
grub-install --target=x86_64-efi --efi-directory=/boot/efi --bootloader-id=Arch --removable
grub-mkconfig -o /boot/grub/grub.cfg
mkdir -p /boot/efi/EFI/BOOT
cp -f /boot/efi/EFI/Arch/grubx64.efi /boot/efi/EFI/BOOT/BOOTX64.EFI || true
cp -f /boot/grub/grub.cfg /boot/efi/EFI/BOOT/grub.cfg || true
```

### 해결책 3: 호스트 환경에서 정확한 파티션 UUID 기반 `/etc/fstab` 생성
- chroot 내부에서 불안정하게 `blkid`를 실행하지 않고, 파이프라인 호스트(`linux_installer.cpp`)에서 마운트 직전 파티션 UUID를 직접 확인하여 `/etc/fstab`을 확정적으로 작성합니다.
  ```cpp
  std::string root_uuid = get_partition_uuid(plan.root_part);
  std::string esp_uuid  = get_partition_uuid(plan.esp_part);
  // fstab 파일 직접 작성:
  // UUID=<root_uuid>  /          ext4   defaults,noatime   0 1
  // UUID=<esp_uuid>   /boot/efi  vfat   umask=0077         0 2
  ```

### 해결책 4: USB Bootloop Guard에 Linux Handoff 플래그 지원 추가
- 설치 완료 시 ESP 파티션에 부팅 플래그를 생성합니다:
  `/boot/efi/EFI/Litewin/boot_stage.flag`
- `packaging/archiso/grub/grub.cfg`에서 이 플래그가 발견되면 USB 재진입 대신 내부 디스크의 `/EFI/BOOT/BOOTX64.EFI`로 자동 체인로딩(Chainload)하도록 구성합니다:
  ```grub
  search --file --no-floppy --set=internal_esp /EFI/Litewin/boot_stage.flag
  if [ -n "$internal_esp" ]; then
      set default="os-handoff"
      set timeout=0
      menuentry "Boot Installed OS (Auto-Handoff from USB)" --id 'os-handoff' {
          chainloader ($internal_esp)/EFI/BOOT/BOOTX64.EFI
          boot
      }
  fi
  ```

---

## 4. 작업 로드맵 (Execution Checklist)

| 번호 | 작업 항목 | 대상 파일 | 검증 방법 |
|---|---|---|---|
| **1** | Host 기반 UUID 추출 및 확정적 `/etc/fstab` 생성 함수 구현 | `src/platform/linux_installer.cpp` | `linux_installer_tests` |
| **2** | 배포판별 `--removable` 옵션 및 `/EFI/BOOT/BOOTX64.EFI` 폴백 복사 로직 추가 | `src/platform/linux_installer.cpp` | `linux_installer_tests` |
| **3** | RHEL 패밀리 `grub2-install` 및 이중 `grub.cfg` 생성 추가 | `src/platform/linux_installer.cpp` | `linux_installer_tests` |
| **4** | Ubuntu/Debian ISO 내 커널/initrd 누락 대비 보정 로직 추가 | `src/core/linux_catalog.cpp`, `src/platform/linux_installer.cpp` | `linux_catalog_tests` |
| **5** | USB Bootloop Guard 리눅스 호환 핸드오프 플래그 등록 | `src/main.cpp`, `packaging/archiso/grub/grub.cfg` | 재부팅 흐름 테스트 |
| **6** | 단위 테스트 갱신 및 전체 CTest 검증 | `tests/linux_installer_tests.cpp` | `ctest --output-on-failure` |
| **7** | 최신 Docker ISO 재빌드 및 실기기 부팅 검증 | `scripts/build_iso_docker.sh` | QEMU/실기기 UEFI 부팅 |
