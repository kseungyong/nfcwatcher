# nfcwatcher

macOS 한글 파일명을 **Windows 호환(NFC, 완성형)** 으로 자동 변환하는 백그라운드 데몬.

## 왜 필요한가

macOS와 Windows는 둘 다 UTF-8을 쓰지만 한글을 저장하는 **유니코드 정규화 방식**이 다릅니다.

| | 정규화 | "한" 저장 |
|---|---|---|
| macOS (Finder/앱) | **NFD** (분해형) | `ㅎ`+`ㅏ`+`ㄴ` = 3개 코드포인트 |
| Windows | **NFC** (완성형) | `한` = 1개 코드포인트 |

화면에는 똑같아 보여도 실제 바이트가 달라서, Mac에서 만든 파일을 ZIP·USB·네트워크·클라우드로
Windows에 넘기면 자모가 분리돼 보이거나(`ㅎㅏㄴ...`) 검색·비교가 안 됩니다.

`nfcwatcher`는 지정한 폴더를 감시하다가 새 파일이 생기면 **파일명을 NFC로 자동 rename** 합니다.
APFS는 rename으로 준 바이트를 그대로 보존하므로 NFC 이름이 유지됩니다.

> **범위 안내**: 이 도구는 **파일을 파일로 전달**하는 경우(USB·직접 복사 등)에 유효합니다.
> **이메일 첨부**는 macOS+메일 앱의 구조적 한계로 해결하지 못합니다. 자세한 근거는 아래
> [이메일 첨부 한계와 근거](#이메일-첨부-한계와-근거-중요)를 보세요.

## 특징

- **가벼움**: 유휴 시 CPU 0%, 메모리 약 2MB (네이티브 C + FSEvents, 폴링 없음)
- **완전 자동**: `launchd`로 로그인 시 자동 시작, 상시 백그라운드 상주
- **메뉴바 UI**: 자동 변환 On/Off, 감시 폴더 관리, 폴더 즉시 변환을 메뉴바에서 조작
- **안전**:
  - 디바운스로 복사 중인 파일은 완료 후에만 변환
  - 이미 NFC면 아무 것도 안 함 (멱등적, 무한루프 없음)
  - 임시/다운로드 확장자·숨김 파일 무시
  - 이름 충돌 시 기본은 건너뛰고 로그만 남김
  - 모든 변환을 로그와 UNDO 매핑으로 기록

## 설치

```sh
make install
```

빌드 → 감시 데몬(`~/.local/bin/nfcwatcher`) launchd 등록 → 메뉴바 앱
(`~/Applications/NFCWatcher.app`) 설치·실행까지 한 번에 수행합니다.

설치 후 메뉴바에 아이콘이 나타납니다.

## 메뉴바 앱

메뉴바 아이콘을 클릭하면:

```
● 자동 변환: 켜짐          ← 현재 상태
자동 변환 끄기              ← 즉시 On/Off (일시정지 플래그)
─────────────────────
감시 폴더
  ~/Downloads   ▸ 감시에서 제거
  ~/Desktop     ▸ 감시에서 제거
＋ 폴더 추가…
─────────────────────
⚡ 폴더 즉시 변환…          ← 폴더 선택 → 개수 미리보기 → 확인 → 변환
─────────────────────
로그 열기 / 설정 파일 열기
로그인 시 자동 시작  ☑
NFCWatcher 종료
```

- **자동 변환 On/Off**: 데몬을 끄지 않고 즉시 일시정지/재개합니다.
- **폴더 즉시 변환**: 선택한 폴더를 먼저 스캔해 "N개가 변환 대상입니다"를
  보여주고, 확인하면 변환합니다. (실수 방지)
- **감시 폴더 추가/제거**: 설정을 바꾸고 데몬을 자동 재적용합니다.

앱은 얇은 컨트롤러입니다. 실제 감시·변환은 C 데몬이 하고, 앱은 데몬을 제어만
하므로 앱을 종료해도 자동 변환은 계속됩니다.

## 사용법

설치하면 백그라운드에서 자동 동작하므로 별도 조작이 필요 없습니다.

```sh
nfcwatcher                 # 데몬 실행 (폴더 상시 감시)
nfcwatcher --scan          # 감시 폴더의 기존 파일을 한 번에 일괄 변환
nfcwatcher --config <path> # 다른 설정 파일 사용
nfcwatcher --version
```

## 설정

`~/.config/nfcwatcher/nfcwatcher.conf` (설치 시 자동 생성). 주요 항목:

```conf
watch = ~/Downloads          # 감시 폴더 (여러 줄 가능, 하위 폴더 재귀 감시)
watch = ~/Desktop
debounce_seconds = 2.0       # 복사 완료 대기 시간
conflict = skip              # skip | rename | overwrite
```

설정을 바꾼 뒤에는 데몬을 재시작하세요:

```sh
launchctl unload ~/Library/LaunchAgents/co.kdsys.nfcwatcher.plist
launchctl load   ~/Library/LaunchAgents/co.kdsys.nfcwatcher.plist
```

## 제거

```sh
make uninstall   # launchd 해제 + 바이너리 삭제 (설정·로그는 보존)
```

## 주의 사항

- **클라우드 동기화 폴더**(Dropbox, Google Drive, OneDrive 등)는 자체 정규화 로직이 있어
  데몬과 서로 파일명을 바꾸는 충돌이 생길 수 있습니다. 처음에는 로컬 폴더만 감시하고,
  클라우드 폴더는 로그를 확인하며 신중히 추가하세요.
- **USB/외장하드**는 파일시스템이 exFAT/FAT32/NTFS라 정규화 규칙이 다릅니다.
  현재 버전은 로컬 APFS 폴더 감시에 최적화되어 있습니다.

## 이 도구가 해결하는 것 / 못 하는 것

한 줄 요약: **파일을 "파일로" 전달할 때는 해결하고, "이메일 첨부"는 해결하지 못합니다.**

| 전달 방법 | Windows에서 파일명 | 이유 |
|---|---|---|
| USB·직접 복사 | ✅ 정상 | 디스크의 NFC 바이트가 그대로 복사됨 |
| 맥 내부 새 파일 | ✅ 정상 유지 | 데몬이 NFC로 자동 변환 |
| 이메일 첨부(맥) | ❌ 깨짐 | 메일 앱이 전송 시 이름을 NFD로 분해 |
| SMB 공유·클라우드 | ⚠️ 경로마다 다름 | 중간에서 재분해될 수 있음 |

## 이메일 첨부 한계와 근거 (중요)

**맥에서 이메일로 한글 이름 파일을 첨부해 보내면, 파일이 디스크에 NFC로 저장돼 있어도
받는 사람에게는 파일명이 NFD(자소 분리)로 전달됩니다.** 이건 이 도구로 고칠 수 없습니다.

### 실측 증거

같은 NFC 파일을 여러 경로로 보내 **받은 메일의 원본(.eml) 헤더 바이트**를 확인한 결과,
모든 경로가 NFD였습니다.

| 전송 경로 | MIME 헤더 파일명 |
|---|---|
| Outlook for Mac (데스크톱) | NFD |
| Outlook 웹 (브라우저 업로드) | NFD |
| OneDrive 첨부 | NFD |
| Apple 메일 | NFD |

### 원인

macOS API별로 같은 NFC 파일 이름을 읽어 비교하면, 딱 하나만 분해합니다.

| API | 결과 |
|---|---|
| `readdir`, `NSFileManager`, `NSURL`, `NSString.UTF8String` | NFC (정상) |
| **`NSString.fileSystemRepresentation`** | **NFD (분해)** |

메일 앱이 첨부 파일명을 얻을 때 `fileSystemRepresentation`(또는 동등한 경로)을 쓰면 NFD가
되고, 그게 MIME 헤더로 전송됩니다. 이건 Apple(분해하는 API)과 Microsoft(그 NFD를 그대로
싣고 수신 시 정규화도 안 하는 Outlook)의 조합이며, 둘 다 외부에서 바꿀 수 없습니다.

- **OS 라이브러리 교체(주입)도 막힘**: SIP 활성 + 메일 앱의 hardened runtime 서명 때문에
  dylib 주입이 거부됩니다. 뚫으려면 시스템 보안을 낮춰야 하고, 업데이트마다 깨지며 권장되지 않습니다.
- **참고**: Gmail은 표시할 때 NFC로 합쳐 보여줘 정상처럼 보이지만, **저장하면 NFD**입니다.
  즉 화면 표시는 신뢰할 수 없고, 실제 바이트만이 진실입니다.

### 회피책 (도구 밖)

- **zip으로 압축해 첨부**: zip은 바이트 컨테이너라 맥이 안쪽 이름을 못 건드립니다.
  단, 압축 안 이름은 NFC, general-purpose bit 11(UTF-8 플래그)을 켜야 Windows에서
  자소분리·글자깨짐이 모두 없습니다. 받는 사람이 압축을 풀어야 하는 비용이 있습니다.
- **Windows PC에서 전송**: 파일을 정상 경로로 옮긴 뒤 Windows에서 보내면 헤더가 NFC입니다.

## 구조

```
src/nfcwatcher.c                     감시/변환 데몬 (C)
menubar/main.m                       메뉴바 제어 앱 (Objective-C)
config/nfcwatcher.conf.example       설정 예시
launchd/co.kdsys.nfcwatcher.plist    데몬 launchd 템플릿
scripts/build_app.sh                 .app 번들 조립
scripts/install.sh / uninstall.sh    설치/제거
Makefile                             빌드/설치
```

## 요구 사항 (빌드)

- macOS 11+ / APFS 볼륨
- Xcode Command Line Tools의 `cc`(clang). Swift 툴체인은 불필요합니다.
