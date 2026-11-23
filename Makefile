# 빌드 규칙. 결과물은 build/(오브젝트)와 bin/(실행 파일)에 만든다.
CC       ?= gcc
CFLAGS   := -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -g -O1
LDFLAGS  :=
SANITIZE ?=

ifeq ($(SANITIZE),address)
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif
ifeq ($(SANITIZE),thread)
CFLAGS  += -fsanitize=thread
LDFLAGS += -fsanitize=thread
endif

BUILD := build
BIN   := bin

LIB_SOURCES  := src/rio.c src/net.c src/http.c src/files.c src/cgi.c src/connection.c
LIB_OBJECTS  := $(LIB_SOURCES:src/%.c=$(BUILD)/%.o)
UNIT_SOURCES := $(wildcard tests/unit/test_*.c)
UNIT_BINS    := $(UNIT_SOURCES:tests/unit/%.c=$(BIN)/%)

.PHONY: all test e2e asan tsan clean

all: $(BIN)/webserver

$(BIN)/webserver: $(BUILD)/main.o $(LIB_OBJECTS) | $(BIN)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/%.o: src/%.c src/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN)/test_%: tests/unit/test_%.c tests/unit/test.h $(LIB_OBJECTS) | $(BIN)
	$(CC) $(CFLAGS) -Isrc $(LDFLAGS) -o $@ $< $(LIB_OBJECTS)

# 단위 테스트: 소켓 없이 모듈 하나씩
test: $(UNIT_BINS)
	@set -e; for t in $(UNIT_BINS); do ./$$t; done

# 기능 테스트: 서버를 실제로 띄우고 nc/curl로 확인
e2e: all
	@bash tests/e2e/run_all.sh

# 메모리 오류 검사: 깨끗하게 다시 빌드한 뒤 단위+기능 테스트를 ASan으로 돌린다
asan:
	$(MAKE) clean
	$(MAKE) SANITIZE=address test e2e

tsan:
	$(MAKE) clean
	$(MAKE) SANITIZE=thread test e2e

$(BUILD) $(BIN):
	mkdir -p $@

clean:
	rm -rf $(BUILD) $(BIN)
