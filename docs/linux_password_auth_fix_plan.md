# 리눅스 비밀번호 인증 실패(Login incorrect) 해결 및 무결성 계정 주입 구현 계획

## 1. 문제 분석 및 근본 원인 (Root Cause)

### 1.1 현상
- 우분투 26.04 LTS 부팅 완료 후 텍스트 콘솔 로그인 프롬프트(`login:`)에서 설치 시 입력한 아이디와 비밀번호를 입력하면 `Login incorrect` 오류가 발생하며 로그인이 거부됨.
- `root` 계정으로 동일한 비밀번호를 입력해도 로그인이 거부됨.

### 1.2 근본 원인
1. **chroot 환경에서의 PAM 인증 토큰 오류**:
   - 우분투/데비안의 기본 `chpasswd` 유틸리티는 PAM(`pam_chauthtok`)을 호출하여 비밀번호를 암호화하고 저장하도록 컴파일되어 있습니다.
   - chroot 환경 내부에서는 활성화된 PAM 세션, D-Bus, systemd-logind 등이 부재하여 `Authentication token manipulation error`가 발생합니다.
2. **`useradd` 기본 동작과 계정 잠금(`!`)**:
   - `useradd`로 계정을 생성하면 보안 기본값으로 `/etc/shadow`의 패스워드 필드가 `!`(계정 잠금)로 기록됩니다.
   - 이후 호출된 `chpasswd`가 오류로 실패했음에도 명령어 체인의 `|| true`로 인해 오류가 무시되었고, `/etc/shadow`는 여전히 `!` 잠금 상태로 방치되었습니다.
3. **`root` 비밀번호 미설정**:
   - 동일 체인(`&& echo "root:..." | chpasswd`)에 묶여 있던 root 비밀번호 설정 역시 앞선 명령 실패로 실행되지 못했습니다.

---

## 2. 해결 전략: PAM 우회 직접 해시 주입 (Direct Hash Injection)

리눅스 계정 인증의 표준이자 가장 안전한 방식은 **PAM 의존성을 완전히 배제하고 `/etc/shadow`에 직접 검증된 해시를 주입**하는 것입니다.

### 핵심 메커니즘
1. **호스트 환경에서 안전한 SHA-512 해시 생성**:
   - LiteWin 호스트 환경(이미 OpenSSL 라이브러리 탑재)에서 비밀번호를 단방향 솔트 암호화(`openssl passwd -6`)하여 표준 glibc 호환 `$6$salt$hash` 문자열을 생성합니다.
2. **`usermod -p`로 shadow 파일에 직접 기록**:
   - `usermod -p '<hash>' <user>` 및 `usermod -p '<hash>' root`는 PAM을 전혀 거치지 않고 shadow 데이터베이스 파일의 2번째 필드에 암호화된 해시를 직접 기록합니다.
   - 계정의 `!` 잠금이 즉시 해제되며 올바른 해시가 주입됩니다.
3. **이중 안전장치 (`chpasswd -c SHA512`)**:
   - 보조 수단으로 chroot 내부에서 `echo '<user>:<pass>' | chpasswd -c SHA512`를 실행하여 crypt 라이브러리 직통 방식을 병행합니다.
4. **`/etc/shadow` 파일 권한 및 소유권 강제 정상화**:
   - `chmod 0640 /etc/shadow` 및 `chown root:shadow /etc/shadow`를 보장합니다.

---

## 3. 세부 코드 수정 계획

### 3.1 대상 파일 및 함수
- **대상 파일**: [`src/platform/linux_installer.cpp`](file:///home/yskyh06/linux-workspace/litewin/src/platform/linux_installer.cpp)
- **대상 함수**:
  1. [`LinuxInstaller::build_chroot_commands`](file:///home/yskyh06/linux-workspace/litewin/src/platform/linux_installer.cpp#L248-L255)
  2. [`LinuxInstaller::update_installed_user_credentials`](file:///home/yskyh06/linux-workspace/litewin/src/platform/linux_installer.cpp#L640-L650)

### 3.2 구현 상세

#### 1) `build_chroot_commands` 개선
호스트에서 비밀번호가 주어졌을 때 OpenSSL로 SHA-512 해시를 계산하거나, chroot 명령 내에서 원라인으로 해시를 생성하여 `usermod -p`에 전달:
```bash
# SHA-512 해시 생성 및 주입 (PAM 완전 배제)
PW_HASH=$(openssl passwd -6 '<escaped_password>')
usermod -p "$PW_HASH" '<username>'
usermod -p "$PW_HASH" root
usermod -U '<username>' 2>/dev/null || true
echo '<username>:<escaped_password>' | chpasswd -c SHA512 2>/dev/null || true
echo 'root:<escaped_password>' | chpasswd -c SHA512 2>/dev/null || true
chmod 0640 /etc/shadow && chown root:shadow /etc/shadow 2>/dev/null || chown root:root /etc/shadow 2>/dev/null || true
```

#### 2) `update_installed_user_credentials` 개선
설치 후 TUI 실시간 입력 변경 시에도 동일하게 `ProcessRunner::run("openssl", {"passwd", "-6", opts.password})`로 생성한 해시를 `usermod -p`를 통해 안전하게 적용.

---

## 4. 검증 계획

1. **단위 테스트 (`tests/linux_installer_tests.cpp`)**:
   - `build_chroot_commands` 결과에 `usermod -p` 및 `chpasswd -c SHA512`가 포함되어 있는지 검증.
   - `test_linux_installer_tests` 전체 89개 테스트 통과 확인.
2. **부팅 ISO 재빌드 및 체크섬 생성**:
   - `./scripts/build_iso_docker.sh` 실행하여 새 ISO 패키징.
3. **실제 로그인 검증**:
   - 생성된 ISO로 Ubuntu 26.04 LTS 설치 후, 설정한 아이디와 비밀번호로 `login:` 프롬프트에서 로그인 및 `sudo -i` 성공 여부 확인.

---

## 5. 실행 절차
1. 사용자 승인 후 코드 수정 착수
2. [`src/platform/linux_installer.cpp`](file:///home/yskyh06/linux-workspace/litewin/src/platform/linux_installer.cpp) 수정
3. [`tests/linux_installer_tests.cpp`](file:///home/yskyh06/linux-workspace/litewin/tests/linux_installer_tests.cpp) 수정
4. `cmake` 빌드 및 `ctest` 단위 테스트 실행
5. 부팅 ISO 재빌드 및 결과 보고
