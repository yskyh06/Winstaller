# ISO 부팅 목록 미표시 원인 분석 및 추가 잠재 버그 예측·`grub.cfg` 전면 개선안

## 1. 현재 발생한 문제 요약: 왜 ISO로 부팅해도 ISO 부팅 목록(`Winstaller`)이 뜨지 않는가?

1. **[`packaging/archiso/grub/grub.cfg`](file:///home/yskyh06/linux-workspace/litewin/packaging/archiso/grub/grub.cfg#L12-L33)의 `boot_stage.flag` 자동 핸드오프(`configfile`)가 ISO 메뉴를 덮어씀**:
   - 이전 Ubuntu 설치 시 내장 디스크의 EFI 파티션에 `/EFI/Litewin/boot_stage.flag`와 구버전 `/EFI/ubuntu/grub.cfg`가 생성되어 영구히 남아 있습니다.
   - 새 ISO로 부팅하면 ISO의 `grub.cfg`가 내장 디스크의 `boot_stage.flag`를 감지하여 기본 선택값(`default`)을 `winstaller-ram`에서 `ubuntu-handoff`로 강제 변경하고, `configfile ($internal_esp)/EFI/ubuntu/grub.cfg`를 자동 실행합니다.
   - GRUB의 `configfile` 명령어는 **현재 ISO의 GRUB 메뉴(`Winstaller`)를 통째로 삭제하고 내장 디스크의 구버전 `grub.cfg` 메뉴로 교체**하므로, ISO 부팅 메뉴 목록이 화면에서 완전히 사라지고 이전 디스크의 `grub>` 프롬프트로 빠지게 됩니다.
2. **`efibootmgr`에 의한 UEFI NVRAM 부팅 1순위 변경**:
   - 이전 설치의 `InstallBootloader` 단계에서 `efibootmgr -o`로 메인보드/VM의 UEFI 부팅 1순위를 내장 디스크(`Ubuntu`)로 고정했기 때문에, UEFI 부트 매니저에서 CD-ROM/USB를 직접 선택하지 않으면 ISO를 거치지 않고 내장 디스크의 이전 GRUB(`grub>`)으로 바로 진입합니다.

---

## 2. 그 외 발생할 수 있는 모든 잠재적 GRUB / 부팅 버그 예측 (총 7가지)

| ID | 발생 위치 | 예상 버그 시나리오 | 치명도 | 예방 및 해결책 |
| :--- | :--- | :--- | :---: | :--- |
| **Bug 1** | ISO [`packaging/archiso/grub/grub.cfg`](file:///home/yskyh06/linux-workspace/litewin/packaging/archiso/grub/grub.cfg#L12-L33) | 내장 디스크에 `boot_stage.flag`가 남아있을 때 `default="ubuntu-handoff"`로 바뀌고 `configfile`이 실행되어 ISO 부팅 메뉴가 증발함 | **Critical** | `set default="winstaller-ram"` 및 `set timeout=5`를 항상 고정하고, `Winstaller` 메뉴를 최상단(1번)에 배치. 내장 디스크 부팅은 하단 보조 메뉴로만 제공 |
| **Bug 2** | ISO [`packaging/archiso/grub/grub.cfg`](file:///home/yskyh06/linux-workspace/litewin/packaging/archiso/grub/grub.cfg#L35-L50) | `search --set=...` 실행 또는 다른 메뉴 진입 실패 후 `Winstaller` 선택 시 `$root` 변수가 내장 디스크로 바뀐 상태여서 `/%INSTALL_DIR%/boot/x86_64/vmlinuz-linux` 파일을 찾지 못함 (`file not found`) | **High** | `winstaller-ram` 메뉴 내부에서 `ARCHISO_HINT` 또는 `search --label --set=root %ARCHISO_LABEL%` 및 `search --file`로 `$root`를 반드시 ISO 볼륨으로 재고정한 뒤 `linux`/`initrd` 로드 |
| **Bug 3** | ISO [`packaging/archiso/grub/grub.cfg`](file:///home/yskyh06/linux-workspace/litewin/packaging/archiso/grub/grub.cfg#L1-L7) | `all_video`, `gfxterm`, `ext2`, `gzio`, `iso9660`, `test` 모듈 미로드 및 `echo` 내 유니코드 특수문자(`┌─┐`)로 인해 일부 UEFI 펌웨어에서 메뉴 화면이 깨지거나 검은 화면으로 멈춤 | **High** | 필수 GRUB 모듈(`all_video`, `ext2`, `gzio`, `iso9660`, `test`, `search_fs_uuid`, `search_label`)을 명시적으로 로드하고, `echo` 출력 박스를 순수 ASCII(`+---+`)로 변경 |
| **Bug 4** | 설치된 OS의 `grubx64.efi` / `BOOTX64.EFI` ([`src/platform/linux_installer.cpp`](file:///home/yskyh06/linux-workspace/litewin/src/platform/linux_installer.cpp#L1181-L1198)) | 호스트 `grub-install` 폴백으로 생성된 `grubx64.efi`가 `$prefix`를 찾기 전에 `if [ ... ]` (`test.mod`)나 `search` (`search.mod`), `zstd` (`zstd.mod` - Ubuntu 24.04/26.04 커널/initrd 압축해제용) 모듈이 내장되어 있지 않아 명령어 실행 실패 | **Critical** | `grub-install` 실행 시 `--modules="part_gpt part_msdos fat ext2 normal boot linux configfile loopback chain efifwsetup efi_gop efi_uga all_video gfxterm gettext echo true loadenv search search_label search_fs_uuid search_fs_file test gzio xzio zstd"` 옵션을 명시하여 모든 필수 모듈을 `.efi` 바이너리 내부에 직접 내장 |
| **Bug 5** | 설치된 OS의 `/EFI/ubuntu/grub.cfg` 및 `/boot/grub/grub.cfg` | `if [ -f /boot/... ]` 조건문이 `test.mod` 미로드 환경이나 GRUB의 ext4 심볼릭 링크 판별 버그로 인해 false를 반환하여 커널 로드를 건너뛰고 빈 화면/메뉴로 복귀함 | **High** | 조건문(`if [ -f ... ]`)에만 의존하지 않고, 탐지된 실제 물리 커널 파일(`/boot/vmlinuz-<kver>`)을 기본 메뉴에서 **조건문 없이 직접 `linux` / `initrd`로 호출**하며, 심볼릭 링크 대신 실제 파일 복사본(또는 하드링크)도 함께 생성 |
| **Bug 6** | 설치된 OS의 `/boot/initrd.img-<kver>` ([`src/platform/linux_installer.cpp`](file:///home/yskyh06/linux-workspace/litewin/src/platform/linux_installer.cpp#L258)) | ISO의 `/casper/initrd`는 라이브 부팅용(casper) initrd이므로, chroot에서 `update-initramfs`가 정확한 커널 버전(`<kver>`)에 대해 재생성되지 않으면 부팅 중 `(initramfs) Unable to find a medium containing a live file system` 오류로 멈춤 | **Critical** | `ConfigureChroot`에서 `for k in /lib/modules/*; do [ -d "$k" ] && update-initramfs -c -k "$(basename "$k")" || update-initramfs -u -k "$(basename "$k")"; done`으로 설치된 커널 버전의 디스크 부팅용 initramfs를 확실히 생성 |
| **Bug 7** | `InstallBootloader`의 `efibootmgr` 부팅 순서 ([`src/platform/linux_installer.cpp`](file:///home/yskyh06/linux-workspace/litewin/src/platform/linux_installer.cpp#L1140)) | 이전 설치에서 남아있는 `/EFI/Litewin/boot_stage.flag`가 디스크 포맷 전에 간섭하거나, 재설치 시 구버전 파티션이 스캔됨 | **Medium** | `PartitionDisk` 직후 새 EFI 파티션 포맷 시 자연히 초기화되며, ISO `grub.cfg`에서 `boot_stage.flag`가 있어도 ISO 부팅(`winstaller-ram`)이 1순위가 되도록 분리 |

---

## 3. 수정할 `grub.cfg` 상세 설계안

### 3.1. Live ISO용 [`packaging/archiso/grub/grub.cfg`](file:///home/yskyh06/linux-workspace/litewin/packaging/archiso/grub/grub.cfg) (전면 개선안)
- **핵심 변경점**:
  1. `set default="winstaller-ram"`과 `set timeout=5`, `set timeout_style=menu`를 **절대 덮어쓰지 않고 고정**하여 ISO 부팅 시 무조건 `Winstaller` 메뉴가 1순위로 5초간 표시됩니다.
  2. `winstaller-ram` 내부에서 `ARCHISO_HINT` 및 `%ARCHISO_LABEL%` 검색으로 `$root`를 확실히 ISO 디바이스로 설정합니다.
  3. 내장 디스크에 설치된 OS(Windows / Ubuntu)가 있을 경우 메뉴 하단에 보조 엔트리로 추가하되, Ubuntu 핸드오프 시 `($linux_root)/boot/grub/grub.cfg`를 우선 로드합니다.

```grub
insmod part_gpt
insmod part_msdos
insmod fat
insmod ext2
insmod ntfs
insmod iso9660
insmod search
insmod search_fs_file
insmod search_fs_uuid
insmod search_label
insmod chain
insmod configfile
insmod normal
insmod linux
insmod gzio
insmod xzio
insmod zstd
insmod test
insmod all_video
insmod gfxterm

set timeout_style=menu
set timeout=5
set default="winstaller-ram"

menuentry "Winstaller - Automated Windows & Linux Installer (Live ISO)" --class arch --class gnu-linux --class os --id 'winstaller-ram' {
    set gfxpayload=keep
    if [ -n "${ARCHISO_HINT}" ]; then
        set root="${ARCHISO_HINT}"
    fi
    if [ ! -e /%INSTALL_DIR%/boot/x86_64/vmlinuz-linux ]; then
        search --no-floppy --label --set=root %ARCHISO_LABEL%
    fi
    if [ ! -e /%INSTALL_DIR%/boot/x86_64/vmlinuz-linux ]; then
        search --no-floppy --file --set=root /%INSTALL_DIR%/boot/x86_64/vmlinuz-linux
    fi
    echo ""
    echo "  +--------------------------------------------------------------------------+"
    echo "  |   * LITEWIN / WINSTALLER LIVE ENVIRONMENT                                |"
    echo "  |   [ LOADING SYSTEM... PLEASE WAIT (15-30s) ]                             |"
    echo "  +--------------------------------------------------------------------------+"
    echo ""
    linux /%INSTALL_DIR%/boot/x86_64/vmlinuz-linux archisobasedir=%INSTALL_DIR% archisolabel=%ARCHISO_LABEL% copytoram=y cow_spacesize=4G quiet loglevel=3 audit=0 udev.log_priority=3
    initrd /%INSTALL_DIR%/boot/x86_64/initramfs-linux.img
}

# Optional secondary entries if an installed OS is present on internal disk
search --file --no-floppy --set=internal_esp /EFI/Litewin/boot_stage.flag
if [ -n "$internal_esp" ]; then
    if [ -e ($internal_esp)/EFI/Microsoft/Boot/bootmgfw.efi ]; then
        menuentry "Boot Installed Windows from Internal Disk" --id 'windows-handoff' {
            set root="$internal_esp"
            chainloader /EFI/Microsoft/Boot/bootmgfw.efi
            boot
        }
    fi
    if [ -e ($internal_esp)/EFI/ubuntu/grub.cfg ]; then
        menuentry "Boot Installed Ubuntu / Linux from Internal Disk" --id 'ubuntu-handoff' {
            insmod ext2
            insmod part_gpt
            search --no-floppy --label --set=linux_root ROOT
            if [ -n "$linux_root" -a -e ($linux_root)/boot/grub/grub.cfg ]; then
                set root="$linux_root"
                set prefix=($linux_root)'/boot/grub'
                configfile ($linux_root)/boot/grub/grub.cfg
            else
                set root="$internal_esp"
                configfile ($internal_esp)/EFI/ubuntu/grub.cfg
            fi
        }
    fi
fi
```

---

### 3.2. 설치된 Ubuntu 디스크의 `/boot/grub/grub.cfg` (`build_main_grub_cfg`) 개선안
- **핵심 변경점**:
  1. `if [ -f ... ]` 조건문 실패 가능성을 원천 차단하기 위해, 기본 `menuentry 'Ubuntu'`에서는 설치 시점에 `/boot`에서 확인된 실제 커널 파일(`vmlinuz-<kver>`)과 `initrd.img-<kver>`를 **조건문 없이 직결(`linux /boot/<kfile> ...`, `initrd /boot/<ifile>`)**합니다.
  2.万一을 대비해 두 번째 보조 메뉴(`Ubuntu (Fallback / Generic Kernel)`)에서 `/boot/vmlinuz-generic`으로 부팅할 수 있도록 이중 안전장치를 둡니다.

```grub
# Automatically generated by LiteWin LinuxInstaller
insmod part_gpt
insmod part_msdos
insmod ext2
insmod fat
insmod gzio
insmod xzio
insmod zstd
insmod search
insmod search_fs_uuid
insmod search_label
insmod search_fs_file
insmod test
insmod linux
insmod normal
insmod all_video
insmod gfxterm

set default=0
set timeout_style=menu
set timeout=3

menuentry 'Ubuntu' --class ubuntu --class gnu-linux --class gnu --class os {
    insmod gzio
    insmod zstd
    insmod part_gpt
    insmod ext2
    search --no-floppy --fs-uuid --set=root <ROOT_UUID>
    linux /boot/<DETECTED_VMLINUZ> root=UUID=<ROOT_UUID> ro
    initrd /boot/<DETECTED_INITRD>
}

menuentry 'Ubuntu (Fallback Kernel)' --class ubuntu --class gnu-linux --class gnu --class os {
    insmod gzio
    insmod zstd
    insmod part_gpt
    insmod ext2
    search --no-floppy --label --set=root ROOT
    linux /boot/vmlinuz-generic root=LABEL=ROOT ro
    initrd /boot/initrd.img-generic
}
```

---

### 3.3. 설치된 Ubuntu 디스크의 `/boot/efi/EFI/ubuntu/grub.cfg` (`build_forwarding_grub_cfg`) 개선안
- `/boot/grub/grub.cfg`가 존재하면 `configfile`로 넘기되, 넘어가지 않더라도 조건문 없이 곧바로 `/boot/<DETECTED_VMLINUZ>`를 직접 부팅할 수 있는 `menuentry`를 포함합니다.

```grub
insmod part_gpt
insmod ext2
insmod fat
insmod search
insmod search_fs_uuid
insmod search_label
insmod search_fs_file
insmod configfile
insmod normal
insmod linux
insmod gzio
insmod zstd
insmod test

search --no-floppy --fs-uuid --set=root <ROOT_UUID>
set prefix=($root)'/boot/grub'
if [ -e $prefix/grub.cfg ]; then
    configfile $prefix/grub.cfg
fi

set timeout_style=menu
set timeout=3
menuentry 'Ubuntu Linux (Direct Boot)' --class ubuntu --class gnu-linux --class os {
    insmod gzio
    insmod zstd
    insmod part_gpt
    insmod ext2
    search --no-floppy --fs-uuid --set=root <ROOT_UUID>
    linux /boot/<DETECTED_VMLINUZ> root=UUID=<ROOT_UUID> ro
    initrd /boot/<DETECTED_INITRD>
}
```


---

## 4. [추가] GRUB에서 CD(LiteWin ISO)로 부팅되어 2.7GB 재설치가 시작되는 현상 분석 및 해결안

### 4.1. 원인
1. **GPT 하이브리드 ISO 구조**: `litewin-2026.11-x86_64.iso` 자체가 GPT 파티션(`gpt1`, `gpt2`, `gpt3`)을 가진 부팅 미디어이므로, CD/USB를 제거하지 않고 부팅하면 GRUB/UEFI에서 CD가 첫 번째 디스크로 잡힐 수 있습니다.
2. **기존 설치 디스크에 `/boot/grub/grub.cfg`가 없을 때 `configfile` 핸드오프 실패**:
   - 기존 [`packaging/archiso/grub/grub.cfg`](file:///home/yskyh06/linux-workspace/litewin/packaging/archiso/grub/grub.cfg)는 `configfile`만 시도했기 때문에, 이전 버전으로 설치되어 `/boot/grub/grub.cfg`가 없는 디스크를 부팅시키지 못했습니다.

### 4.2. 개선할 [`packaging/archiso/grub/grub.cfg`](file:///home/yskyh06/linux-workspace/litewin/packaging/archiso/grub/grub.cfg) 설계
- `search --no-floppy --label --set=linux_root ROOT`로 내장 디스크의 우분투 `ROOT` 파티션을 자동 탐색하여, `/boot/grub/grub.cfg`가 없더라도 **`($linux_root)/boot/vmlinuz` + `($linux_root)/boot/initrd.img` + `boot=local`로 즉시 커널을 직접 부팅**하는 `[Internal Disk] Boot Installed Ubuntu Linux (Direct Boot)` 메뉴를 상시 제공합니다.
- 메뉴 이름을 `[Internal Disk] Boot Installed Ubuntu Linux`와 `[Live CD/USB] Winstaller - OS Installer (Reinstall)`로 명확히 구분하여 CD 재설치 진입 혼동을 방지합니다.
