# 다국어(글로벌) Windows ESD 카탈로그 확장 계획서

본 문서는 Winstaller에서 한국어(`ko-KR`)와 영어(`en-US`) 외에 주요 글로벌 언어를 지원하기 위해 Microsoft 공식 정적 ESD 카탈로그를 확장하고, TUI 언어 선택 모달과 연동하기 위한 상세 구현 계획서입니다.

---

## 1. 대상 언어 및 지원 스펙

| 번호 | 언어 명칭 | 언어 코드 (Locale) | 지원 버전 | 기본 에디션 인덱스 (Home / Pro) |
| :---: | :--- | :---: | :---: | :---: |
| 1 | 한국어 (Korean) | `ko-kr` | Win11 26H2 / 24H2, Win10 22H2 | Home (Index 4) / Pro (Index 6) |
| 2 | 영어 (English - US) | `en-us` | Win11 26H2 / 24H2 | Home (Index 4) / Pro (Index 6) |
| 3 | 일본어 (Japanese) | `ja-jp` | Win11 26H2 / 24H2 | Home (Index 4) / Pro (Index 6) |
| 4 | 중국어 간체 (Chinese Simplified) | `zh-cn` | Win11 26H2 / 24H2 | Home (Index 4) / Pro (Index 6) |
| 5 | 독일어 (German) | `de-de` | Win11 26H2 / 24H2 | Home (Index 4) / Pro (Index 6) |
| 6 | 프랑스어 (French) | `fr-fr` | Win11 26H2 / 24H2 | Home (Index 4) / Pro (Index 6) |
| 7 | 스페인어 (Spanish) | `es-es` | Win11 26H2 / 24H2 | Home (Index 4) / Pro (Index 6) |

---

## 2. Microsoft 공식 Delivery Optimization CDN 연동 규격

1. **프로토콜 표준**:
   - 도메인: `http://dl.delivery.mp.microsoft.com/filestreamingservice/files/{GUID}/{Filename}.esd`
   - 포트: HTTP 80 (TLS/443 연결 시 핸드셰이크 오류 방지)
2. **보안 및 무결성 보장**:
   - `EVP_sha256` 기반 인플라이트 스트리밍 해시 검증을 통해 다운로드 중 위변조 및 패킷 손상 방지
3. **ESD 파일 명명 규칙**:
   - Windows 11 26H2: `26100.1.240331-1435.ge_release_CLIENTCONSUMER_RET_x64FRE_{locale}.esd`
   - Windows 11 24H2: `26100.4349.250607-1500.ge_release_svc_refresh_CLIENTCONSUMER_RET_x64FRE_{locale}.esd`

---

## 3. 코드 수정 상세 계획

### 3.1. `src/core/esd_catalog.cpp`
- `g_known_targets` 배열에 신규 5개 언어(`ja-jp`, `zh-cn`, `de-de`, `fr-fr`, `es-es`)의 공식 ESD 타깃 메타데이터 등록
- 대소문자 무관 검색(`iequals`) 함수 유지 및 언어 코드 정규화

### 3.2. `src/ui/winstaller_tui.cpp`
- `prompt_edition_selection` 내 `languages` 벡터 확장:
  ```cpp
  const std::vector<std::pair<std::string, std::string>> languages = {
      {"ko-KR", "Korean (한국어)"},
      {"en-US", "English (United States)"},
      {"ja-JP", "Japanese (日本語)"},
      {"zh-CN", "Chinese Simplified (简体中文)"},
      {"de-DE", "German (Deutsch)"},
      {"fr-FR", "French (Français)"},
      {"es-ES", "Spanish (Español)"}
  };
  ```
- 언어 선택 모달 창 높이(`m_h`)를 7개 항목에 맞춰 유동적 또는 13줄로 확장

### 3.3. `src/core/unattend_generator.cpp`
- `oobeSystem` 패스의 `Microsoft-Windows-International-Core`에 선택된 언어 코드(`InputLocale`, `SystemLocale`, `UILanguage`, `UserLocale`) 자동 매핑

---

## 4. 검증 및 테스트 계획

1. **단위 테스트 (`tests/esd_catalog_tests.cpp`)**:
   - 7개 언어 모두에 대해 `find_esd_target(Win11_26H2, lang, "x64")` 호출 시 유효한 `EsdTarget` 객체가 반환되는지 assertion 검증
2. **TUI 단위 테스트 (`tests/tui_tests.cpp`)**:
   - 모달 인덱스 확장 및 7개 언어 순환 네비게이션 무결성 검증
3. **전체 ctest 실행**:
   - 기존 87개 테스트에 다국어 검증 항목을 추가하여 100% 통과 확인
