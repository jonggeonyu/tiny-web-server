# tiny-web-server Makefile
#
#   make        : 서버 빌드 (build/tiny)
#   make test   : 단위 테스트 빌드 + 실행 (하나라도 실패하면 make가 실패)
#   make asan   : AddressSanitizer/UBSan 빌드 (build/tiny-asan) + 단위 테스트 실행
#   make clean  : build/ 삭제

CC      ?= cc
BUILD   := build

# 공통 옵션: C11 + POSIX 함수 사용, 경고는 모두 켜고 경고=에러로 취급
CFLAGS_COMMON := -std=c11 -D_POSIX_C_SOURCE=200809L \
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

.PHONY: all test asan clean

# 테스트용 .o 같은 중간 파일을 make 가 자동 삭제하지 않게 한다 (재빌드 방지)
.SECONDARY:

all: $(BUILD)/tiny

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

clean:
	rm -rf $(BUILD)

# 헤더 파일 변경 시 자동 재컴파일을 위한 의존성 파일
-include $(OBJS:.o=.d) $(ASAN_OBJS:.o=.d) \
         $(TEST_SRCS:%.c=$(BUILD)/obj/%.d) $(TEST_SRCS:%.c=$(BUILD)/asan/obj/%.d)
