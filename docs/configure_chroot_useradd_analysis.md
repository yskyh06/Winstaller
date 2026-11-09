# ConfigureChroot 단계 `useradd -m -s /bin/bash 'User'` 오류 원인 분석 및 해결 방안

## 1. 발생한 오류 현상
`ExtractRootFS` 단계가 정상 통과된 후, 다음 단계인 `ConfigureChroot` 진입 시 아래와 같은 오류가 발생하며 중단됨:

```text
Failed at step: ConfigureChroot

Chroot config command failed: useradd -m -s /bin/bash 'User'

Press Enter to Retry, or Esc to Reboot ...
```

---

## 2. 근본 원인 분석 (Root Causes)

### 원인 1 (핵심 직접 원인): Debian/Ubuntu `useradd`의 대문자 포함 계정명(`'User'`) 거부 (Exit Code 3)
1. **TUI 기본값**: [`src/ui/winstaller_tui.hpp`](file:///home/yskyh06/linux-workspace/litewin/src/ui/winstaller_tui.hpp) 18행에서 `TuiUserData::username`의 초기값이 Windows 무인 설치 표준인 **`"User"`(첫 글자 대문자 `U`)**로 설정되어 있습니다.
2. **파이프라인 전달**: [`src/main.cpp`](file:///home/yskyh06/linux-workspace/litewin/src/main.cpp) 387행에서:
   ```cpp
   lopts.username = user_data.username.empty() ? "user" : user_data.username;
   ```
   `user_data.username`이 비어 있지 않고 `"User"`이므로, 대문자가 포함된 `"User"`가 그대로 `LinuxInstaller`에 전달되었습니다.
3. **Ubuntu `useradd`의 엄격한 소문자 정책 (`chkname.c`)**:
   - 우분투(특히 Ubuntu 24.04 / 26.04의 `shadow` 패키지)의 `useradd`는 POSIX/Debian 이름 규칙인 **`^[a-z_][a-z0-9_-]*[$]?$`(영문 소문자 시작 및 소문자만 허용)**를 기본 강제합니다.
   - 대문자 `U`가 포함된 `'User'`를 실행하면 우분투의 `useradd`는:
     ```text
     useradd: invalid user name 'User': use --badname to ignore
     ```
     를 출력하고 **Exit Code 3 (`E_BAD_ARG`)**으로 즉시 실패합니다.

---

### 원인 2: 기존 사용자 계정(예: `ubuntu`) 존재 시 `useradd` 중복 실패 (Exit Code 9)
- 만약 사용자가 TUI에서 계정명을 `ubuntu`로 입력하거나 재시도할 경우, SquashFS 이미지 내부 `/etc/passwd`에 이미 `ubuntu` 계정이 존재하면 `useradd -m -s /bin/bash 'ubuntu'`는 **Exit Code 9 (`E_NAME_IN_USE`: `user already exists`)**를 반환하며 실패합니다.
- `build_chroot_commands()`에서는 계정 존재 여부(`id -u`)를 확인하지 않고 무조건 `useradd`를 호출하고 있었습니다.

---

### 원인 3: 비밀번호 미입력(기본 빈 값) 시 계정 잠금(`!`)으로 인한 부팅 후 로그인 불가
- TUI에서 비밀번호를 입력하지 않으면 `opts.password`는 빈 문자열(`""`)입니다.
- 현재 [`src/platform/linux_installer.cpp`](file:///home/yskyh06/linux-workspace/litewin/src/platform/linux_installer.cpp) 235행은 `!opts.password.empty()`일 때만 `chpasswd`를 실행합니다.
- 리눅스에서 `useradd` 직후 비밀번호를 설정하지 않으면 `/etc/shadow`에 `!`(Locked Account)가 기록되어, **설치가 끝나고 부팅하더라도 CLI 로그인 프롬프트에서 로그인이 영구 거부(`Login incorrect`)**됩니다.
- 비밀번호가 비어 있을 때는 반드시 `passwd -d <username>`으로 계정 잠금을 해제해야 합니다.

---

### 원인 4: 설치 중/완료 후 TUI에서 수정한 계정 정보 반영(`update_installed_user_credentials`) 누락 버그
- 사용자가 설치 진행 중 또는 완료 직후 카운트다운 화면에서 계정명/비밀번호/호스트명을 수정하면, [`src/main.cpp`](file:///home/yskyh06/linux-workspace/litewin/src/main.cpp) 464행에서 `LinuxInstaller::update_installed_user_credentials(lopts)`를 호출합니다.
- 그러나 `LinuxInstaller::execute()`의 마지막 단계(`UnmountVolumes`)에서 이미 `/mnt/litewin_linux`와 `/dev`, `/proc`, `/sys`가 모두 언마운트된 상태이므로, `update_installed_user_credentials()` 내부의 `chroot /mnt/litewin_linux ...` 호출이 빈 마운트 포인트에서 실행되어 실패하고 있었습니다.

---

### 원인 5: `ConfigureChroot` 내 후속 명령(`update-initramfs -c -k all`)의 잠재적 실패
- `useradd` 바로 다음에 실행되는 `update-initramfs -c -k all`의 경우, `ExtractRootFS`에서 이미 `/boot/initrd.img-<kver>` 파일이 생성되어 있으면 `-c`(Create new) 옵션이 기존 파일 존재로 인해 실패하거나, 최소 서버 이미지에서 `initramfs-tools` 대신 기본 `initrd`만 사용하는 경우 불필요한 에러를 반환할 수 있습니다.
- `update-initramfs -u -k all || update-initramfs -c -k all || true` 형태로 안전하게 처리해야 합니다.

---

## 3. 해결 방안 (Solution Architecture)

### 해결책 1. 리눅스 사용자명 소문자 정규화 및 `useradd` 안전 래핑
1. **사용자명 소문자 변환(Sanitization)**:
   - 리눅스 파이프라인에서는 입력된 `opts.username`(예: 기본값 `"User"` 또는 `"Admin"`)의 영문 대문자를 자동으로 소문자(`"user"`, `"admin"`)로 정규화하거나, `useradd` 호출 시:
     ```bash
     id -u 'user' >/dev/null 2>&1 || useradd -m -s /bin/bash 'user' || useradd --badname -m -s /bin/bash 'user' || useradd --badnames -m -s /bin/bash 'user'
     ```
     로 실행하여:
     - 이미 존재하는 계정이면 `useradd`를 건너뛰고 홈 디렉터리와 셸만 보장(`usermod -s /bin/bash` 및 `mkdir -p /home/<user> && chown -R <user>:<user> /home/<user>`).
     - 신규 계정이면 표준 소문자 계정 생성 및 `--badname`/`--badnames` 폴백까지 이중 방어합니다.

### 해결책 2. 빈 비밀번호(`opts.password.empty()`) 시 계정 잠금 해제
- 비밀번호가 지정된 경우:
  ```bash
  echo 'user:password' | chpasswd
  ```
- 비밀번호가 비어 있는 경우(기본값):
  ```bash
  passwd -d 'user' || usermod -p '' 'user' || true
  ```
  을 실행하여 부팅 후 비밀번호 없이도 즉시 로그인 가능하도록 보장합니다.

### 해결책 3. `ConfigureChroot` 후속 명령 방어 및 상세 `stderr` 에러 로깅
- `usermod -aG sudo <user> || true`
- `update-initramfs -u -k all || update-initramfs -c -k all || true`
- `ConfigureChroot` 명령 실패 시 `proc.stderr_output`을 에러 메시지에 함께 포함하여 원인을 명확히 표시합니다.

### 해결책 4. `update_installed_user_credentials()`에서 타겟 루트 파티션 자동 재마운트 후 반영
- 설치 완료 후 TUI에서 변경된 계정/비밀번호/호스트명을 반영할 때, 타겟 루트 파티션(`root_part`)이 언마운트 상태이면 임시로 마운트(`/dev` 바인드 포함)한 뒤 계정 생성·`sudo` 그룹 추가·비밀번호 설정을 수행하고 다시 안전하게 언마운트합니다.
