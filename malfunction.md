# 네트워크 탐지 및 디스크 탐색 코드 검토 결함

검토일: 2026-10-31

검토 당시 확인한 결함과 이후 수정 결과를 기록한다. 아래 6개 결함의 코드 수정과 관련 테스트를 완료했으며, 전체 사용자 케이스 테스트는 사용자 선택 후 진행한다.
P1은 우선 수정이 필요한 결함, P2는 후속 수정이 필요한 결함을 뜻한다.

## 1. [P1] 데이터가 있는 디스크를 설치 가능으로 판정

- 위치: [src/core/disk_target.hpp:80](src/core/disk_target.hpp#L80)
- 원인: 마운트 지점과 하위 파티션이 없으면 빈 디스크처럼 취급한다. 현재 디스크 조회에는 파일시스템·RAID 시그니처 정보가 포함되지 않는다.
- 발생 조건: 디스크 전체에 직접 만든 파일시스템처럼, 데이터가 있지만 하위 파티션과 마운트 지점이 없는 장치.
- 영향: 기존 데이터가 있는 장치가 설치 대상으로 표시될 수 있고, 이후 초기화에 연결되면 데이터가 손실될 수 있다.
- 개선 방향: 파일시스템·RAID 등의 시그니처를 조회하고, 기존 데이터가 있거나 확인이 불완전한 장치는 거절한다. 파티션이 없다는 사실만으로 빈 디스크임을 확정하지 않는다.
- 확인 방법: 코드 검토. 실제 장치에 대한 변경 작업은 수행하지 않았다.

## 2. [P1] 마운트 상태를 몰라도 설치 가능으로 판정

- 위치: [src/core/disk_target.hpp:35](src/core/disk_target.hpp#L35)
- 원인: `mountpoints == nullopt`를 마운트되지 않은 상태처럼 처리한다. 파서 계약에서 이 값은 정보 누락 또는 알 수 없음을 뜻한다.
- 발생 조건: 이름·경로·종류·용량 조건은 충족하지만 마운트 정보가 없고 하위 장치가 없는 디스크.
- 영향: 조회가 불완전한 장치를 설치 가능으로 판정한다.
- 개선 방향: 미사용 확인과 정보 부재를 구분하고, 확인할 수 없는 상태는 거절 또는 확인 불가로 반환한다.
- 재현 결과: 합성 `DiskInfo`에 `mountpoints == nullopt`를 지정했을 때 `can_install() == true`를 반환했다.

## 3. [P1] 계산한 파티션이 디스크 끝을 초과

- 위치: [src/core/partition_layout.hpp:84](src/core/partition_layout.hpp#L84)
- 원인: 전체 디스크 용량을 파티션에 배분하면서 시작 여백과 GPT 끝부분의 예약 공간을 빼지 않는다. 정렬에 따른 추가 공간도 최종 범위 검증에 반영하지 않는다.
- 영향: 적용할 수 없는 파티션 배치를 계산 성공으로 반환한다.
- 개선 방향: 실제 사용 가능한 섹터 범위에서 고정 파티션과 정렬 비용을 먼저 계산한 뒤 Windows 파티션 용량을 정한다. 마지막 파티션이 GPT 사용 가능 범위 안에 있는지 검증한다.
- 재현 결과: 기본 옵션과 32 GiB 입력에서 디스크는 67,108,864섹터인데 마지막 파티션의 끝 다음 섹터는 67,110,912였다. 물리 용량을 2,048섹터(1 MiB) 초과해도 성공을 반환했다. 이 초과분과 별도로 GPT 끝부분의 예약 공간도 필요하다.

## 4. [P1] 외부 명령의 stdin 종료로 Litewin 전체 종료 가능

- 위치: [src/platform/process_runner.cpp:108](src/platform/process_runner.cpp#L108)
- 원인: 자식 프로세스가 stdin을 닫은 뒤 부모가 파이프에 쓰면 발생하는 `SIGPIPE`를 처리하지 않는다.
- 발생 조건: 입력을 받지 않고 종료하거나 입력을 읽기 전에 실패하는 명령에 stdin 데이터를 전달하는 경우.
- 영향: 오류 결과를 반환하는 대신 호출 프로세스 전체가 종료될 수 있다.
- 개선 방향: 파이프 쓰기의 `SIGPIPE` 및 `EPIPE`를 처리하고, 자식 프로세스를 회수한 뒤 오류를 반환한다.
- 재현 결과: `ProcessRunner::run("/bin/true", {}, 큰 입력)` 호출에서 오류 반환 대신 신호 13(`SIGPIPE`)으로 종료됐다.

## 5. [P2] 외부 명령과 입출력 교착 발생

- 위치: [src/platform/process_runner.cpp:105](src/platform/process_runner.cpp#L105)
- 원인: 부모가 stdin 데이터를 전부 쓴 뒤에야 stdout·stderr를 읽는다.
- 발생 조건: 자식이 입력을 읽으면서 큰 출력을 생성해 출력 파이프가 가득 차고, 부모도 입력 파이프에 쓰느라 대기하는 경우.
- 영향: 부모와 자식이 서로를 기다리며 작업이 멈춘다.
- 개선 방향: 비차단 I/O와 `poll` 등의 방식으로 stdin 쓰기와 stdout·stderr 읽기를 함께 처리한다.
- 재현 결과: `/bin/cat`에 16 MiB 입력을 전달했을 때 3초 제한 안에 완료되지 않았다. 파이프 용량에 따라 재현에 필요한 입력 크기는 달라질 수 있다.

## 6. [P2] 네트워크 프로브 제한 시간이 DNS 조회에 미적용

- 위치: [src/platform/network_probe.cpp:90](src/platform/network_probe.cpp#L90)
- 원인: 숫자 주소 조회에 실패하면 동기 `getaddrinfo()`로 이름을 조회하지만, 이 호출에는 전달받은 `timeout`이 적용되지 않는다.
- 발생 조건: 기본 숫자 IP 대신 호스트명을 전달하고 DNS 응답이 지연되는 경우.
- 영향: 설정한 프로브 제한 시간을 넘어 대기할 수 있다.
- 개선 방향: 숫자 IP 전용 API로 제한하거나, 이름 조회를 포함한 전체 작업에 시간 제한을 적용한다.
- 확인 방법: 코드 검토. 기본 숫자 IP 대상에서는 이름 조회 경로를 피한다.

## 검증 범위와 결과

- 별도 임시 빌드 디렉터리에서 Debug 전체 빌드 성공.
- `ctest --test-dir /tmp/litewin-review-build --output-on-failure -LE integration`: 55개 테스트 모두 통과.
- 파티션 범위 초과, 마운트 정보 누락 시 설치 허용, `SIGPIPE` 종료, 큰 입력의 입출력 교착은 임시 재현 프로그램으로 확인했다.
- 실제 네트워크·디스크 통합 검증과 실제 장치 변경 작업은 수행하지 않았다.
- 기존 테스트 통과만으로 위 경계 조건이 검증된 것은 아니다. 각 결함 수정 시 해당 조건을 확인하는 회귀 테스트가 필요하다.


## 수정 결과 (2026-10-31)

1. **디스크 시그니처 확인**: `DiskInfo::has_signatures`에 확인 결과를 보관한다. `inspect_disks()`는 각 디스크에 `wipefs --json --output TYPE --no-act -- <장치 경로>`를 실행해 읽기 전용으로 시그니처를 확인한다. 시그니처가 발견되면 설치 대상으로 거절한다. `wipefs` 부재·실행 실패·잘못된 출력은 확인 불가로 유지하고 거절한다. JSON 파서만 호출해서는 시그니처 검증을 완료한 것으로 취급하지 않는다.
2. **마운트 정보 누락 거절**: 마운트 정보 또는 시그니처 확인 결과가 없으면 `incomplete_metadata`로 거절한다.
3. **GPT 사용 가능 범위 내 배치**: 양 끝의 GPT 예약 공간과 시작·파티션 정렬 비용을 반영한 뒤 남은 공간으로 Windows 파티션 크기를 계산한다. 512바이트·4096바이트 섹터를 지원하며, 잘못된 옵션과 공간 부족을 거절한다. 생성 스크립트에 섹터 크기를 명시한다.
4. **SIGPIPE 종료 방지**: 자식 stdin 연결에 로컬 스트림 소켓과 `MSG_NOSIGNAL`을 사용한다. 전역 신호 처리기를 바꾸지 않으며, 자식이 입력을 닫으면 오류 결과를 반환하고 자식을 회수한다.
5. **입출력 교착 방지**: 비차단 stdin 쓰기와 stdout·stderr 읽기를 하나의 `poll` 루프에서 함께 처리한다. 자식 실행 전 인자를 준비하고, 부분 채널 생성 실패의 자원 정리와 `waitpid`의 인터럽트 재시도도 처리한다.
6. **DNS 대기 제거**: 프로브는 숫자 IPv4/IPv6 주소만 지원한다. 호스트명은 DNS 조회 없이 즉시 오류로 반환한다. 공개 헤더에 이 계약을 명시했다.

### 수정 검증

추가한 테스트 파일: [tests/malfunction_regression_tests.cpp](tests/malfunction_regression_tests.cpp)

| 추가 회귀 테스트 | 확인 내용 |
| --- | --- |
| `regression.signatures` | 디스크 전체의 파일시스템·RAID·GPT·LVM 시그니처 거절, 확인된 빈 디스크 허용, 읽기 전용 명령 인자 |
| `regression.unknown_signatures` | 잘못된 JSON·필드 누락·도구 실패 시 설치 거절 |
| `regression.unknown_mounts` | 마운트 정보와 시그니처 확인 결과 누락 시 거절, JSON 입력만으로 검증 상태 위조 불가 |
| `regression.layout_bounds` | 32/60 GiB급 디스크, 512/4096바이트 섹터, 정렬이 필요한 크기에서 GPT 범위·정렬·비중첩 확인 |
| `regression.invalid_layout` | 0인 섹터 크기·정렬, 과도한 정렬·파티션 크기, 예약 공간 부족 거절 |
| `regression.closed_stdin` | 입력을 읽지 않는 자식과 실행 파일 부재에서 호출자 생존·오류 반환, SIGPIPE 처리기 유지 |
| `regression.large_stdio` | 16 MiB 입력과 각각 16 MiB인 stdout·stderr의 동시 전송, 데이터 무손실·교착 없음 |
| `regression.numeric_only` | 호스트명·잘못된 IP 즉시 거절, 숫자 IPv4/IPv6 허용 |

- Debug에서 수정 관련 테스트 타깃 빌드 성공.
- 추가 회귀 테스트 8개와 관련 기존 테스트 12개, **총 20개 통과**.
- 기존 테스트 범위: `target.*` 6개, `layout.*` 5개, `probe.invalid_address` 1개.
- 샌드박스가 로컬 소켓의 `send` 호출을 차단해, 최종 20개 검증은 허용된 샌드박스 외부 실행으로 완료했다.
- 외부 인터넷 서버 요청과 실제 블록 장치 접근·변경 없이 합성 데이터와 로컬 자식 프로세스로 검증했다. 숫자 IP 허용 확인은 루프백 주소의 포트 0에만 시도했다.
- **전체 테스트와 실제 장치 통합 테스트는 실행하지 않았다.** 사용자 케이스 선택 후 별도로 진행한다.

재실행 명령 (전체 테스트가 아닌 수정 관련 범위):

```sh
cmake -S . -B /tmp/litewin-review-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/litewin-review-build --target malfunction_regression_tests disk_target_tests partition_layout_tests network_probe_tests -j2
ctest --test-dir /tmp/litewin-review-build --output-on-failure -R '^(regression\.|target\.|layout\.|probe\.invalid_address$)'
```

### 이후 선택할 사용자 케이스 (아직 실행하지 않음)

- 유선 또는 Wi-Fi가 이미 연결된 상태에서 조회·도달성 검사·진행 판단.
- Wi-Fi 미연결·유선 미연결·어댑터 없음·IP는 있지만 외부 접근 실패인 상태에서 행동 판단 및 필요 시 설정 후 재조회.
- 디스크 목록 조회와 빈 디스크·기존 파티션·마운트 중·시그니처 존재·확인 불가인 장치의 설치 대상 판정.
- 32/60 GiB급 디스크 및 512/4096바이트 섹터의 파티션 배치 계산·스크립트 생성·dry-run.

실제 포맷·파티션 적용 테스트는 위 읽기 전용 조회 및 dry-run과 별도로 범위를 정해야 한다.

## 디스크 사용자 케이스 검증 (2026-10-31)

사용자 선택에 따라 디스크 탐색·설치 대상 판정·파티션 배치·dry-run만 검토했다. 네트워크 테스트는 실행하지 않았다.

### 테스트 보완

- `tests/disk_inspector_tests.cpp`: 기존 실제 조회 테스트가 조회 오류를 출력한 뒤 성공으로 종료하던 동작을 수정했다. 조회 실패는 이제 테스트 실패로 처리한다.
- 실제 조회 결과에는 장치 수·디스크 수·시그니처 확인 불가 수·설치 가능 수만 출력한다.
- 합성 명령 실행기를 사용해 도구 부재, lsblk 실패, 잘못된 JSON/스키마, 빈 목록, 여러 디스크 중 한 장치의 시그니처 조회 권한 실패를 검증하는 테스트 5개를 추가했다.
- 한 장치의 시그니처 조회 실패가 다른 장치의 결과를 오염시키지 않는지, 디스크 외 장치는 보존하되 설치 대상으로 허용하지 않는지 확인했다.

### 결과

| 검증 범위 | 결과 |
| --- | --- |
| 디스크 JSON 파서 `inventory.*` | 20개 통과 |
| 설치 대상 판정 `target.*` | 6개 통과 |
| 디스크 관련 회귀 테스트 | 5개 통과 |
| 파티션 배치·스크립트 생성 | 3개 통과 |
| 파티션 경로·적용/포맷/마운트/해제 dry-run | 4개 통과 |
| 디스크 조회 합성 테스트 | 5개 통과 |
| 실제 디스크 읽기 전용 조회 | 1개 통과, 아래 제약 참조 |
| **CTest 합계** | **44개 통과, 실패 없음** |

추가로 프로젝트의 `generate_sfdisk_script()`가 생성한 스크립트를 사용해 임시 희소 파일에서 실제 `sfdisk --no-act` 검증을 수행했다.

- 조합: 32 GiB/512바이트, 32 GiB/4096바이트, 60 GiB/512바이트, 60 GiB/4096바이트.
- 4개 조합 모두 sfdisk 종료 코드 0.
- 각 임시 파일의 앞·뒤 64 KiB 해시가 실행 전후 동일함을 확인했다.
- `/tmp`의 임시 파일만 사용했으며 실제 블록 장치의 파티션 적용·포맷·마운트·해제를 수행하지 않았다.

### 실제 장치 검증의 제약

실제 조회 결과: `devices=5`, `disks=5`, `signatures_unknown=5`, `eligible=0`.

현재 실행 환경에서는 모든 실제 디스크의 시그니처 확인 결과가 불명이었다. 이 상태에서 설치 가능으로 오판하지 않고 모두 거절하는 동작은 확인했다. **실제 장치에서 시그니처를 성공적으로 읽고 빈 디스크와 기존 파일시스템을 구분하는 동작은 아직 검증하지 못했다.** 해당 부분은 장치 접근이 가능한 환경에서 별도 확인이 필요하다. 합성 테스트의 시그니처 판별 성공과 구분한다.

재실행 명령:

```sh
cmake -S . -B /tmp/litewin-review-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/litewin-review-build --target disk_inventory_tests disk_target_tests disk_inspector_tests partition_layout_tests disk_partitioner_tests malfunction_regression_tests list_disks -j2
ctest --test-dir /tmp/litewin-review-build --output-on-failure -R '^(inventory\.|target\.|inspector\.|partitioner\.|regression\.(signatures|unknown_signatures|unknown_mounts|layout_bounds|invalid_layout)$|layout\.(calculate_32gb|too_small_disk|generate_sfdisk_script)$)'
```
