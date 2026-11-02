# WSL2 Antigravity CLI 보조 모델(Web Consultant) 시스템 가이드

## 1. 개요 및 구조

Windows 11 WSL2(Ubuntu 26.04) 환경에서 Antigravity CLI(AGY)를 메인 오케스트레이터로 사용하고, Gemini Web 및 Google AI Studio를 추가 비용 없이 보조 추론 모델로 활용하기 위한 통합 가이드입니다.

- **Antigravity CLI (Main Orchestrator):** 코드베이스 편집, 로컬 터미널 실행, 단위 테스트, Git 관리, 최종 검증
- **Gemini Web (`https://gemini.google.com`):** 복잡한 디버깅, 코드 리뷰, 제2의 시각 제공
- **Google AI Studio (`https://aistudio.google.com`):** 시스템 아키텍처 검토, 알고리즘 설계 분석, 독립 검증
- **web-consultant Skill (`~/.gemini/antigravity-cli/skills/web-consultant`):** 요청 프롬프트 규격화, 민감 정보 제거, 5단계 표준 응답 회수 및 검증
- **비용 원칙:** GCP / Vertex AI / 유료 API 키 사용 없음 (100% 무료 웹 환경 연동)

---

## 2. 환경 조사 결과 요약

| 점검 항목 | 결과 및 세부사항 | 상태 |
| :--- | :--- | :--- |
| **OS & WSL2** | Ubuntu 26.04 LTS (x86_64), Linux kernel 6.18.33.2-microsoft-standard-WSL2 | 정상 |
| **WSLg / Display** | `WAYLAND_DISPLAY=wayland-0`, `DISPLAY=:0` (WSLg GUI 지원) | 정상 |
| **Chrome 설치** | Google Chrome 156.0.8078.79 설치됨 (`/opt/google/chrome/chrome`) | 정상 |
| **AGY CLI** | Antigravity CLI 정상 구동 및 세션 유지 | 정상 |
| **Browser Subagent** | `/browser` 명령 지원 (DevTools MCP 기반 연동) | 준비 완료 |
| **전역 Skill** | `~/.gemini/antigravity-cli/skills/web-consultant` 등록 완료 | 정상 |

---

## 3. Chrome 디버깅 포트 실행 및 브라우저 연결 방법

최신 Chrome(136+)의 보안 정책으로 인해 기본 사용자 프로필에서는 `--remote-debugging-port`가 자동으로 무시됩니다. 따라서 반드시 전용 디버깅 디렉터리를 지정해야 합니다.

### Chrome 실행 명령어 (WSL2 터미널)
```bash
# 기존 실행 중인 백그라운드 크롬 프로세스 정리
pkill -f chrome

# 전용 디버깅 프로필로 Chrome 실행 (GUI 창이 뜸)
google-chrome \
  --remote-debugging-port=9222 \
  --user-data-dir=~/.config/google-chrome-debug \
  --remote-allow-origins=* \
  --no-first-run &
```

> **포트 정상 청취 확인:**
> ```bash
> curl -s http://127.0.0.1:9222/json/version
> ```

### 웹 서비스 로그인 안내 (사용자 직접 수행)
1. 실행된 Chrome 창에서 `https://gemini.google.com` 및 `https://aistudio.google.com`에 직접 로그인합니다.
2. 각 서비스의 모델 선택 메뉴에서 **Gemini 4 Argon** (또는 최신 지원 모델)이 활성화되어 있는지 확인합니다.
3. *보안 준수:* 인증 토큰이나 쿠키를 에이전트에 복사하거나 저장할 필요가 전혀 없습니다.

---

## 4. 공통 보조 모델 인터페이스 및 5단계 응답 규격

모든 보조 모델 질의는 `web-consultant`의 표준 봉투(Envelope)를 통해 전달되며, 응답은 5개 섹션으로 정형화됩니다:

1. **Summary:** 분석 내용 핵심 요약 (2~3문장)
2. **Findings:** 원인 분석, 코드 문제점, 아키텍처 제언
3. **Proposed Changes:** 제안하는 코드 변경점 및 Diff
4. **Risks:** 발생 가능한 회귀 버그, 보안 위험, 엣지 케이스
5. **Recommended Tests:** 변경 사항을 검증하기 위한 구체적인 테스트 명령어 및 케이스

### 민감 정보 마스킹 (Sanitization)
`web-consultant/scripts/format_consultation.py`를 통해 API 키, 비밀번호, 토큰 등 민감 정보는 자동으로 `[REDACTED]` 처리된 후 프롬프트가 구성됩니다.

---

## 5. 세분화된 권한 정책 (Permissions)

Auto 모드에서 반복적인 불필요 승인을 줄이고 시스템 파괴 명령을 방어하기 위한 권한 규칙이 [permissions_config.json](file:///home/yskyh06/linux-workspace/litewin/permissions_config.json)에 정의되어 있습니다.

- **자동 승인 (Allow):**
  - 워크스페이스 내부 읽기/쓰기 (`read_file(/home/yskyh06/linux-workspace/litewin)`, `write_file(...)`)
  - 읽기 전용 Git 명령 (`git status`, `git diff`, `git log`, `git show`, `git branch`)
  - 단위 테스트 (`npm test`, `pytest`, `cargo test`, `go test`)
  - 안전한 검색/조회 유틸리티 (`ls`, `cat`, `grep`, `find`, `wc`)
  - 신뢰할 수 있는 개발 문서 (`github.com`, `developer.mozilla.org`, `docs.python.org`)
- **승인 필수 (Ask):**
  - 관리자 및 시스템 변경 (`sudo`, `rm -rf`)
  - 파괴적 Git 작업 (`git push --force`, `git reset --hard`)
  - 외부 네트워크 전송 (`curl`, `wget`, `ssh`, `gpg`)
- **차단 (Deny):**
  - 민감한 인증 키 디렉터리 접근 (`~/.ssh`, `~/.aws`, `~/.gnupg`, `/etc/shadow`)

---

## 6. 백업 및 복구 가이드

- **백업 위치:** `/tmp/gemini_config_backup/`
  - 원본 프로젝트 설정: `8537585d-1ad4-4827-a4c5-b55a2c0dc5a0.json`
  - 원본 Skill: `web-consultant/`
- **복구 방법:**
  설정을 초기 상태로 되돌리려면 `/tmp/gemini_config_backup/`의 백업 파일을 원래 위치로 복사하거나 [permissions_config.json](file:///home/yskyh06/linux-workspace/litewin/permissions_config.json)을 참고하여 `/permissions` 패널에서 재설정할 수 있습니다.
