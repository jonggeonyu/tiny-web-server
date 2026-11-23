#!/usr/bin/env bash
#
# e2e.sh: 실제 서버를 띄우고 curl/nc 로 기능을 확인하는 자동 테스트.
#
#   make e2e                                   # 일반 빌드로
#   make e2e E2E_BIN=build/tiny-asan           # ASan 빌드로 (누수 검사는 10-4 에서)
#   E2E_WRAP="valgrind -q" E2E_SKIP_SLOW=1 bash tests/e2e.sh
#
# 환경 변수
#   E2E_BIN        실행할 서버 (기본 build/tiny)
#   E2E_WRAP       서버 앞에 붙일 명령 (예: valgrind ...)
#   E2E_SKIP_SLOW  1 이면 몇 초씩 걸리는 시간 초과 테스트를 건너뜀 (valgrind 처럼 느린 실행용)
#
# 안전 규칙 (예전에 겪은 문제들): 서버 PID 를 직접 저장해서 그것만 종료, 모든 curl 에 --max-time,
# wait 는 PID 를 지정해서, 끝나면 trap 으로 서버와 임시 폴더를 반드시 정리.

set -u

E2E_BIN=${E2E_BIN:-build/tiny}
E2E_WRAP=${E2E_WRAP:-}
E2E_SKIP_SLOW=${E2E_SKIP_SLOW:-0}
START_WAIT_S=10
[ -n "$E2E_WRAP" ] && START_WAIT_S=60 # valgrind 는 시작이 느리다

cd "$(dirname "$0")/.." || exit 1
REPO=$(pwd)

if [ ! -x "$E2E_BIN" ]; then
    echo "서버 실행 파일이 없음: $E2E_BIN (먼저 make)" >&2
    exit 1
fi

# ------------------------------------------------------------------ 결과 집계
PASS=0
FAIL=0
FAILED_NAMES=()

ok() { PASS=$((PASS + 1)); printf '  \033[32mOK\033[0m   %s\n' "$1"; }
ng() {
    FAIL=$((FAIL + 1)); FAILED_NAMES+=("$1")
    printf '  \033[31mFAIL\033[0m %s\n' "$1"
    [ $# -ge 2 ] && printf '       %s\n' "$2"
}
# check 이름 기대값 실제값
check() { if [ "$2" = "$3" ]; then ok "$1"; else ng "$1" "기대: [$2]  실제: [$3]"; fi; }
# check_has 이름 찾을문자열 내용
check_has() { if printf '%s' "$3" | grep -qF -- "$2"; then ok "$1"; else ng "$1" "[$2] 가 없음"; fi; }
check_lt() { if [ "$3" -lt "$2" ]; then ok "$1"; else ng "$1" "기대: < $2  실제: $3"; fi; }
section() { printf '\n### %s\n' "$1"; }

# ------------------------------------------------------------------ 준비: 임시 폴더
WORK=$(mktemp -d "${TMPDIR:-/tmp}/tiny-e2e.XXXXXX") || exit 1
ROOT=$WORK/www
CGI=$WORK/cgi-bin
cp -r "$REPO/www" "$ROOT"
cp -rp "$REPO/cgi-bin" "$CGI"                 # -p: 실행 권한 유지
echo "OUTSIDE SECRET" > "$WORK/outside.txt"
echo "secret" > "$ROOT/secret.txt" && chmod 000 "$ROOT/secret.txt"
ln -s ../outside.txt "$ROOT/escape.txt"        # 루트 밖을 가리키는 링크
ln -s ../outside.txt "$CGI/escape.sh"
head -c 3000000 /dev/urandom > "$ROOT/big.bin"
head -c 1048576 /dev/urandom > "$WORK/body1m.bin"
head -c 500000 /dev/urandom > "$WORK/body500k.bin"
head -c 2000000 /dev/zero > "$WORK/body2m.bin"

# ------------------------------------------------------------------ 서버 실행/종료
SRV_PIDS=()

free_port() {
    python3 -I -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'
}

# start_server 변수이름 [추가 인자...]  → 변수이름_PID, 변수이름_PORT, 변수이름_LOG 설정
start_server() {
    local name=$1; shift
    local port; port=$(free_port)
    local log=$WORK/$name.log
    # shellcheck disable=SC2086
    $E2E_WRAP "$E2E_BIN" -p "$port" -r "$ROOT" -c "$CGI" "$@" > "$log" 2>&1 &
    local pid=$!
    SRV_PIDS+=("$pid")
    printf -v "${name}_PID" '%s' "$pid"
    printf -v "${name}_PORT" '%s' "$port"
    printf -v "${name}_LOG" '%s' "$log"
    local i
    for ((i = 0; i < START_WAIT_S * 10; i++)); do
        if curl -s --max-time 1 -o /dev/null "http://127.0.0.1:$port/"; then return 0; fi
        if ! kill -0 "$pid" 2>/dev/null; then break; fi
        sleep 0.1
    done
    echo "서버 시작 실패 ($name). 로그:" >&2
    cat "$log" >&2
    return 1
}

# stop_server PID [시그널] → 종료 코드를 STOP_RC 에
# 기본은 SIGTERM: 스크립트에서 & 로 띄운 프로그램은 bash 규칙상 SIGINT 를 "무시" 상태로 시작하므로,
# 서버가 직접 처리기를 등록하기 전(10-3 이전)에는 SIGINT 로 끌 수 없다.
stop_server() {
    local pid=$1 sig=${2:-TERM} i
    STOP_RC=
    kill -"$sig" "$pid" 2>/dev/null
    for ((i = 0; i < 300; i++)); do # 최대 30초
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" 2>/dev/null
            STOP_RC=$?
            return 0
        fi
        sleep 0.1
    done
    kill -KILL "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    STOP_RC=killed
    return 1
}

cleanup() {
    local p
    for p in "${SRV_PIDS[@]}"; do kill -KILL "$p" 2>/dev/null; done
    chmod -R u+rwX "$WORK" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

# ------------------------------------------------------------------ 도우미
C="curl -s --max-time 15"
status() { $C -o /dev/null -w '%{http_code}' "$@"; }
# raw 포트 : 표준 입력을 그대로 보내고 응답 전체를 출력 (서버가 닫을 때까지)
raw() { timeout 10 nc -N 127.0.0.1 "$1"; }

start_server S -T 2 || exit 1
U=http://127.0.0.1:$S_PORT
echo "서버: $E2E_WRAP $E2E_BIN  (포트 $S_PORT, 유휴 2초)"
BASE_FDS=$(ls "/proc/$S_PID/fd" 2>/dev/null | wc -l)

# ================================================================== 정적 파일
section "정적 파일"
check "GET / → 200" 200 "$(status "$U/")"
check "/ 의 Content-Type" "text/html; charset=utf-8" "$($C -o /dev/null -w '%{content_type}' "$U/")"
check "style.css 의 Content-Type" "text/css; charset=utf-8" "$($C -o /dev/null -w '%{content_type}' "$U/style.css")"
check "PNG 내용이 원본과 같음" same "$($C "$U/images/gradient.png" | cmp -s - "$ROOT/images/gradient.png" && echo same)"
check "3MB 파일 내용이 같음" same "$($C "$U/big.bin" | cmp -s - "$ROOT/big.bin" && echo same)"
check "/docs → 301" 301 "$(status "$U/docs")"
check "/docs 의 Location" "$U/docs/" "$($C -o /dev/null -w '%{redirect_url}' "$U/docs")"
check "/docs/ → 200" 200 "$(status "$U/docs/")"
check "없는 파일 → 404" 404 "$(status "$U/nope.html")"
check "../ → 403" 403 "$(status --path-as-is "$U/../PROJECT.md")"
check "%2e%2e → 403" 403 "$(status --path-as-is "$U/%2e%2e/PROJECT.md")"
check "읽기 권한 없는 파일 → 403" 403 "$(status "$U/secret.txt")"
check "루트 밖 심볼릭 링크 → 403" 403 "$(status "$U/escape.txt")"
check "루트 밖 내용이 새지 않음" "" "$($C "$U/escape.txt" | grep -o 'OUTSIDE SECRET')"
H=$($C -I "$U/images/gradient.png" | tr -d '\r')
check_has "HEAD: Content-Length 는 실제 크기" "Content-Length: $(stat -c %s "$ROOT/images/gradient.png")" "$H"
check "HEAD: 본문 없음" 0 "$($C -I -o /dev/null -w '%{size_download}' "$U/images/gradient.png")"
check "정적 파일에 POST → 405" 405 "$(status -d x "$U/index.html")"
check_has "405 의 Allow 헤더" "Allow: GET, HEAD" "$($C -i -d x "$U/index.html" | tr -d '\r')"

# ================================================================== 잘못된 요청
section "잘못된 요청"
first_line() { head -1 | tr -d '\r'; }
check "GARBAGE → 400" "HTTP/1.1 400 Bad Request" "$(printf 'GARBAGE\r\n\r\n' | raw "$S_PORT" | first_line)"
check "HTTP/2.0 → 505" "HTTP/1.1 505 HTTP Version Not Supported" "$(printf 'GET / HTTP/2.0\r\nHost: x\r\n\r\n' | raw "$S_PORT" | first_line)"
check "http/1.1 (소문자) → 400" "HTTP/1.1 400 Bad Request" "$(printf 'GET / http/1.1\r\nHost: x\r\n\r\n' | raw "$S_PORT" | first_line)"
check "DELETE → 501" 501 "$(status -X DELETE "$U/")"
check "get (소문자 메서드) → 501" "HTTP/1.1 501 Not Implemented" "$(printf 'get / HTTP/1.1\r\nHost: x\r\n\r\n' | raw "$S_PORT" | first_line)"
check "Host 없음 → 400" "HTTP/1.1 400 Bad Request" "$(printf 'GET / HTTP/1.1\r\n\r\n' | raw "$S_PORT" | first_line)"
check "URI 5000자 → 414" 414 "$(status "$U/$(head -c 5000 /dev/zero | tr '\0' a)")"
check "헤더 9000바이트 → 431" 431 "$(status -H "X-Big: $(head -c 9000 /dev/zero | tr '\0' a)" "$U/")"
check "Transfer-Encoding: chunked → 501" 501 "$(status -H 'Transfer-Encoding: chunked' -d abc "$U/echo")"
check "%zz → 400" 400 "$(status "$U/%zz")"
check "Content-Length 19자리 → 413" "HTTP/1.1 413 Content Too Large" "$(printf 'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 9999999999999999999\r\n\r\n' | raw "$S_PORT" | first_line)"
check "Content-Length 두 개가 다름 → 400" "HTTP/1.1 400 Bad Request" "$(printf 'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab' | raw "$S_PORT" | first_line)"
check "헤더 보내다 멈춤 → 408 (유휴 2초)" "HTTP/1.1 408 Request Timeout" "$( (printf 'GET / HTTP/1.1\r\nHo'; sleep 4) | timeout 10 nc -q0 127.0.0.1 "$S_PORT" | first_line)"

# ================================================================== 본문
section "POST 본문"
check "짧은 POST echo" "name=kim" "$($C -d 'name=kim' "$U/echo")"
check "500KB 바이너리 echo" same "$($C --data-binary @"$WORK/body500k.bin" "$U/echo" | cmp -s - "$WORK/body500k.bin" && echo same)"
check "정확히 1MiB echo" same "$($C --data-binary @"$WORK/body1m.bin" "$U/echo" | cmp -s - "$WORK/body1m.bin" && echo same)"
V=$($C -v --data-binary @"$WORK/body2m.bin" -o /dev/null -w 'CODE=%{http_code} UP=%{size_upload}' "$U/echo" 2>&1)
check_has "2MB → 413" "CODE=413" "$V"
check_has "2MB: curl 이 Expect: 100-continue 를 붙임" "> Expect: 100-continue" "$V"
check_has "2MB: 본문을 보내지 않고 끝남" "UP=0" "$V"
check_has "Expect 직접 지정 → 100 Continue" "< HTTP/1.1 100 Continue" "$($C -v -H 'Expect: 100-continue' -d hello "$U/echo" 2>&1)"
check "이상한 Expect → 417" 417 "$(status -H 'Expect: teapot' -d x "$U/echo")"
check "본문이 덜 오고 끊김 → 400" "HTTP/1.1 400 Bad Request" "$(printf 'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc' | raw "$S_PORT" | first_line)"
check "GET /echo → 405" 405 "$(status "$U/echo")"

# ================================================================== CGI
section "CGI"
check "hello.sh → 200" 200 "$(status "$U/cgi-bin/hello.sh")"
check "adder?a=3&b=4" "3 + 4 = 7" "$($C "$U/cgi-bin/adder?a=3&b=4")"
check "adder 잘못된 입력 → 400 (CGI 가 정한 Status)" 400 "$(status "$U/cgi-bin/adder?a=x")"
check "notfound.sh → 404" 404 "$(status "$U/cgi-bin/notfound.sh")"
check "1MiB 넣으며 1MiB 받기 (교착 없음)" same "$($C --data-binary @"$WORK/body1m.bin" "$U/cgi-bin/echo_post.sh" | cmp -s - "$WORK/body1m.bin" && echo same)"
ENVOUT=$($C -H 'Proxy: http://evil' "$U/cgi-bin/env.sh/extra/path?q=1")
check_has "PATH_INFO 전달" "PATH_INFO=/extra/path" "$ENVOUT"
check_has "QUERY_STRING 전달" "QUERY_STRING=q=1" "$ENVOUT"
check "HTTP_PROXY 차단 (httpoxy)" "" "$(printf '%s' "$ENVOUT" | grep '^HTTP_PROXY=')"
check "CGI 의 fd 는 0,1,2,3 뿐" "0 1 2 3" "$($C "$U/cgi-bin/fds.sh" | awk 'NR>1{print $(NF-2)}' | sort -n | tr '\n' ' ' | sed 's/ $//')"
check "실행 권한 없음 → 403" 403 "$(status "$U/cgi-bin/noexec.sh")"
check "CGI 디렉토리 밖 링크 → 403" 403 "$(status "$U/cgi-bin/escape.sh")"
check "/cgi-bin/ → 403" 403 "$(status "$U/cgi-bin/")"
check "crash.sh → 502" 502 "$(status "$U/cgi-bin/crash.sh")"
check "badoutput.sh → 502" 502 "$(status "$U/cgi-bin/badoutput.sh")"
if [ "$E2E_SKIP_SLOW" = 1 ]; then
    echo "  (건너뜀) forever.sh → 504 (5초 걸림)"
else
    check "forever.sh → 504 (시간 초과)" 504 "$(status "$U/cgi-bin/forever.sh")"
fi

# ================================================================== keep-alive
section "keep-alive / 파이프라이닝"
check_has "curl 이 연결을 재사용" "Reusing existing" "$($C -v -o /dev/null -o /dev/null "$U/" "$U/style.css" 2>&1)"
P=$(printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /hello.txt HTTP/1.1\r\nHost: x\r\n\r\nGET /style.css HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | raw "$S_PORT" | tr -d '\r')
check "파이프라이닝 3개 → 응답 3개" 3 "$(printf '%s\n' "$P" | grep -ac '^HTTP/1.1 200')"
check "마지막만 Connection: close" "keep-alive keep-alive close" "$(printf '%s\n' "$P" | grep -a '^Connection:' | awk '{print $2}' | tr '\n' ' ' | sed 's/ $//')"
check "HTTP/1.0 기본은 닫기 → 응답 1개" 1 "$(printf 'GET / HTTP/1.0\r\n\r\nGET / HTTP/1.0\r\n\r\n' | raw "$S_PORT" | grep -ac '^HTTP/1.1')"
check "404 뒤에도 연결 유지 → 응답 2개" 2 "$(printf 'GET /nope HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | raw "$S_PORT" | grep -ac '^HTTP/1.1')"

# ================================================================== 동시 처리
section "동시 처리"
if [ "$E2E_SKIP_SLOW" = 1 ]; then
    echo "  (건너뜀) 2초 CGI 10개 동시"
else
    t0=$(date +%s%N); pids=()
    for i in $(seq 10); do $C -o /dev/null "$U/cgi-bin/slow.sh" & pids+=($!); done
    wait "${pids[@]}"
    check_lt "2초 CGI 10개 동시 → 4초 안에 (ms)" 4000 $(( ($(date +%s%N) - t0) / 1000000 ))
fi

start_server B -t 1 -q 1 || exit 1
UB=http://127.0.0.1:$B_PORT
$C -o /dev/null "$UB/cgi-bin/slow.sh" & p1=$!
sleep 0.3
$C -o /dev/null "$UB/cgi-bin/slow.sh" & p2=$!
sleep 0.3
check "일꾼 1, 대기열 1 이 다 찼을 때 → 503" 503 "$(status "$UB/")"
check_has "503 의 Retry-After" "Retry-After: 1" "$($C -i "$UB/" | tr -d '\r')"
wait "$p1" "$p2"
check "대기열이 빈 뒤 → 200" 200 "$(status "$UB/")"
stop_server "$B_PID"

# ================================================================== 마무리 점검
section "마무리 점검 (주 서버)"
check "서버가 살아 있음" 200 "$(status "$U/")"
check "자식 프로세스(좀비 포함) 없음" 0 "$(ps --ppid "$S_PID" --no-headers | wc -l)"
if [ -z "$E2E_WRAP" ]; then
    sleep 2.5 # keep-alive 로 쉬던 연결이 정리될 시간 (유휴 2초 + 여유)
    check "열린 fd 수가 시작 때와 같음" "$BASE_FDS" "$(ls "/proc/$S_PID/fd" | wc -l)"
fi
stop_server "$S_PID"
check "SIGTERM 으로 서버가 끝남" yes "$([ "$STOP_RC" != killed ] && echo yes)"

# ------------------------------------------------------------------ 결과
printf '\n== e2e: %d개 중 %d개 실패\n' $((PASS + FAIL)) "$FAIL"
if [ "$FAIL" -gt 0 ]; then
    printf '   실패: %s\n' "${FAILED_NAMES[@]}"
    echo "   서버 로그: (아래 마지막 20줄)"
    tail -20 "$S_LOG"
    exit 1
fi
exit 0
