# 인텔 최신 PC 및 노트북(11~14세대) VMD 디스크 미인식 해결 가이드

> **스마트폰 QR 코드로 들어오신 분들을 위한 공식 가이드입니다.**
> 인텔 11세대 이상 최신 프로세서가 탑재된 노트북 및 조립/브랜드 데스크톱 PC에서 설치 대상 디스크(SSD)가 화면에 나타나지 않을 때 제조사 공식 지침에 따라 해결하는 방법입니다.

---

## 1. 왜 내 SSD가 화면에 나타나지 않나요?

인텔 11세대~14세대(Core Ultra 시리즈 포함) PC 및 노트북에는 **Intel VMD(Volume Management Device)**라는 하드웨어 RAID 가상화 기술이 기본으로 켜져 있습니다.

마이크로소프트의 공식 Windows 11 순정 설치 미디어조차 이 장치를 구동할 수 있는 드라이버가 기본 내장되어 있지 않아, **"드라이브를 찾을 수 없습니다"**라는 오류가 발생하며 하드디스크가 숨겨지게 됩니다.

---

## 2. 해결 방법: BIOS에서 Intel VMD 끄기 (제조사 공식 권장)

바이오스(BIOS) 설정에서 **Intel VMD를 끄면(`Disabled`)**, 숨겨져 있던 NVMe SSD가 마이크로소프트 및 리눅스 표준 NVMe 인터페이스로 즉시 연결되어 **추가 드라이버 설치 없이 100% 정상 인식**됩니다.

> [!TIP]
> **단일 SSD를 사용하는 일반 PC 및 노트북에서는 VMD를 꺼도 속도나 성능 저하가 전혀 발생하지 않습니다.** 오히려 마이크로소프트 순정 표준 드라이버(`stornvme`)로 직결되므로 향후 윈도우 업데이트 시 드라이버 충돌로 인한 블루스크린 위험이 완전히 사라집니다.

---

## 3. 데스크톱 PC 메인보드 제조사별 가이드

PC 전원을 켜자마자 화면이 나오기 전에 **`Del` 키 또는 `F2` 키를 연타**하여 BIOS에 진입합니다.

### [ASUS] 에이수스 메인보드 (ROG, TUF, PRIME)
1. 부팅 시 **`Del` 키**를 연타하여 BIOS 진입 후, **`F7` 키**를 눌러 **`Advanced Mode`**로 전환합니다.
2. 상단 메뉴의 **`Advanced`** $\rightarrow$ **`VMD setup menu`**를 클릭합니다.
3. **`Enable VMD controller`** 항목을 **`Disabled`**로 변경합니다.
4. **`F10` 키**를 누르고 `OK`를 선택하여 저장 후 재부팅합니다.
- 🔗 [ASUS 공식 고객지원 가이드](https://www.asus.com/support/faq/1044458)

---

### [MSI] MSI 메인보드 (MPG, MAG, PRO)
1. 부팅 시 **`Del` 키**를 연타하여 BIOS 진입 후, **`F7` 키**를 눌러 **`Advanced Mode`**로 전환합니다.
2. **`Settings`** $\rightarrow$ **`Advanced`** $\rightarrow$ **`Integrated Peripherals`**로 이동합니다.
3. **`RAID Configuration (Intel VMD)`** 항목을 **`Disabled`**로 변경합니다.
4. **`F10` 키**를 눌러 저장 후 재부팅합니다.
- 🔗 [MSI 공식 기술 지원 안내](https://www.msi.com/support/technical_details/MB_BIOS_Intel_RAID)

---

### [GIGABYTE / AORUS] 기가바이트 메인보드
1. 부팅 시 **`Del` 키**를 연타하여 BIOS 진입 후, **`F2` 키**를 눌러 **`Advanced Mode`**로 전환합니다.
2. 상단 메뉴의 **`Settings`** $\rightarrow$ **`IO Ports`** $\rightarrow$ **`VMD setup menu`**로 이동합니다.
3. **`Enable VMD controller`** 항목을 **`Disabled`**로 변경합니다.
4. **`F10` 키**를 눌러 저장 후 재부팅합니다.
- 🔗 [GIGABYTE 공식 FAQ 가이드](https://www.gigabyte.com/Support/FAQ/4078)

---

### [ASRock] 애즈락 메인보드
1. 부팅 시 **`Del` 키** 또는 **`F2` 키**를 연타하여 BIOS로 진입합니다.
2. 상단 메뉴의 **`Advanced`** $\rightarrow$ **`Storage Configuration`** $\rightarrow$ **`VMD Configuration`**으로 이동합니다.
3. **`Enable VMD controller`** 항목을 **`Disabled`**로 변경합니다.
4. **`F10` 키**를 눌러 저장 후 재부팅합니다.
- 🔗 [ASRock 공식 지원 FAQ](https://www.asrock.com/support/faq.asp?id=506)

---

## 4. 노트북 제조사별 BIOS 설정 가이드

컴퓨터 전원을 끈 상태에서 전원을 켜자마자 화면이 나오기 전에 **해당 제조사의 BIOS 진입 키를 연타**합니다.

### [삼성] 갤럭시북 시리즈
1. 전원을 켜자마자 **`F2` 키**를 연타하여 **Samsung BIOS Configuration**으로 진입합니다.
2. 메뉴에서 **`Advanced`** 탭을 클릭합니다.
3. **`VMD Controller`** 항목을 찾아서 **`Off`** (또는 `Disabled`)로 변경합니다.
4. **`F10` 키**를 누르고 **`Yes`**를 선택하여 설정을 저장하고 재부팅합니다.

---

### [LG] 그램 (Gram) 시리즈
1. 전원을 켜자마자 **`F2` 키**를 연타하여 **LG BIOS Setup**으로 진입합니다.
2. 상단 메뉴의 **`Advanced`** 탭으로 이동합니다.
3. **`Intel VMD`** 항목을 찾아 **`Disabled`**로 변경합니다.
4. **`F10` 키**를 누른 뒤 저장하고 재부팅합니다.

---

### [ASUS] 노트북 (젠북, 비보북, ROG)
1. 전원을 켜자마자 **`F2` 키**를 연타하여 BIOS 화면으로 들어간 뒤 **`F7` 키**(`Advanced Mode`)를 누릅니다.
2. **`Advanced`** $\rightarrow$ **`VMD setup menu`**로 이동합니다.
3. **`Enable VMD controller`** 항목을 **`Disabled`**로 변경하고 안내 창에서 **`Ok`**를 누릅니다.
4. **`F10` 키**를 눌러 저장 후 재부팅합니다.
- 🔗 [ASUS 노트북 공식 가이드](https://www.asus.com/support/faq/1044458)

---

### [Dell] 델 (XPS, Inspiron, Latitude, Alienware)
1. 전원을 켜자마자 **`F2` 키**를 연타하여 BIOS로 들어갑니다.
2. 좌측 메뉴의 **`Storage`** $\rightarrow$ **`SATA Operation`** (또는 `Storage Controller`)으로 이동합니다.
3. 기존의 **`RAID On`** 설정을 **`AHCI / NVMe`**로 변경합니다.
4. **`Apply Changes`**를 누르고 **`Exit`**로 재부팅합니다.
- 🔗 [Dell 공식 기술 지원 문서](https://www.dell.com/support/kbdoc/000188116)

---

### [Lenovo] 레노버 (아이디어패드, 씽크패드, 요가, 리전)
1. 전원을 켜자마자 **`F2` 키**를 연타합니다. (또는 본체 측면의 Novo 버튼 사용)
2. **`Configuration`** 탭으로 이동합니다.
3. **`Intel VMD Controller`** 항목을 **`Disabled`**로 변경합니다.
4. **`F10` 키**를 눌러 저장 후 재부팅합니다.
- 🔗 [Lenovo 공식 기술 지원 안내](https://support.lenovo.com/solutions/ht506197)

---

### [HP] 휴렛팩커드
1. 전원을 켜자마자 **`F10` 키**를 연타하여 BIOS Setup으로 진입합니다.
2. **`Configuration`** 또는 **`Advanced`** $\rightarrow$ **`Storage`** 메뉴로 들어갑니다.
3. **`Intel VMD`**를 찾아 **`Disabled`**로 변경합니다.
4. **`F10` 키**를 눌러 저장 후 재부팅합니다.
- 🔗 [HP 공식 고객지원 가이드](https://support.hp.com/document/ish_4859717-4859823-16)

---

## 5. 변경 완료 후 다시 진행하기

위 설정 후 Winstaller 부팅 USB로 다시 켜면:
1. 저장장치 선택 화면에 내 고속 NVMe SSD가 바로 나타납니다.
2. 디스크를 선택하고 윈도우 설치를 진행하시면 마이크로소프트 표준 드라이버로 안정적인 클린 설치가 완료됩니다!
