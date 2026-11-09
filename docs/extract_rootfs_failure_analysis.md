# ExtractRootFS 단계 `FATAL ERROR: unlink_file` 원인 규명 및 전체 로직 무결성 재설계

## 1. 발생한 오류 현상
우분투 26.04 Server CLI 설치 중 `ExtractRootFS` 단계에서 실제 `stderr`가 노출되면서 확인된 정확한 오류 메시지:

```text
Failed at step: ExtractRootFS

Image extraction failed:
FATAL ERROR: unlink_file: failed to [delete <path>, because Directory not empty]
Press Enter to Retry, or Esc to Reboot ...
```

---

## 2. `FATAL ERROR: unlink_file`의 정확한 발생 메커니즘 (소스 코드 레벨 분석)

### 2.1 `squashfs-tools` (`unsquashfs.c`)의 `unlink_file()` 동작 구조
`squashfs-tools` (v4.7.5)의 `unsquashfs.c` 내부 `unlink_file()` 코드는 덮어쓰기(`-f`) 시 기존 경로를 삭제할 때 다음과 같이 동작합니다:

```c
int unlink_file(char *pathname)
{
    struct stat buf;
    if (lstat(pathname, &buf) == -1) {
        if (errno == ENOENT) return TRUE;
        EXIT_UNSQUASH("unlink_file: lstat failed ...\n");
    }
    if (S_ISDIR(buf.st_mode))
        res = rmdir(pathname);
    else
        res = unlink(pathname);

    if (res == -1) {
        EXIT_UNSQUASH_STRICT("unlink_file: failed to delete %s, because %s\n",
                             pathname, strerror(errno));
    }
    return TRUE;
}
```

### 2.2 왜 `unlink_file`이 실패했는가? (Layered SquashFS 덮어쓰기 충돌)
우분투 24.04 및 26.04(Desktop/Server)는 단일 SquashFS가 아닌 **OverlayFS용 계층형 SquashFS(Layered SquashFS)** 구조를 사용합니다:
- **Layer 1 (하위 계층)**: `casper/ubuntu-server-minimal.squashfs` (또는 `minimal.squashfs`)
- **Layer 2 (상위 계층)**: `casper/ubuntu-server-minimal.ubuntu-server.squashfs` (또는 `minimal.standard.squashfs`)

기존 `ExtractRootFS` 로직은 동일한 타겟 디렉터리(`/mnt/litewin_linux`)에 `unsquashfs`를 두 번 연속 실행했습니다:
1. **1차 실행**: Layer 1을 `/mnt/litewin_linux`에 압축 해제합니다. 이때 특정 경로(예: `/etc/...`, `/var/...`, `/usr/...`)에 **내부에 파일이 들어 있는 디렉터리(Non-empty Directory)** 또는 읽기 전용 권한(`0555`) 디렉터리가 생성됩니다.
2. **2차 실행**: Layer 2를 동일한 `/mnt/litewin_linux`에 `unsquashfs -f`로 덮어씁니다.
3. **충돌 발생**: Layer 2에서 하위 계층의 디렉터리를 **심볼릭 링크(Symlink), 일반 파일, 또는 삭제 마커인 OverlayFS Whiteout(`c 0:0` 캐릭터 디바이스)**으로 대체하려 할 때, `unsquashfs`는 기존 경로를 지우기 위해 `unlink_file()` -> `rmdir()`를 호출합니다.
4. 그러나 해당 디렉터리 안에는 Layer 1에서 풀린 하위 파일들이 들어 있으므로 `rmdir()`가 **`ENOTEMPTY (Directory not empty)`**로 즉시 실패합니다.

### 2.3 왜 `-no-exit-code` 옵션이 이를 막지 못했는가?
`squashfs-tools`의 에러 매크로 정의(`unsquashfs.h`)는 다음과 같습니다:

```c
#define EXIT_UNSQUASH_STRICT(s, args...) \
    do { \
        if (ignore_errors) \
            ERROR(s, ##args); \
        else \
            EXIT_UNSQUASH(s, ##args); \
    } while(0)
```

- **`-no-exit-code`**: 추출이 끝까지 완료된 후 비치명적 경고(XATTR 미지원 등)가 있었을 때 마지막 종료 코드만 `2` 대신 `0`으로 바꾸는 옵션입니다.
- **`-ignore-errors` (`-ig`)**: `EXIT_UNSQUASH_STRICT`(`unlink_file` 실패 등)가 발생했을 때 즉시 `FATAL ERROR`로 프로세스를 중단(`exit(1)`)하지 않고 경고로 전환하여 계속 진행하게 만드는 옵션입니다.
- 즉, `-ignore-errors` 없이 `-no-exit-code`만 주면 `unlink_file` 실패 시 `EXIT_UNSQUASH_STRICT`가 곧바로 `FATAL ERROR`를 출력하며 `exit(1)`로 강제 종료됩니다.
- 더 근본적으로는, `-ignore-errors`를 주더라도 비어 있지 않은 하위 디렉터리가 삭제되지 않으면 상위 계층의 심볼릭 링크나 화이트아웃이 제대로 반영되지 않아 파일시스템 무결성이 깨집니다.

---

## 3. `ExtractRootFS` 단계 전수 점검: 발생 가능한 모든 잠재 오류 (4대 리스크)

`ExtractRootFS` 로직 전체를 재검토하여 도출한 모든 잠재적 실패 지점은 다음과 같습니다:

| 구분 | 잠재적 오류 원인 | 발생 조건 | 결과 |
| :--- | :--- | :--- | :--- |
| **리스크 1** | **Layered SquashFS 직접 덮어쓰기 충돌 (`unlink_file`)** | Layer 1의 비어있지 않은 디렉터리를 Layer 2의 파일/심볼릭링크/Whiteout(`c 0:0`)이 덮어쓸 때 `rmdir()`가 `ENOTEMPTY` 반환 | `FATAL ERROR: unlink_file: failed to delete ...` 발생 및 추출 중단 |
| **리스크 2** | **Ubuntu 24.04/26.04 커널 레이어(`.generic.squashfs`) 및 레이어 이름 불일치** | 우분투 Live Server ISO는 `ubuntu-server-minimal.ubuntu-server.generic.squashfs` 3번째 레이어에 커널 모듈(`/lib/modules`)을 분리 보관함. 카탈로그에 2개 레이어만 고정된 경우 커널 모듈 누락 또는 ISO 마이너 버전별 레이어 파일명 차이 발생 가능 | 추출 실패 또는 부팅 시 커널 모듈/initramfs 생성 실패 |
| **리스크 3** | **Casper 커널/Initrd 복사 시 읽기 전용 파일/심볼릭 링크 충돌 및 확장자 차이** | `/boot/vmlinuz-*`가 읽기 전용 심볼릭 링크로 이미 존재하거나, 구버전(18.04/20.04) ISO에서 `casper/initrd.lz` 이름을 사용할 때 | `cp` 실패 또는 `initrd` 누락 |
| **리스크 4** | **`[ Retry (Enter) ]` 재시도 시 잔여 루프/오버레이 마운트 충돌** | 이전 시도 실패 시 임시 마운트 디렉터리(`/mnt/litewin_linux_iso`, 오버레이 마운트 등)가 정리되지 않은 경우 | 재시도 시 마운트 또는 파일 추출 실패 |

---

## 4. 완벽한 해결 설계 (Zero-Failure `ExtractRootFS` 아키텍처)

### 해결책 1. `LayeredSquashFS` 추출 방식을 **커널 네이티브 `OverlayFS` 마운트 병합 + 스테이징 폴백**으로 전면 교체
우분투의 공식 설치기(`subiquity`/`curtin`)와 동일하게, 여러 SquashFS 레이어를 `unsquashfs`로 같은 폴더에 억지로 덮어쓰지 않고 **리눅스 커널의 `overlay` 파일시스템(또는 스테이징 병합)**을 사용하여 추출합니다.

1. **1차 방식 (Kernel Native OverlayFS Merge)**:
   - 각 SquashFS 레이어(`layer_0`, `layer_1`, ...)를 읽기 전용 루프 디바이스(`mount -t squashfs -o loop,ro`)로 `/tmp/litewin_sq/l0`, `/tmp/litewin_sq/l1` 등에 마운트합니다.
   - 리눅스 커널의 `overlay` 파일시스템을 읽기 전용 다중 하위 계층 옵션(`mount -t overlay overlay -o ro,lowerdir=<최상위레이어>:...:<최하위레이어> /tmp/litewin_sq/merged`)으로 마운트합니다.
     - 커널 VFS가 Whiteout(`c 0:0`), Opaque 디렉터리(`trusted.overlay.opaque`), 디렉터리→심볼릭 링크 교체, 권한 충돌을 **100% 커널 레벨에서 완벽하게 병합**합니다.
   - 병합된 `/tmp/litewin_sq/merged/.`를 타겟 파티션(`/mnt/litewin_linux/`)으로 `cp -a` 복사한 뒤 마운트를 해제합니다.
2. **2차 폴백 (Staged Layer Merge + `-ignore-errors -no-exit-code`)**:
   - 만약 컨테이너/특수 환경에서 `mount -t overlay`를 사용할 수 없는 경우:
     - Base 레이어는 `unsquashfs -f -ignore-errors -no-exit-code -d '/mnt/litewin_linux'`로 추출합니다.
     - 상위 레이어는 임시 스테이징 폴더(`/mnt/litewin_linux_stage`)에 추출(또는 마운트)한 뒤:
       1. 상위 레이어의 Whiteout(`find ... -type c -size 0`)에 대응하는 타겟 경로를 `rm -rf`로 먼저 제거하고 화이트아웃 파일도 삭제합니다.
       2. 상위 레이어에서는 파일/심볼릭 링크인데 타겟에서는 디렉터리인 경로(또는 그 반대)를 `rm -rf`로 사전 정리합니다.
       3. `cp -af /mnt/litewin_linux_stage/. /mnt/litewin_linux/`로 충돌 없이 병합합니다.

### 해결책 2. 모든 `unsquashfs` 호출에 `-ignore-errors -no-exit-code` 동시 적용
- 단일 SquashFS(`SingleSquashFS`) 및 폴백 추출 시:
  ```bash
  unsquashfs -f -ignore-errors -no-exit-code -d '/mnt/litewin_linux' '<layer.squashfs>'
  ```
- `-ignore-errors`를 추가하여 `EXIT_UNSQUASH_STRICT`로 인한 강제 중단(`FATAL ERROR`)을 원천 차단합니다.

### 해결책 3. 마운트된 ISO 내부 `/casper` 레이어 동적 검증 및 커널 레이어(`.generic.squashfs`) 자동 병합
- `iso_loop_mounted` 상태에서 `ExtractRootFS` 진입 전, 실제 마운트된 ISO의 `/casper` 디렉터리를 스캔합니다:
  1. 설정된 레이어 외에 `<마지막_레이어_이름>.generic.squashfs`(예: `casper/ubuntu-server-minimal.ubuntu-server.generic.squashfs` 또는 `casper/minimal.standard.generic.squashfs`)가 존재하면 자동으로 최상위 레이어로 추가하여 정식 커널 이미지와 `/lib/modules/<kver>`를 온전히 설치합니다.
  2. 만약 카탈로그에 지정된 레이어 파일명이 ISO 내부에 없을 경우, `/casper/filesystem.squashfs` 등 실제 존재하는 SquashFS 체인으로 자동 폴백합니다.

### 해결책 4. 커널 및 Initrd 복사 무결성 강화
- `/boot/vmlinuz-*` 및 `/boot/initrd.img-*` 복사 시:
  - 레이어 추출 과정에서 이미 정식 `/boot/vmlinuz-<kver>`와 `/boot/initrd.img-<kver>`가 생성되었는지 확인하고, 없을 때만 `casper/vmlinuz` 및 `casper/initrd`(또는 `casper/initrd.lz`, `casper/initrd.gz`)에서 복사합니다.
  - 복사 전 대상 경로에 깨진 심볼릭 링크가 있으면 먼저 제거(`rm -f`)하여 `cp` 에러를 방지합니다.
