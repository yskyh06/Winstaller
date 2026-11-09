# LiteWin (Winstaller) Version 1.1 — 2차 교차 검증(Audit) 및 기능 테스트 지침서

> **목적**: 본 문서는 2차 검증 에이전트(Claude Opus 등)가 `feature/v1.1-multios` 브랜치에 구현된 **Version 1.1 (리눅스 배포판 다이렉트 설치 엔진 & 상단 탭 멀티 OS TUI)** 기능을 독립적이고 엄격하게 감사(Audit) 및 기능 테스트하기 위한 체크리스트입니다.
> **기준 설계서**: `docs/v1_1_roadmap.md`

---

## 1. 검증 대상 파일 목록 (Scope of Audit)

| 영역 | 대상 파일 경로 | 역할 및 변경 구분 |
| :--- | :--- | :--- |
| **Linux 카탈로그** | `src/core/linux_catalog.hpp`<br>`src/core/linux_catalog.cpp`<br>`tests/linux_catalog_tests.cpp` | **[신규]** 5대 배포판(2026.11 기준 정규+연장 지원) 메타데이터, 추출 포맷(`SingleSquashFS`/`LayeredSquashFS`/`Erofs`), `(Expired)` 표기 규칙 |
| **Linux 설치 엔진** | `src/platform/linux_installer.hpp`<br>`src/platform/linux_installer.cpp`<br>`tests/linux_installer_tests.cpp` | **[신규]** GPT(`FAT32` ESP + `ext4` Root) 파티셔닝, 3대 포맷 추출기, 3대 계열별(`Debian`/`RedHat`/`Arch`) `chroot` 초기화 및 UEFI GRUB 설치 |
| **멀티 OS TUI** | `src/ui/winstaller_tui.hpp`<br>`src/ui/winstaller_tui.cpp`<br>`tests/tui_tests.cpp` | **[수정]** 상단 5개 가로 탭(`[Windows]`, `[Ubuntu]`, `[Debian]`, `[RHEL / Fedora]`, `[Arch]`) 전환 및 단일 리스트 선택 UI |
| **메인 파이프라인 연동** | `src/main.cpp`<br>`CMakeLists.txt` | **[수정]** TUI에서 선택한 OS(`choice.is_linux`)에 따른 `InstallPipeline` vs `LinuxInstaller` 분기 실행 및 빌드 타겟 등록 |

---

## 2. 핵심 검증 항목 (Step-by-Step Audit Checklist)

### 검증 1: 모듈 간 ID 및 데이터 정합성 (Cross-Module Integration)
- [ ] **TUI $\leftrightarrow$ Catalog ID 일치 여부**: `src/ui/winstaller_tui.cpp`의 `WinstallerTui::get_tab_os_items()`가 반환하는 `linux_distro_id` 값들이 `src/core/linux_catalog.cpp`의 `find_linux_target_by_id()`에서 **단 하나도 누락 없이 100% 조회(`std::optional` non-empty)되는지** 대조 검증할 것.
- [ ] **`main.cpp` 폴백(Fallback) 안전성**: 만약 `main.cpp`에서 `find_linux_target_by_id(selected_choice.linux_distro_id)`가 `std::nullopt`를 반환할 경우, 빈 URL/레이어로 설치가 강행되지 않고 명확한 에러로 즉시 중단되는지 확인할 것.

### 검증 2: 카탈로그 명세 준수 여부 (`linux_catalog.*`)
- [ ] **배지 제거 및 `(Expired)` 표기 규칙**: 모든 배포판의 `display_name`에 `[최신]` 또는 `[레거시]` 같은 배지가 없어야 하며, 정규 지원이 종료된 연장 지원 버전(Ubuntu 20.04/18.04, Debian 11, Rocky/Alma 8)만 정확히 `" (Expired)"`로 끝나는지 확인할 것.
- [ ] **2026년 최신 파일시스템 포맷 반영 여부**:
  - Ubuntu 24.04 / 26.04 LTS가 단일 SquashFS가 아닌 `LayeredSquashFS`(최소 2개 이상의 계층 경로)로 정의되어 있는지 확인.
  - Fedora 44 / 45 및 Rocky / AlmaLinux 10이 SquashFS가 아닌 `Erofs`(`LiveOS/rootfs.erofs`)로 정의되어 있는지 확인.

### 검증 3: 리눅스 설치 엔진 보안 및 실제 동작 완결성 (`linux_installer.*`)
- [ ] **`dry_run == false` (실제 하드웨어 모드) 동작 검증**: `LinuxInstaller::execute()`가 `dry_run == true`일 때만 문자열을 기록하는 스텁(Stub)에 그치지 않고, `dry_run == false`일 때 실제로 `ProcessRunner` / `DiskPartitioner` / `HttpDownloader`를 호출해 파티셔닝 $\rightarrow$ 포맷(`mkfs.fat`, `mkfs.ext4`) $\rightarrow$ 마운트 $\rightarrow$ 이미지 추출 $\rightarrow$ `chroot` 실행 $\rightarrow$ 언마운트를 수행하는지 코드 레벨에서 엄격히 감사할 것.
- [ ] **명령어 인젝션(Command Injection) 및 이스케이프 안전성**: 사용자가 입력한 `username`, `password`, `hostname`, `mount_root`가 `build_chroot_commands()`나 셸 명령어 문자열에 조합될 때 특수문자/공백으로 인한 인젝션이나 구문 오류 위험이 없는지 확인할 것.
- [ ] **실패 시 클린업(Rollback / Unmount)**: 중간 단계(예: 추출 실패 또는 `chroot` 실패)에서 오류 발생 시 마운트된 `/mnt/litewin_linux/boot/efi`, `/proc`, `/sys`, `/dev` 가상 파일시스템과 루트 파티션이 안전하게 언마운트되는지 확인할 것.
- [ ] **파티션 디바이스 명명 규칙**: `/dev/sda` $\rightarrow$ `/dev/sda1`, `/dev/nvme0n1` $\rightarrow$ `/dev/nvme0n1p1`, `/dev/mmcblk0` $\rightarrow$ `/dev/mmcblk0p1` 분기가 정확한지 확인할 것.

### 검증 4: TUI 상단 탭 UX 및 하위 호환성 (`winstaller_tui.*`)
- [ ] **상단 가로 탭 네비게이션**: `show_edition_modal()`에서 `Key::Left` / `Key::Right` 입력 시 5개 탭(`[ Windows ] | [ Ubuntu ] | [ Debian ] | [ RHEL / Fedora ] | [ Arch ]`)이 정상 전환되고 커서 인덱스(`selected_idx`)가 새 탭의 리스트 범위를 벗어나지 않도록(Out-of-bounds 방지) 초기화되는지 확인할 것.
- [ ] **Windows 설치 흐름 회귀 영향도**: Linux 선택 후 다시 `Back`으로 돌아가 `Windows` 탭의 에디션을 재선택했을 때 `choice.is_linux = false` 및 `choice.linux_distro_id = ""`로 깨끗하게 리셋되는지 확인할 것.
- [ ] **TUI 사후 설정(`main.cpp` 완료 후 루프)**: 설치 완료 후 `main.cpp` 하단에서 Windows 전용 후처리(`p4` NTFS 마운트 및 `Windows/Panther/unattend.xml` 재기록)가 `selected_choice.is_linux == true`일 때 잘못 실행되지 않는지 확인할 것.

---

## 3. 빌드 및 자동화 테스트 실행 방법

검증 에이전트는 아래 명령어를 직접 실행하여 컴파일 경고 유무와 테스트 통과 여부를 확인해야 합니다.

```bash
# 1. CMake 구성 및 전체 빌드 (-Wall -Wextra -Wpedantic 경고 확인)
cmake -B build
cmake --build build -j4

# 2. 전체 CTest 스위트 실행 (총 89개 테스트)
ctest --test-dir build --output-on-failure

# 3. v1.1 핵심 테스트 바이너리 개별 상세 실행
./build/linux_catalog_tests
./build/linux_installer_tests
./build/tui_tests
```

---

## 4. 2차 검증 결과 보고 양식 (Opus 출력 가이드)

검증 완료 후 아래 형식으로 결함 및 개선 사항을 분류하여 보고해 주세요:
1. **[Critical / Blocker]**: 실제 설치 실패, 데이터 손실, 모듈 간 ID 불일치, `dry_run=false` 미구현, 메모리/인덱스 침범 버그
2. **[High]**: 예외 처리 누락(마운트 해제 누락, 폴백 미처리), Windows/Linux 분기 간 상태 오염(State Leak)
3. **[Medium / Low]**: 코드 중복, 입력값 검증 보완, UX 엣지 케이스
4. **종합 판정**: `PASS` / `CONDITIONAL PASS` / `FAIL` 및 수정 권고 패치 목록
