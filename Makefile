# tiny-web-server Makefile
#
#   make        : 서버 빌드 (build/tiny)
#   make test   : 단위 테스트 빌드 + 실행 (하나라도 실패하면 make가 실패)
#   make asan   : AddressSanitizer/UBSan 빌드 (build/tiny-asan) + 단위 테스트 실행
#   make e2e    : 실제 서버를 띄우고 curl 로 기능 확인 (tests/e2e.sh)
#   make test-proxy : 로컬 백엔드 두 대로 Reverse Proxy 검증 (Python 3)
#                 make e2e E2E_BIN=build/tiny-asan  → ASan 서버로 (종료 때 LeakSanitizer 누수 검사)
#   make valgrind : 디버그 빌드(build/tiny-debug)를 valgrind 로 감싸 e2e 실행 → 누수/메모리 오류/fd 집계
#   make bench  : ab/wrk 부하 측정 → build/bench/results.md (tests/bench.sh)
#   make clean  : build/ 삭제

CC      ?= cc
BUILD   := build

# 공통 옵션: C11 + POSIX 함수 사용, 경고는 모두 켜고 경고=에러로 취급
#   _XOPEN_SOURCE=700: POSIX 2008 + X/Open(XSI) 확장. glibc 는 realpath 를 이 모드에서만 선언한다
#   (-O2 에서는 _FORTIFY_SOURCE 헤더가 우연히 선언해 줘서 가려져 있었다 → -O0 디버그 빌드에서 발견)
CFLAGS_COMMON := -std=c11 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 \
                 -Wall -Wextra -Werror -pthread -Isrc -MMD -MP
CFLAGS_REL    := -O2
CFLAGS_ASAN   := -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined
LDFLAGS       := -pthread

# 소스 목록 (파일을 추가하면 자동으로 포함됨)
SRCS      := $(wildcard src/*.c)
LIB_SRCS  := $(filter-out src/main.c,$(SRCS))   # main.c 를 뺀 나머지 = 테스트에서 링크할 모듈
TEST_SRCS := $(wildcard tests/test_*.c)

# 일반 빌드 결과물 경로
OBJS      := $(SRCS:%.c=$(BUILD)/obj/%.o)
LIB_OBJS  := $(LIB_SRCS:%.c=$(BUILD)/obj/%.o)
TEST_BINS := $(TEST_SRCS:tests/%.c=$(BUILD)/tests/%)

# ASan 빌드 결과물 경로 (일반 빌드와 섞이지 않게 별도 폴더)
ASAN_OBJS      := $(SRCS:%.c=$(BUILD)/asan/obj/%.o)
ASAN_LIB_OBJS  := $(LIB_SRCS:%.c=$(BUILD)/asan/obj/%.o)
ASAN_TEST_BINS := $(TEST_SRCS:tests/%.c=$(BUILD)/asan/tests/%)

.PHONY: all test asan e2e test-proxy valgrind bench clean

# 테스트용 .o 같은 중간 파일을 make 가 자동 삭제하지 않게 한다 (재빌드 방지)
.SECONDARY:

# C 로 만든 CGI 예제 (cgi-bin/adder.c → cgi-bin/adder)
CGI_SRCS := $(wildcard cgi-bin/*.c)
CGI_BINS := $(CGI_SRCS:.c=)

all: $(BUILD)/tiny $(CGI_BINS)

cgi-bin/%: cgi-bin/%.c
	$(CC) -std=c11 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Wall -Wextra -Werror -O2 $< -o $@

# ---------- 일반 빌드 ----------
$(BUILD)/tiny: $(OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_REL) -c $< -o $@

$(BUILD)/tests/%: $(BUILD)/obj/tests/%.o $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $^ -o $@ $(LDFLAGS)

# ---------- ASan 빌드 ----------
$(BUILD)/tiny-asan: $(ASAN_OBJS)
	$(CC) $(CFLAGS_ASAN) $^ -o $@ $(LDFLAGS)

$(BUILD)/asan/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_ASAN) -c $< -o $@

$(BUILD)/asan/tests/%: $(BUILD)/asan/obj/tests/%.o $(ASAN_LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_ASAN) $^ -o $@ $(LDFLAGS)

# ---------- 테스트 실행 ----------
# 각 테스트 실행 파일을 차례로 돌리고, 하나라도 실패하면 종료 코드 1
define run_tests
	@fail=0; \
	for t in $(1); do \
		echo "== $$t"; \
		./$$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "*** 실패한 테스트가 있습니다 ***"; exit 1; fi; \
	echo "== 모든 테스트 통과"
endef

test: $(TEST_BINS)
	$(call run_tests,$(TEST_BINS))

asan: $(BUILD)/tiny-asan $(ASAN_TEST_BINS)
	$(call run_tests,$(ASAN_TEST_BINS))

# ---------- 디버그 빌드 (valgrind 용: 최적화 없이 + 줄 번호 정보) ----------
DEBUG_OBJS := $(SRCS:%.c=$(BUILD)/debug/obj/%.o)

$(BUILD)/tiny-debug: $(DEBUG_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

$(BUILD)/debug/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_COMMON) -g -O0 -c $< -o $@

# ---------- 기능 테스트 (e2e) ----------
E2E_BIN ?= $(BUILD)/tiny

e2e: $(E2E_BIN) $(CGI_BINS)
	E2E_BIN=$(E2E_BIN) bash tests/e2e.sh

test-proxy: $(E2E_BIN)
	python3 tests/test_reverse_proxy.py $(E2E_BIN)

valgrind: $(BUILD)/tiny-debug $(CGI_BINS)
	E2E_BIN=$(BUILD)/tiny-debug E2E_VALGRIND=1 bash tests/e2e.sh

# ---------- 부하 측정 ----------
bench: $(BUILD)/tiny $(CGI_BINS)
	bash tests/bench.sh

clean:
	rm -rf $(BUILD) $(CGI_BINS)

# 헤더 파일 변경 시 자동 재컴파일을 위한 의존성 파일
-include $(OBJS:.o=.d) $(ASAN_OBJS:.o=.d) $(DEBUG_OBJS:.o=.d) \
         $(TEST_SRCS:%.c=$(BUILD)/obj/%.d) $(TEST_SRCS:%.c=$(BUILD)/asan/obj/%.d)
