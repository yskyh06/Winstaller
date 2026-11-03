# Winstaller TUI 단계 간소화 및 직관적 드롭다운 UI 개편 세부 구현 방안

## 1. 개편 목적 및 방향
- **직관적인 모달 선택**: 좌우키/스페이스 값 변경 방식의 인지 부하를 없애고, `Enter`를 누르면 세로 목록 팝업(모달)이 열려 위/아래 방향키와 `Enter`로 항목을 선택하는 웹/앱 표준 셀렉트 박스 패턴 적용.
- **용어 명확화**: 엔지니어링 용어인 `ESD Source`를 직관적인 **`Install Source`** (`[X] Local Cache (Skip Download)`)로 변경.
- **불필요한 기술 문구 제거**: `(100% ram resident)` 등 일반 사용자에게 불필요한 TMI 문구를 제거하고 화면 여백과 미니멀리즘 확보.
- **2-Screen 워크플로우 완성**: 디스크 선택(면책 동의 결합) ➔ 올인원 추천 설정 대시보드 2단계 구조 확립.

---

## 2. 화면 인터랙션 상세 명세

### 2.1. 메인 설정 대시보드 (`prompt_edition_selection`)
- 메인 화면에서는 **`위/아래(Up/Down)`**로만 행 이동.
  - 행 0: `[1] Windows Edition : [ Windows 11 Home ] (Enter to change)` ➔ Enter 시 에디션 모달
  - 행 1: `[2] Target Language : [ Korean (ko-KR)   ] (Enter to change)` ➔ Enter 시 언어 모달
  - 행 2: `[3] Install Source   : [X] Local Cache (Skip Download)` ➔ Space 시 토글 (캐시 존재 시)
  - 행 3: `[ Start Installation (Enter) ]` ➔ Enter 시 배포 시작
  - 행 4: `[ Back (B) ]` / `[ Exit (Esc) ]`

### 2.2. 에디션 선택 모달 (`show_edition_modal`)
- 화면 중앙에 콤팩트한 팝업 카드로 표시.
- 위/아래로 `Windows 11 Home`과 `Windows 11 Pro` 이동.
- `Enter`로 선택 후 메인 화면 갱신 복귀, `Esc`로 취소.

### 2.3. 언어 선택 모달 (`show_language_modal`)
- 화면 중앙에 지원 언어 7종(한국어, 영어, 일본어, 중국어, 독일어, 프랑스어, 스페인어) 세로 목록 표시.
- 위/아래로 탐색 후 `Enter`로 선택 확정, `Esc`로 취소.

---

## 3. 작업 Goal
- **Goal 1**: `src/ui/winstaller_tui.hpp` 및 `src/ui/winstaller_tui.cpp`에 에디션/언어 전용 드롭다운 모달 함수 추가.
- **Goal 2**: 메인 설정 화면에서 상하 이동과 Enter 드롭다운 호출 로직 구현 및 `Install Source` 용어 변경 적용.
- **Goal 3**: 단위 테스트(`tests/tui_tests.cpp`) 검증 및 CMake 빌드 & 87개 테스트 통과.
- **Goal 4**: Docker 기반 최종 ISO 빌드 및 무결성 검증.
