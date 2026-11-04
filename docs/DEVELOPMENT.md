# Winstaller 개발자 및 기술 가이드

Winstaller의 빌드, 테스트, 아키텍처 및 배포용 ISO 패키징에 대한 기술 가이드입니다.

---

## 1. 아키텍처 개요

Winstaller는 초경량 임베디드 리눅스 라이브 환경(Arch Linux 기반, `copytoram=y`)에서 실행되며 다음과 같은 단계로 파이프라인을 수행합니다:

1. **디스크 및 네트워크 인벤토리**: `sysfs` 및 `getifaddrs`를 통해 블록 장치와 네트워크 어댑터 상태 열거
2. **파티션 레이아웃 계획**: GPT 기반 [WinRE (800MB) | ESP (1GB) | MSR (16MB) | OS (C:) | 임시 캐시 (8GB)] 구성
3. **Microsoft 공식 ESD 스트리밍**: HTTP Range 이어받기 및 OpenSSL EVP 기반 인플라이트 SHA-256 스트리밍 해시 검증
4. **WIM 이미지 적용**: `wimlib-imagex` 엔진을 사용하여 C: 파티션에 Windows 시스템 파일 압축 해제 및 배포
5. **무인 설정(`unattend.xml`) 주입**: TPM 2.0/SecureBoot 우회, NRO 우회, `DisableZAU`(OOBE 업데이트 스킵), 로컬 계정 설정
6. **WinPE 및 BCD 부트스트래핑**: Windows Boot Manager 및 WinPE BCD 레코드 등록
7. **사후 파티션 자동 정리**: 첫 부팅 시 `extend_c.txt`를 통해 5번 임시 파티션 삭제 및 C 드라이브 전체 확장

---

## 2. 개발 환경 요구 사양

- C++20 호환 컴파일러 (`g++ >= 11` 또는 `clang++ >= 13`)
- CMake 3.20 이상 및 Ninja/Make
- 필수 라이브러리: `libcurl`, `OpenSSL`, `wimlib`, `Google Test (gtest)`

---

## 3. 로컬 빌드 및 테스트

### 컴파일
```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### 단위 및 통합 테스트 실행 (87개 검증 유닛)
```bash
ctest --test-dir build --output-on-failure
```

---

## 4. Docker 기반 부팅 미디어(ISO) 빌드

Host 시스템의 환경 오염 없이 격리된 Docker 컨테이너에서 공식 Archiso 이미지를 빌드합니다.

```bash
# Docker 기반 Archiso 빌드
./scripts/build_iso_docker.sh
```

- 빌드 산출물 위치: `build/iso_out/litewin-*.iso`
- 체크섬 파일: `build/iso_out/SHA256SUMS`

### ISO 무결성 검증
```bash
./scripts/verify_iso.sh build/iso_out/litewin-*.iso
```

---

## 5. 소스 코드 디렉터리 구조

```text
├── boot/                   # GRUB 부트로더 설정
├── packaging/archiso/      # Archiso 프로필 (airootfs, systemd 서비스, 커널 파라미터)
├── scripts/                # ISO 빌드 및 검증 자동화 스크립트
├── src/
│   ├── core/               # 디스크 인벤토리, 파티셔닝, ESD 카탈로그, 무인 설치 생성기
│   ├── platform/           # HTTP 다운로더, 프로세스 실행기, WinPE 부트스트래퍼, 설치 파이프라인
│   ├── ui/                 # ANSI 터미널 엔진, Catppuccin TUI 마법사
│   └── main.cpp            # 메인 엔트리포인트 및 CLI/TUI 제어 루프
├── tests/                  # 단위 테스트 및 파이프라인 통합 테스트
└── docs/                   # 릴리스 후보(RC) 감사 보고서 및 검증 매트릭스
```
