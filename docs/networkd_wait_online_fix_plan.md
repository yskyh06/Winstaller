# systemd-networkd-wait-online 부팅 지연 해결 및 고속 부팅 구현 계획

## 1. 개요 및 원인 분석

### 1.1 현상
- 우분투 서버(Ubuntu 26.04 LTS / Server Minimal) 부팅 초기 단계에서 아래와 같은 메시지와 함께 시스템 부팅이 멈추거나 수십 초~최대 2분간 지연됨:
  ```text
  A start job is running for Wait for Network to be Configured (... / 2min)
  ```
- 진행 시간이 초과(Timeout: 120s)되거나 네트워크 DHCP 바인딩이 끝날 때까지 `login:` 프롬프트가 표시되지 않음.

### 1.2 발생 원인 (Root Cause)
1. **`systemd-networkd-wait-online.service`의 기본 동작**:
   - 시스템 내에 감지된 모든 네트워크 인터페이스(가상 NIC, 비활성 이더넷 포트 등)가 IP 주소를 할당받아 `routable`(온라인) 상태가 될 때까지 `network-online.target`을 대기시킵니다.
   - 기본 타임아웃은 120초(2분)로 설정되어 있습니다.
2. **단독 디스크/오프라인 환경에서의 대기**:
   - 케이블이 연결되지 않은 포트가 있거나, VM 환경에서 DHCP 응답이 즉시 오지 않는 경우 2분 전체를 대기한 뒤 실패(Failed) 처리되어야 비로소 다음 부팅 단계로 넘어갑니다.
3. **영향 범위**:
   - 이 대기 서비스는 부팅 과정에서 대기할 뿐이며, 실제 네트워크 통신과 DHCP 클라이언트는 백그라운드 데몬(`systemd-networkd` 및 `systemd-resolved`)이 전담합니다.
   - 따라서 대기 서비스를 비활성화하더라도 부팅 후 네트워크 연결, IP 수동/DHCP 할당, 인터넷 사용에는 아무런 부정적 영향이 없습니다.

---

## 2. 해결 방안 및 기술적 비교

| 구분 | 대안 A: 서비스 마스크 (`systemctl mask`) (권장) | 대안 B: 타임아웃 단축 (Drop-in Override) | 대안 C: `--any` 플래그 적용 |
| :--- | :--- | :--- | :--- |
| **방식** | `systemctl mask systemd-networkd-wait-online.service` | `TimeoutStartSec=3s` 설정 | 모든 인터페이스 대신 1개만 연결되면 통과 |
| **장점** | 네트워크 지연 0초, 부팅 즉시 1초 만에 `login:` 진입 | 3초 내에 타임아웃 처리 | 케이블 연결 시 1초 통과 |
| **단점** | `network-online.target`을 엄격히 요구하는 극소수 초기 부팅 서비스(NFS 마운트 등) 미대기 | 미연결 시 3초 멈춤 발생 | 미연결 포트 존재 시 동일 지연 가능 |
| **평가** | **단독 설치 환경 및 Server CLI UX에 최적 (적극 권장)** | 보조 대안 | 부분적 해결 |

---

## 3. 세부 구현 계획

### 3.1 타겟 파일 및 수정 위치
- **대상 파일**: [`src/platform/linux_installer.cpp`](file:///home/yskyh06/linux-workspace/litewin/src/platform/linux_installer.cpp)
- **대상 함수**: [`LinuxInstaller::build_chroot_commands`](file:///home/yskyh06/linux-workspace/litewin/src/platform/linux_installer.cpp#L258-L270) (`LinuxBootFamily::Debian`)

### 3.2 추가 명령어
chroot 환경에서 Debian/Ubuntu 패밀리 설정 시 다음 최적화 명령을 일괄 적용:
```bash
# 1. systemd-networkd 및 NetworkManager 온라인 대기 서비스 무력화 (부팅 지연 0초)
chroot <mountpoint> systemctl mask systemd-networkd-wait-online.service 2>/dev/null || true
chroot <mountpoint> systemctl mask NetworkManager-wait-online.service 2>/dev/null || true

# 2. systemd-networkd 심볼릭 링크 직접 비활성화 (systemd 미실행 chroot 환경 호환)
rm -f <mountpoint>/etc/systemd/system/network-online.target.wants/systemd-networkd-wait-online.service 2>/dev/null || true
ln -sf /dev/null <mountpoint>/etc/systemd/system/systemd-networkd-wait-online.service 2>/dev/null || true
```

### 3.3 단위 테스트 계획
- **대상 파일**: [`tests/unit/test_linux_installer.cpp`](file:///home/yskyh06/linux-workspace/litewin/tests/unit/test_linux_installer.cpp)
- `build_chroot_commands` 실행 결과에 `systemd-networkd-wait-online` 마스크 명령어가 정상 포함되어 있는지 검증하는 단언문 추가.

---

## 4. 작업 단계 (Step-by-Step)
1. **[사용자 승인]**: 본 구현 계획 검토 및 작업 진행 승인
2. **[코드 수정]**: [`src/platform/linux_installer.cpp`](file:///home/yskyh06/linux-workspace/litewin/src/platform/linux_installer.cpp) 내 `build_chroot_commands`에 마스크 처리 추가
3. **[테스트 검증]**: `ctest --test-dir build` 유닛 테스트 전체 통과 확인
4. **[ISO 재빌드]**: `./scripts/build_iso_docker.sh`를 통해 새 부팅 ISO 빌드 및 SHA256 체크섬 확인
