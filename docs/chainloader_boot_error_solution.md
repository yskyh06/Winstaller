# GRUB Chainloader Boot 126 Unknown Error 원인 분석 및 해결 방안

## 1. 개요 및 오류 현상
우분투(Ubuntu 26.04 Server CLI) 설치 완료 후 재부팅 시 콘솔에 아래와 같은 GRUB 오류 메시지가 출력되며 시스템 부팅이 중단되는 현상이 발생함:

```text
error: loader/efi/chainloader.c:grub_chainloader_boot:126:unknown error.
```

본 문서는 GNU GRUB 소스 코드 레벨에서 해당 오류가 발생하는 정확한 메커니즘을 분석하고, 코드 수정을 진행하기 전 완벽한 해결 방안을 정의한다.

---

## 2. 소스 코드 레벨 분석 (GNU GRUB `chainloader.c`)

GNU GRUB(`grub-core/loader/efi/chainloader.c`)의 `grub_chainloader_boot` 함수 구현은 다음과 같다:

```c
static grub_err_t
grub_chainloader_boot (void)
{
  b = grub_efi_system_table->boot_services;
  status = efi_call_3 (b->start_image, image_handle, &exit_data_size, &exit_data);
  if (status != GRUB_EFI_SUCCESS)
    {
      if (exit_data)
        {
          ...
        }
      return grub_error (GRUB_ERR_BAD_OS, "unknown error"); /* Line 126 */
    }
  return GRUB_ERR_NONE;
}
```

- **Line 126의 의미**: UEFI Boot Services의 `b->start_image(image_handle, ...)` API 호출이 `GRUB_EFI_SUCCESS(0)`가 아닌 오류 코드를 반환했으며, 펌웨어로부터 추가적인 `exit_data` 문자열이 전달되지 않았을 때 GRUB이 기본적으로 출력하는 에러(`GRUB_ERR_BAD_OS, "unknown error"`)이다.
- 즉, **GRUB이 타겟 EFI 파일(`BOOTX64.EFI` 등)을 메모리에 로드한 뒤 UEFI 펌웨어에 실행을 요청(`StartImage`)했으나, 펌웨어(또는 EFI 바이너리) 레벨에서 실행이 거부되거나 즉시 실패하고 반환된 것**이다.

---

## 3. 근본 원인 분석 (Root Causes)

### 원인 1: USB Bootloop Guard의 무조건적 `chainloader` 호출 충돌 (주요 원인)
- 현재 `packaging/archiso/grub/grub.cfg`에 설정된 USB 핸드오프 로직:
  ```grub
  # [Phase 2.2] USB Bootloop Guard (Handoff to internal disk if installation is staged)
  search --file --no-floppy --set=internal_esp /EFI/Litewin/boot_stage.flag
  if [ -n "$internal_esp" ]; then
      set default="windows-handoff"
      set timeout=0
      menuentry "Boot Installed Windows (Auto-Handoff from USB)" --id 'windows-handoff' {
          chainloader ($internal_esp)/EFI/BOOT/BOOTX64.EFI
          boot
      }
  fi
  ```
- **Windows 설치 시**:
  - `BOOTX64.EFI`는 Microsoft의 독립적인 부트 관리자(`bootmgfw.efi`)이므로 GRUB에서 체인로딩이 정상 동작함.
- **Linux/Ubuntu 설치 시**:
  - 타겟 ESP의 `BOOTX64.EFI`는 `shimx64.efi` 또는 `grubx64.efi`임.
  - **이미 실행 중인 GRUB(USB 라이브 환경)에서 또 다른 GRUB(`grubx64.efi`) 또는 Shim을 `chainloader`로 실행하면, UEFI `StartImage`가 부모 GRUB이 점유한 메모리 풀 및 장치 핸들과 충돌하여 `EFI_SECURITY_VIOLATION` 또는 `EFI_LOAD_ERROR`를 반환함.**
  - 특히 GRUB에서 리눅스로 핸드오프할 때는 `chainloader`가 아닌 **`configfile`** 명령어를 사용하여 타겟 디스크의 `grub.cfg`를 직접 로드해야 함.

### 원인 2: Secure Boot / SBAT(Secure Boot Advanced Targeting) 검증 실패
- 타겟 디스크에 복사된 `BOOTX64.EFI`가 Canonical 서명된 `grubx64.efi`이거나 미서명 바이너리인 경우:
  - UEFI Secure Boot가 켜져 있으면 `StartImage()`는 즉시 `EFI_SECURITY_VIOLATION(0x800000000000001A)`을 반환함.
  - Archiso 호스트 GRUB 빌드 시 적용된 SBAT 레벨과 Canonical 우분투 바이너리의 SBAT 레벨 간 충돌이 발생하여 실행이 거부됨.

### 원인 3: 타겟 ESP `BOOTX64.EFI`의 심볼릭 링크 또는 0바이트 파일 결함
- `src/platform/linux_installer.cpp`에서 `/boot/efi/EFI/ubuntu/` 내부의 `shimx64.efi` 또는 `grubx64.efi`를 복사할 때:
  - 최소 설치(Server Minimal) 환경에서는 `shimx64.efi` 패키지가 없어 `grubx64.efi`만 존재하거나,
  - 복사 과정에서 경로 누락 또는 불완전 복사로 유효하지 않은 PE32+ 헤더를 가진 파일이 `BOOTX64.EFI`로 생성되었을 경우 `EFI_UNSUPPORTED` 발생.

---

## 4. 단계별 해결 방안 (Solution Architecture)

### 해결책 1. `packaging/archiso/grub/grub.cfg` 핸드오프 메커니즘 개편
무조건적인 `chainloader` 호출을 제거하고, 설치된 OS 타입에 맞춰 분기하도록 변경:

```grub
# [Phase 2.2] USB Bootloop Guard (Auto-Handoff to Internal Disk)
search --file --no-floppy --set=internal_esp /EFI/Litewin/boot_stage.flag
if [ -n "$internal_esp" ]; then
    set timeout=2

    # 1. Windows 핸드오프 (Microsoft Boot Manager 체인로딩)
    if [ -f ($internal_esp)/EFI/Microsoft/Boot/bootmgfw.efi ]; then
        set default="windows-handoff"
        menuentry "Boot Installed Windows (Auto-Handoff)" --id 'windows-handoff' {
            insmod chain
            chainloader ($internal_esp)/EFI/Microsoft/Boot/bootmgfw.efi
            boot
        }
    fi

    # 2. Ubuntu / Linux 핸드오프 (configfile 직접 호출 - 체인로더 에러 완전 제거)
    if [ -f ($internal_esp)/EFI/ubuntu/grub.cfg ]; then
        set default="ubuntu-handoff"
        menuentry "Boot Installed Ubuntu (Auto-Handoff)" --id 'ubuntu-handoff' {
            configfile ($internal_esp)/EFI/ubuntu/grub.cfg
        }
    elif [ -f ($internal_esp)/EFI/BOOT/grub.cfg ]; then
        set default="linux-handoff"
        menuentry "Boot Installed Linux (Auto-Handoff)" --id 'linux-handoff' {
            configfile ($internal_esp)/EFI/BOOT/grub.cfg
        }
    fi
fi
```

> **핵심 장점**: 리눅스 환경에서는 `chainloader`를 호출하지 않고 GRUB 고유의 `configfile` 명령어로 타겟 디스크의 설정을 그대로 이어받으므로, **UEFI `StartImage` 충돌이나 SBAT 서명 거부가 100% 원천 차단**됨.

---

### 해결책 2. 타겟 디스크 포워딩 `grub.cfg` 스텁 구조 정밀화 (`src/platform/linux_installer.cpp`)
타겟 디스크의 `/boot/efi/EFI/ubuntu/grub.cfg` 및 `/boot/efi/EFI/BOOT/grub.cfg`에 생성되는 포워딩 스텁이 정상적으로 루트 파티션을 찾고 `/boot/grub/grub.cfg`를 로드하도록 보장:

```grub
search.fs_uuid <ROOT_PARTITION_UUID> root
set prefix=($root)'/boot/grub'
configfile $prefix/grub.cfg
```

---

### 해결책 3. UEFI NVRAM `BootOrder` 최우선 설정 (`efibootmgr -o`)
- 설치 완료 후 호스트에서 `efibootmgr`로 신규 생성된 Ubuntu 부트 번호(예: `0001`)를 `BootOrder`의 가장 맨 앞으로 설정:
  ```bash
  efibootmgr -o 0001,0000,0002
  efibootmgr -n 0001
  ```
- 이렇게 구성하면 사용자가 USB를 꽂은 채 재부팅하더라도 메인보드 펌웨어가 USB 대신 설치된 디스크의 Ubuntu 부트로더를 1순위로 즉시 직접 실행하므로 USB GRUB을 거칠 필요가 없음.

---

### 해결책 4. `BOOTX64.EFI` 생성 무결성 검증
- `src/platform/linux_installer.cpp`의 `InstallBootloader` 단계에서:
  1. `shimx64.efi`가 존재하면 최우선으로 `BOOTX64.EFI`로 복사 (Secure Boot 호환성 확보).
  2. 없으면 `grubx64.efi`를 복사.
  3. 복사된 `BOOTX64.EFI`의 파일 크기가 0보다 큰지(`size > 0`) 파일 검증 수행.
  4. 호스트의 `grub-install --removable` 실행 시 생성된 바이너리와 무결성 교차 검증.

---

## 5. 작업 계획 요약 (추후 사용자 승인 시 진행)
1. **`packaging/archiso/grub/grub.cfg` 수정**:
   - `chainloader` 단일 호출을 `bootmgfw.efi` 체인로딩(Windows)과 `configfile` 로딩(Ubuntu/Linux)으로 명확히 분기.
   - `timeout=0`으로 인한 무한 에러 루프를 방지하기 위해 `timeout=2` 및 대체 메뉴 보존.
2. **`src/platform/linux_installer.cpp` 수정**:
   - `BOOTX64.EFI` 유효성 검증 로직 강화.
   - `efibootmgr -o`로 `BootOrder` 최우선 등록 보장.
3. **단위 테스트 업데이트 및 검증**:
   - `tests/linux_installer_tests.cpp` dry-run 검증.
4. **신규 부팅 ISO 빌드 및 최종 부팅 검증**.
