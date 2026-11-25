# Reverse Proxy 데모 빌드 파일
#   make        : ./backend, ./proxy 빌드
#   make clean  : 빌드 결과물 삭제
#   make run    : 빌드 후 백엔드 5개 + proxy 실행 (run.sh), Ctrl+C로 모두 종료
#   make asan   : 메모리 점검용 ./backend-asan, ./proxy-asan 빌드 → ASAN=1 ./run.sh 로 실행

CC       = gcc
# CLAUDE.md 규칙: 경고 없이 빌드되도록 -Wall -Wextra를 켜고, 스레드를 쓰므로 -pthread
CFLAGS   = -Wall -Wextra -O2 -pthread -std=c11
# -std=c11은 순수 C 표준만 켜므로, POSIX 함수(getaddrinfo, usleep, rand_r, realpath 등)를 쓰려면
# _DEFAULT_SOURCE가 필요합니다. 공용 헤더는 src/common에서 찾습니다.
CPPFLAGS = -D_DEFAULT_SOURCE -Isrc/common
# 트래픽 생성기의 지수 분포(log 함수)에 수학 라이브러리가 필요
LDLIBS   = -lm

COMMON_OBJS  = src/common/csapp.o src/common/http.o src/common/net.o
BACKEND_OBJS = src/backend/main.o
PROXY_OBJS   = src/proxy/main.o src/proxy/config.o src/proxy/router.o \
               src/proxy/static.o src/proxy/upstream.o src/proxy/health.o \
               src/proxy/events.o src/proxy/cache.o src/proxy/control.o \
               src/proxy/loadgen.o

# 메모리 점검용 빌드: AddressSanitizer(잘못된 메모리 접근·누수) + UndefinedBehaviorSanitizer.
# 보통 빌드와 섞이지 않도록 오브젝트 없이 소스에서 바로 다른 이름(-asan)으로 만듭니다.
ASAN_FLAGS   = -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1
COMMON_SRCS  = $(COMMON_OBJS:.o=.c)
BACKEND_SRCS = $(BACKEND_OBJS:.o=.c)
PROXY_SRCS   = $(PROXY_OBJS:.o=.c)
ALL_H        = $(wildcard src/common/*.h src/proxy/*.h)

.PHONY: all run clean asan

all: backend proxy

backend: $(BACKEND_OBJS) $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

proxy: $(PROXY_OBJS) $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# .c → .o (헤더가 바뀌어도 다시 컴파일되도록 의존성을 적어 둠)
COMMON_H = src/common/csapp.h src/common/http.h
src/common/csapp.o:   src/common/csapp.c src/common/csapp.h
src/common/http.o:    src/common/http.c $(COMMON_H)
src/common/net.o:     src/common/net.c src/common/net.h
src/backend/main.o:   src/backend/main.c $(COMMON_H)
src/proxy/main.o:     src/proxy/main.c $(COMMON_H) src/proxy/config.h src/proxy/router.h \
                      src/proxy/static.h src/proxy/upstream.h src/proxy/health.h \
                      src/proxy/events.h src/proxy/cache.h src/proxy/control.h \
                      src/proxy/loadgen.h
src/proxy/config.o:   src/proxy/config.c src/proxy/config.h
src/proxy/router.o:   src/proxy/router.c src/proxy/router.h src/proxy/config.h
src/proxy/static.o:   src/proxy/static.c src/proxy/static.h $(COMMON_H)
src/proxy/upstream.o: src/proxy/upstream.c src/proxy/upstream.h src/proxy/config.h \
                      src/proxy/router.h src/proxy/cache.h src/common/net.h $(COMMON_H)
src/proxy/health.o:   src/proxy/health.c src/proxy/health.h src/proxy/events.h \
                      src/proxy/router.h src/proxy/config.h src/common/net.h $(COMMON_H)
src/proxy/events.o:   src/proxy/events.c src/proxy/events.h src/common/net.h $(COMMON_H)
src/proxy/cache.o:    src/proxy/cache.c src/proxy/cache.h src/common/net.h
src/proxy/control.o:  src/proxy/control.c src/proxy/control.h src/proxy/cache.h \
                      src/proxy/events.h src/proxy/health.h src/proxy/router.h \
                      src/proxy/config.h src/common/net.h $(COMMON_H)
src/proxy/loadgen.o:  src/proxy/loadgen.c src/proxy/loadgen.h src/proxy/control.h \
                      src/proxy/events.h src/proxy/router.h src/proxy/config.h \
                      src/common/net.h $(COMMON_H)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

run: all
	./run.sh

asan: backend-asan proxy-asan

backend-asan: $(BACKEND_SRCS) $(COMMON_SRCS) $(ALL_H)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(ASAN_FLAGS) -o $@ $(BACKEND_SRCS) $(COMMON_SRCS)

proxy-asan: $(PROXY_SRCS) $(COMMON_SRCS) $(ALL_H)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(ASAN_FLAGS) -o $@ $(PROXY_SRCS) $(COMMON_SRCS) $(LDLIBS)

clean:
	rm -f backend proxy backend-asan proxy-asan $(BACKEND_OBJS) $(PROXY_OBJS) $(COMMON_OBJS)
