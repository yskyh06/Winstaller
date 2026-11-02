# Litewin

RAM 기반 경량 Linux에서 Windows 설치를 준비하고 WinPE로 인계하는 도구를
기능별로 개발합니다. 목표·미검증 사항·TODO는 [plan.md](plan.md)를 참조하세요.

현재 구현은 **디스크 조회 JSON 파서**와 **Linux 네트워크 인터페이스 조회 함수**입니다.
실제 디스크 조회나 설치 가능 판단, 다운로드, 네트워크 연결 설정, 설치 기능은 아직 없습니다.

## 빌드와 테스트

C++20 컴파일러, CMake 3.20 이상과 CMake가 사용할 빌드 도구가 필요합니다.
JSON 의존성은 저장소에 포함되어 빌드 중 다운로드하지 않습니다.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target disk_inventory_tests network_inspector_tests inspect_network
ctest --test-dir build --output-on-failure
```

전체 Linux/USB 이미지를 빌드하지 않습니다. 실제 블록 장치 접근은 하지 않습니다.
네트워크 라이브러리·테스트·조회 예제는 Linux에서만 빌드합니다.
단위 테스트는 저장 데이터와 합성 인터페이스를 사용하며, `integration` 레이블의
테스트 하나만 실제 인터페이스를 읽습니다. 외부 서버에 요청하지 않습니다.
일반 Linux에서는 관리자 권한이 필요 없지만, 제한된 샌드박스는 조회를 차단할 수 있습니다.
그 환경에서는 단위 테스트를 먼저 실행하고 실제 조회 테스트는 허용된 환경에서 실행합니다.

```sh
ctest --test-dir build --output-on-failure -LE integration
ctest --test-dir build --output-on-failure -L integration
```

## 파서 사용

공개 헤더는 `core/disk_inventory.hpp`, CMake 타깃은 `litewin_inventory`입니다.

```cpp
auto result = litewin::parse_disk_inventory(json_text);
if (const auto* inventory = std::get_if<litewin::DiskInventory>(&result)) {
    // inventory contains device trees, not a list of approved install targets.
} else {
    const auto& error = std::get<litewin::ParseError>(result);
    // Report error.location and error.message to the caller.
}
```

입력 열은 `NAME,PATH,TYPE,SIZE,MODEL,SERIAL,TRAN,MOUNTPOINTS`에 해당합니다.
`SIZE`는 `--bytes`를 사용한 정수여야 합니다. 식별에 필요한 필드 누락과
타입 오류는 실패로 반환하며, 선택 정보의 누락은 알 수 없음으로 보존합니다.
알 수 없는 추가 필드는 허용합니다. 자세한 계약은 헤더와 계획 문서를 참조하세요.

테스트 데이터는 합성 데이터이며 실제 장비의 식별 정보를 포함하지 않습니다.
외부 라이브러리의 출처와 라이선스는 [third_party/README.md](third_party/README.md)에 있습니다.

## 네트워크 인터페이스 조회

공개 함수는 `platform/network_inspector.hpp`의
`litewin::inspect_network_interfaces()`, CMake 타깃은 `litewin_network`입니다.
공통 데이터 구조는 `core/network_interface.hpp`에 정의합니다.

```cpp
auto result = litewin::inspect_network_interfaces();
if (const auto* interfaces = std::get_if<litewin::NetworkInventory>(&result)) {
    // Enumerate interfaces and their addresses; this is not a connectivity test.
} else {
    const auto& error = std::get<litewin::NetworkInspectionError>(result);
    // Report error.operation and error.code.message().
}
```

직접 확인할 수 있는 작은 예제 실행 파일도 제공합니다. 장치 이름·유형·상태·IP 주소를
터미널에 출력합니다.

```sh
./build/inspect_network
```

- `getifaddrs()`로 이름·활성 상태·IPv4/IPv6 주소를 조회하고 목록을 자동 해제합니다.
- down 상태, IP 미할당, 루프백과 가상 인터페이스도 제외하지 않습니다.
- sysfs의 `wireless` 또는 `phy80211` 정보로 Wi-Fi를 구분합니다. 이름 접두사로 추측하지 않습니다.
- Ethernet 링크 유형에는 가상 인터페이스도 포함될 수 있습니다. 물리 유선 연결을 보장하지 않습니다.
- 분류 정보가 없거나 접근할 수 없으면 종류를 `unknown`으로 유지합니다.
- IPv6 scope ID를 보존하며 장치 이름·주소 순서를 정렬하고 동일 주소 중복을 제거합니다.
- 장치 없음은 빈 목록 성공, OS 조회 실패는 작업 이름과 오류 코드를 반환합니다.
- IP나 활성 플래그가 인터넷 연결을 뜻하지 않습니다. DNS·HTTPS 검사, 드라이버 진단,
  rfkill 조회, Wi-Fi 설정 화면 실행은 후속 기능입니다.
- 관측 도중 장치 상태가 바뀔 수 있으므로 연결 설정 후 다시 호출해야 합니다.

테스트는 Wi-Fi 표식, 주소 없는 장치, 여러 주소와 scope, 조회 실패, 자원 해제를
합성 데이터로 검증합니다. 실제 Wi-Fi 하드웨어에서의 확인은 아직 하지 않았습니다.
