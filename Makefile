# nfcwatcher — macOS 한글 파일명 NFD→NFC 자동 변환
PREFIX ?= $(HOME)/.local
SRC     = src/nfcwatcher.c
APPSRC  = menubar/main.m
CFLAGS ?= -O2 -Wall -Wextra

.PHONY: all daemon app install uninstall run scan clean

all: daemon app

# C 감시 데몬
daemon: build/nfcwatcher
build/nfcwatcher: $(SRC)
	@mkdir -p build
	cc $(CFLAGS) -o $@ $< -framework CoreServices

# 메뉴바 앱 (.app 번들까지)
app:
	@bash scripts/build_app.sh

# 전체 설치 (데몬 launchd 등록 + 앱 설치·실행)
install:
	@bash scripts/install.sh

uninstall:
	@bash scripts/uninstall.sh

# 데몬 포그라운드 실행 (디버그)
run: build/nfcwatcher
	build/nfcwatcher

# 기존 파일 일괄 변환 (감시 폴더 대상)
scan: build/nfcwatcher
	build/nfcwatcher --scan

clean:
	rm -rf build
