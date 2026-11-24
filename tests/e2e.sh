#!/usr/bin/env bash
#
# e2e.sh: 실제 서버를 띄우고 curl/nc 로 기능을 확인하는 자동 테스트.
#
#   make e2e                                   # 일반 빌드로
#   make e2e E2E_BIN=build/tiny-asan           # ASan 빌드로 (누수 검사는 10-4 에서)
#   make valgrind                              # 디버그 빌드를 valgrind 로 감싸서 (누수/메모리 오류/fd 집계)
#
# 환경 변수
#   E2E_BIN        실행할 서버 (기본 build/tiny)
#   E2E_WRAP       서버 앞에 붙일 명령
#   E2E_VALGRIND   1 이면 서버마다 valgrind 로 감싸고, 끝에 결과를 집계 (build/valgrind-logs/ 에 로그 보관)
#   E2E_SKIP_SLOW  1 이면 몇 초씩 걸리는 시간 초과 테스트를 건너뜀 (valgrind 처럼 느린 실행용)
#
# 안전 규칙 (예전에 겪은 문제들): 서버 PID 를 직접 저장해서 그것만 종료, 모든 curl 에 --max-time,
# wait 는 PID 를 지정해서, 끝나면 trap 으로 서버와 임시 폴더를 반드시 정리.

set -u

E2E_BIN=${E2E_BIN:-build/tiny}
E2E_WRAP=${E2E_WRAP:-}
E2E_SKIP_SLOW=${E2E_SKIP_SLOW:-0}
E2E_VALGRIND=${E2E_VALGRIND:-0}
# ASan 빌드라면 종료할 때 LeakSanitizer 가 누수를 검사하도록 (리눅스 기본값이지만 명시)
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1}

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
# 시간(ms) 기준 검사: valgrind 처럼 느린 실행에서는 건너뜀
check_time() { if [ "$TIMED" = 1 ]; then check_lt "$@"; else echo "  (건너뜀: 느린 실행) $1"; fi; }
section() { printf '\n### %s\n' "$1"; }

# ------------------------------------------------------------------ 준비: 임시 폴더
WORK=$(mktemp -d "${TMPDIR:-/tmp}/tiny-e2e.XXXXXX") || exit 1
if [ "$E2E_VALGRIND" = 1 ]; then
    mkdir -p "$WORK/vg"
    # --leak-check=full          : 누수마다 어디서 할당했는지 보여줌
    # --show-leak-kinds=all      : definitely/indirectly/possibly lost, still reachable 모두 표시
    # --errors-for-leak-kinds=definite,indirect : 이 두 종류는 "에러"로 센다
    # --track-fds=yes            : 종료할 때 열려 있는 fd 목록 (fd 누수 검사)
    # --error-exitcode=99        : 에러가 있으면 서버 종료 코드를 99 로 → "종료 코드 0" 검사가 실패
    # --log-file=.../%p.log      : 프로세스(pid)마다 따로 기록
    E2E_WRAP="valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=definite,indirect \
--track-fds=yes --error-exitcode=99 --log-file=$WORK/vg/%p.log"
    E2E_SKIP_SLOW=1
fi
START_WAIT_S=10
[ -n "$E2E_WRAP" ] && START_WAIT_S=60 # valgrind 는 시작이 느리다
TIMED=1                                # 시간 기준 검사를 할지 (느린 실행에서는 의미가 없다)
[ -n "$E2E_WRAP" ] && TIMED=0
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
    if [ -n "${NOFILE:-}" ]; then
        # fd 한도를 낮춘 채 실행 (exec 하므로 pid 는 서버 자신)
        bash -c 'ulimit -n "$1"; shift; exec "$@"' _ "$NOFILE" "$E2E_BIN" -p "$port" -r "$ROOT" -c "$CGI" "$@" > "$log" 2>&1 &
    else
        # shellcheck disable=SC2086
        $E2E_WRAP "$E2E_BIN" -p "$port" -r "$ROOT" -c "$CGI" "$@" > "$log" 2>&1 &
    fi
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
# 기본은 SIGINT (Ctrl+C 와 같은 신호). 스크립트에서 & 로 띄운 프로그램은 bash 규칙상 SIGINT 를
# "무시" 상태로 물려받지만, 서버가 sigaction 으로 처리기를 직접 등록해서 덮어쓴다 (10-3).
stop_server() {
    local pid=$1 sig=${2:-INT} i
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

# ================================================================== 시작 실패
section "시작 실패 (포트 충돌, 잘못된 -p)"
# 서버를 띄우려다 실패해야 하는 경우들. 실수로 떠 버리면 끝나지 않으므로 timeout 으로 감싼다
# (5초 뒤 강제 종료 → 종료 코드 124 가 나와서 "1 이 아님"으로 실패가 드러난다).
# valgrind 로 감싸지 않는다: 느린 시작 때문에 5초 제한과 맞지 않고, 바로 끝나는 프로세스라 검사할 것도 적다.
# start_fail 인자... → 출력은 START_OUT, 종료 코드는 START_RC 에
start_fail() {
    START_OUT=$(timeout 5 "$E2E_BIN" "$@" -r "$ROOT" -c "$CGI" 2>&1)
    START_RC=$?
}

# 이미 주 서버(S)가 쓰고 있는 포트로 하나 더 띄운다 → bind 가 EADDRINUSE 로 실패해야 한다
start_fail -p "$S_PORT"
check "포트 충돌 → 종료 코드 1" 1 "$START_RC"
check_has "포트 충돌: bind 실패 메시지" "bind 실패 (포트 $S_PORT)" "$START_OUT"
check_has "포트 충돌: 원인 (Address already in use)" "Address already in use" "$START_OUT"
check "포트 충돌 뒤에도 원래 서버는 정상" 200 "$(status "$U/")"

# 잘못된 포트 값: 숫자 아님, 범위 밖(1~65535), 뒤에 글자가 붙음, 빈 값
for p in abc 0 65536 99999 -1 80x ""; do
    start_fail -p "$p"
    check "-p '$p' → 종료 코드 1" 1 "$START_RC"
    check_has "-p '$p' → '잘못된 포트 번호' 메시지" "잘못된 포트 번호" "$START_OUT"
    check "-p '$p' → 서버가 시작되지 않음" "" "$(printf '%s' "$START_OUT" | grep '서버 시작')"
done

# -p 뒤에 값이 아예 없음 → getopt 가 거절, 사용법 출력
START_OUT=$(timeout 5 "$E2E_BIN" -p 2>&1)
START_RC=$?
check "-p 값 없음 → 종료 코드 1" 1 "$START_RC"
check_has "-p 값 없음 → 사용법 출력" "사용법:" "$START_OUT"

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

# ================================================================== 갑작스러운 연결 종료
section "갑작스러운 연결 종료"
# 클라이언트 흉내: RST(강제 종료)로 끊기 등. curl 로는 만들기 어려워서 작은 python 도우미를 쓴다
cat > "$WORK/abrupt.py" <<'PYEOF'
import socket, struct, sys, time
port, mode = int(sys.argv[1]), sys.argv[2]
s = socket.create_connection(("127.0.0.1", port))
def rst():  # SO_LINGER(켜기, 0초) 로 닫으면 FIN 대신 RST: "강제로 끊음"
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0)); s.close()
if mode == "header":      # 헤더를 보내다 RST
    s.sendall(b"GET / HTTP/1.1\r\nHo"); time.sleep(0.2); rst()
elif mode == "body":      # 본문 업로드 도중 RST
    s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 100000\r\n\r\n" + b"x" * 1000); time.sleep(0.2); rst()
elif mode == "response":  # 3MB 응답을 조금만 받고 RST
    s.sendall(b"GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n"); s.recv(10000); rst()
elif mode == "bigbody":   # Expect 없이 2MB 를 끝까지 보냄 → 413 을 온전히 받아야 함
    s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\n\r\n")
    try:
        s.sendall(b"x" * 2000000)
    except OSError:
        pass  # 서버가 거절 후 닫았으면 보내기가 중간에 실패할 수 있다
    s.settimeout(5)
    try:
        print(s.recv(100).split(b"\r\n")[0].decode())
    except OSError as e:
        print("recv 실패:", type(e).__name__)
PYEOF
timeout 10 python3 -I "$WORK/abrupt.py" "$S_PORT" header
check "헤더 도중 RST → 서버 정상" 200 "$(status "$U/")"
timeout 10 python3 -I "$WORK/abrupt.py" "$S_PORT" body
check "본문 업로드 도중 RST → 서버 정상" 200 "$(status "$U/")"
timeout 10 python3 -I "$WORK/abrupt.py" "$S_PORT" response
sleep 0.3
check "3MB 응답 받는 도중 RST → 서버 정상" 200 "$(status "$U/")"
check_has "그 연결은 '응답이 중간에 끊김' 으로 정리" "응답이 중간에 끊김" "$(cat "$S_LOG")"
check "Expect 없이 2MB 를 다 보내도 413 을 온전히 받음" "HTTP/1.1 413 Content Too Large" "$(timeout 15 python3 -I "$WORK/abrupt.py" "$S_PORT" bigbody)"
# CGI 가 도는 동안 클라이언트가 포기하고 떠남 → CGI 는 끝까지 회수, 좀비 없음
$C --max-time 0.5 -o /dev/null "$U/cgi-bin/slow.sh"
check "CGI 실행 중 클라이언트가 떠남 (curl 시간 초과 = 28)" 28 "$?"
sleep 2.5
check "그 CGI 도 회수됨 (자식 0개)" 0 "$(ps --ppid "$S_PID" --no-headers | wc -l)"
check "그 뒤에도 서버 정상" 200 "$(status "$U/")"

# ================================================================== 빈 연결
section "빈 연결 (첫 요청 전)"
start_server E -t 1 -T 2 || exit 1
UE=http://127.0.0.1:$E_PORT
E_BASE=$(ls "/proc/$E_PID/fd" 2>/dev/null | wc -l)
# 빈 연결 5개를 3초 동안 열어 둔다 (아무것도 보내지 않음). 끝나면 서버가 닫았는지(EOF) 개수를 출력
timeout 10 python3 -I -c '
import socket, sys, time
port = int(sys.argv[1])
socks = [socket.create_connection(("127.0.0.1", port)) for _ in range(5)]
time.sleep(3)
closed = 0
for s in socks:
    s.settimeout(1)
    try:
        closed += (s.recv(1) == b"")
    except OSError:
        pass
print(closed)
' "$E_PORT" > "$WORK/empty.out" &
EP=$!
sleep 0.5
T=$($C -o /dev/null -w '%{http_code} %{time_total}' "$UE/")
check "일꾼 1개 + 빈 연결 5개 → 새 요청 200" 200 "${T% *}"
check_time "그 새 요청이 0.5초 안에 (ms)" 500 "$(awk -v t="${T#* }" 'BEGIN{printf "%d", t*1000}')"
check "연결 후 1초 쉬고 보낸 요청 → 200" "HTTP/1.1 200 OK" "$( (sleep 1; printf 'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n') | timeout 10 nc -q0 127.0.0.1 "$E_PORT" | first_line)"
wait "$EP"
check "빈 연결 5개는 유휴 2초 뒤 응답 없이 닫힘" 5 "$(cat "$WORK/empty.out")"
if [ -z "$E2E_WRAP" ]; then
    check "빈 연결이 정리된 뒤 fd 수가 시작 때와 같음" "$E_BASE" "$(ls "/proc/$E_PID/fd" | wc -l)"
fi
check "빈 연결에는 408 을 보내지 않음 (로그에 408 없음)" 0 "$(grep -c '408' "$E_LOG")"
stop_server "$E_PID"

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

# ================================================================== fd 고갈
section "fd 고갈 (ulimit -n 64)"
if [ -n "$E2E_WRAP" ]; then
    echo "  (건너뜀: valgrind 는 자기 fd 가 필요해서 한도를 낮춘 실행과 맞지 않음)"
else
    NOFILE=64 start_server L || exit 1
    UL=http://127.0.0.1:$L_PORT
    # 아무것도 보내지 않는 연결 100개 → 서버의 fd(64개)가 바닥난다
    timeout 20 python3 -I -c '
import socket, sys, time
socks = []
for i in range(100):
    try:
        socks.append(socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=2))
    except OSError:
        break
open(sys.argv[2], "w").write(str(len(socks)))
time.sleep(3)
' "$L_PORT" "$WORK/fdcount" &
    FDP=$!
    sleep 1.5
    check "fd 가 바닥난 동안에도 서버 프로세스는 살아 있음" yes "$(kill -0 "$L_PID" 2>/dev/null && echo yes)"
    wait "$FDP"
    check "연결 100개를 모두 맺음 (커널 대기열에서 기다림)" 100 "$(cat "$WORK/fdcount")"
    sleep 0.5
    check "연결이 닫힌 뒤 다시 정상 처리" 200 "$(status "$UL/")"
    check_has "로그에 fd 고갈이 기록됨" "Too many open files" "$(cat "$L_LOG")"
    check_lt "고갈 로그는 1초에 한 번 이하로 제한 (줄 수)" 8 "$(grep -c 'accept 실패' "$L_LOG")"
    stop_server "$L_PID"
    check "fd 고갈을 겪은 서버도 정상 종료 (종료 코드 0)" 0 "$STOP_RC"
fi

# ================================================================== 마무리 점검
section "마무리 점검 (주 서버)"
check "서버가 살아 있음" 200 "$(status "$U/")"
check "자식 프로세스(좀비 포함) 없음" 0 "$(ps --ppid "$S_PID" --no-headers | wc -l)"
if [ -z "$E2E_WRAP" ]; then
    sleep 2.5 # keep-alive 로 쉬던 연결이 정리될 시간 (유휴 2초 + 여유)
    check "열린 fd 수가 시작 때와 같음" "$BASE_FDS" "$(ls "/proc/$S_PID/fd" | wc -l)"
fi
stop_server "$S_PID"
check "SIGINT(Ctrl+C) → 정상 종료 (종료 코드 0)" 0 "$STOP_RC"
check_has "종료 요약이 로그에 남음" "[main] 종료 완료" "$(cat "$S_LOG")"

# ================================================================== 안전한 종료
section "안전한 종료 (SIGINT/SIGTERM)"
# ① 처리 중인 요청은 끝까지, 응답은 Connection: close
start_server F || exit 1
UF=http://127.0.0.1:$F_PORT
$C -i "$UF/cgi-bin/slow.sh" > "$WORK/inflight.out" &
CP=$!
sleep 0.5
t0=$(date +%s%N)
kill -TERM "$F_PID"
sleep 0.3
check "종료 시작 뒤 새 연결은 거절됨" 000 "$(curl -s --max-time 3 -o /dev/null -w '%{http_code}' "$UF/")"
wait "$CP"
check "처리 중이던 요청(2초 CGI)은 200 으로 끝남" "HTTP/1.1 200 OK" "$(head -1 "$WORK/inflight.out" | tr -d '\r')"
check_has "그 응답은 Connection: close" "Connection: close" "$(tr -d '\r' < "$WORK/inflight.out")"
stop_server "$F_PID" TERM
check "SIGTERM → 정상 종료 (종료 코드 0)" 0 "$STOP_RC"
check_time "처리 중 요청을 기다려 약 1.5초 뒤 종료 (ms)" 3000 $(( ($(date +%s%N) - t0) / 1000000 ))
check_has "로그: 처리 중 1개 마무리" "처리 중 1개" "$(cat "$F_LOG")"

# ② 쉬는 keep-alive 연결은 기다리지 않고 바로 닫힘
start_server G || exit 1
timeout 10 python3 -I -c '
import socket, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
time.sleep(0.3); s.recv(65536)        # 응답을 받고 쉬는 연결이 됨
open(sys.argv[2], "w").write("ready")
s.settimeout(5); t0 = time.time()
print("closed" if s.recv(1) == b"" else "data", round((time.time() - t0) * 1000))
' "$G_PORT" "$WORK/ka_ready" > "$WORK/ka.out" &
KP=$!
for i in $(seq 50); do [ -f "$WORK/ka_ready" ] && break; sleep 0.1; done
stop_server "$G_PID"
wait "$KP"
check "쉬던 keep-alive 연결이 종료 때 닫힘" closed "$(awk '{print $1}' "$WORK/ka.out")"
check_time "그 연결은 1초 안에 닫힘 (ms)" 1000 "$(awk '{print $2}' "$WORK/ka.out")"
check "정상 종료 (종료 코드 0)" 0 "$STOP_RC"

# ③ 두 번째 신호 → 기다리지 않고 즉시 종료, 실행 중인 CGI 도 정리
if [ -z "$E2E_WRAP" ]; then
    start_server H || exit 1
    $C -o /dev/null "http://127.0.0.1:$H_PORT/cgi-bin/forever.sh" &
    FP=$!
    sleep 0.5
    CGI_PID=$(ps -o pid= --ppid "$H_PID" | head -1 | tr -d ' ')
    t0=$(date +%s%N)
    kill -INT "$H_PID"; sleep 0.2; kill -INT "$H_PID"
    stop_server "$H_PID" 0 # 신호 0 = 아무것도 보내지 않고 끝나기만 기다림 (종료 코드를 거둔다)
    check "두 번째 SIGINT → 즉시 종료 (종료 코드 130)" 130 "$STOP_RC"
    check_time "즉시 종료 (ms)" 1500 $(( ($(date +%s%N) - t0) / 1000000 ))
    sleep 0.2
    check "실행 중이던 CGI(forever.sh) 도 정리됨" gone "$([ -n "$CGI_PID" ] && ! kill -0 "$CGI_PID" 2>/dev/null && echo gone)"
    wait "$FP" 2>/dev/null
fi

# ④ 대기열에서 기다리던 연결도 종료 전에 처리됨 (일꾼 1개: CGI 가 일꾼을 잡은 동안 두 번째 요청은 대기열에)
start_server Q -t 1 || exit 1
UQ=http://127.0.0.1:$Q_PORT
$C -o "$WORK/q_inflight.out" "$UQ/cgi-bin/slow.sh" &
QP1=$!
sleep 0.3
$C -i "$UQ/hello.txt" > "$WORK/q_queued.out" &
QP2=$!
sleep 0.3
stop_server "$Q_PID" TERM
wait "$QP1" "$QP2"
check_has "처리 중이던 CGI 는 끝까지 응답" "slow done" "$(cat "$WORK/q_inflight.out")"
check "대기열에 있던 연결도 200 으로 응답" "HTTP/1.1 200 OK" "$(head -1 "$WORK/q_queued.out" | tr -d '\r')"
check_has "그 응답도 Connection: close" "Connection: close" "$(tr -d '\r' < "$WORK/q_queued.out")"
check "대기열까지 비운 뒤 정상 종료 (종료 코드 0)" 0 "$STOP_RC"

# ⑤ 강제 시한: 헤더를 아주 천천히 보내는 클라이언트가 일꾼을 붙잡아도 -S 초 뒤에는 끝난다
if [ -z "$E2E_WRAP" ]; then
    start_server D -S 2 || exit 1
    timeout 15 python3 -I -c '
import socket, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nX-Slow: ")
for _ in range(20):                       # 0.5초마다 1바이트: 유휴(10초)/요청(30초) 시간 제한에 안 걸림
    time.sleep(0.5)
    try:
        s.sendall(b"a")
    except OSError:
        break
' "$D_PORT" &
    DP=$!
    sleep 0.5
    t0=$(date +%s%N)
    stop_server "$D_PID" TERM
    took=$(( ($(date +%s%N) - t0) / 1000000 ))
    check "시한 초과 → 강제 종료 (종료 코드 1)" 1 "$STOP_RC"
    check_time "시한(2초) 직후에 끝남 (ms)" 3500 "$took"
    check_lt "시한보다 일찍 끝나지는 않음 (ms)" "$took" 1500
    check_has "로그: 종료 시한 초과" "종료 시한 초과" "$(cat "$D_LOG")"
    kill "$DP" 2>/dev/null; wait "$DP" 2>/dev/null

fi

# ⑥ CGI 자식은 서버가 막아 둔 시그널 마스크를 물려받지 않는다 (막힌 채로 exec 하면 kill -TERM 이 안 먹는다)
start_server M || exit 1
SIGBLK=$($C "http://127.0.0.1:$M_PORT/cgi-bin/sigmask.sh" | awk '/SigBlk/{print $2}')
check "CGI 의 막힌 시그널 없음 (SigBlk)" 0000000000000000 "$SIGBLK"
stop_server "$M_PID"

# ================================================================== 리버스 프록시 (F15)
section "리버스 프록시"
# 백엔드 2개: 같은 서버 프로그램을 루트만 바꿔서 띄운다. 어느 쪽이 답했는지 who.txt 로 구별
for b in b1 b2; do
    mkdir -p "$WORK/$b/api"
    echo "$b" > "$WORK/$b/api/who.txt"
    cp "$ROOT/big.bin" "$WORK/$b/api/big.bin"
done
start_server B1 -r "$WORK/b1" || exit 1
start_server B2 -r "$WORK/b2" || exit 1
start_server P -P "/api=127.0.0.1:$B1_PORT,127.0.0.1:$B2_PORT" || exit 1
UP=http://127.0.0.1:$P_PORT

# ① 라운드로빈: 번갈아 답한다 (시작 확인 요청은 "/" 라서 프록시를 거치지 않음 → 번호표 0부터)
RR=$(for i in 1 2 3 4; do $C "$UP/api/who.txt"; done | tr '\n' ' ')
check "라운드로빈: b1 b2 b1 b2 순서" "b1 b2 b1 b2 " "$RR"
check_has "접근 로그: 어느 백엔드가 처리했는지" "프록시 → 127.0.0.1:$B2_PORT" "$(cat "$P_LOG")"

# ② prefix 밖은 프록시 자신이 처리
check "prefix 밖(/hello.txt)은 자기 파일" same "$($C "$UP/hello.txt" | cmp -s - "$ROOT/hello.txt" && echo same)"
check "이름만 비슷한 /apix 는 프록시 안 함 → 404" 404 "$(status "$UP/apix")"

# ③ 응답 전달: 헤더 중복 없음, HEAD, 큰 파일, 백엔드의 에러 상태 그대로
PH=$($C -i "$UP/api/who.txt" | tr -d '\r')
check "Server 헤더는 하나만" 1 "$(printf '%s\n' "$PH" | grep -c '^Server:')"
check "Content-Length 헤더는 하나만" 1 "$(printf '%s\n' "$PH" | grep -c '^Content-Length:')"
check_has "백엔드의 Last-Modified 는 전달" "Last-Modified:" "$PH"
check "HEAD: Content-Length 는 GET 과 같음" 3 "$($C -I "$UP/api/who.txt" | tr -d '\r' | awk -F': ' '/^Content-Length/{print $2}')"
check "HEAD: 본문 없음" 0 "$($C -I -o /dev/null -w '%{size_download}' "$UP/api/who.txt")"
check "3MB 파일 내용이 같음 (프록시 경유)" same "$($C "$UP/api/big.bin" | cmp -s - "$ROOT/big.bin" && echo same)"
check "백엔드의 404 는 그대로 404" 404 "$(status "$UP/api/nope.txt")"
# 두 번째 요청의 num_connects 가 0 = 새로 연결하지 않고 앞 연결을 재사용
check "프록시 경유 keep-alive: 두 번째 요청은 새 연결 없음" 0 \
    "$($C -o /dev/null -o /dev/null -w '%{num_connects}\n' "$UP/api/who.txt" "$UP/api/who.txt" | tail -1)"

# ④ 모든 요청을 프록시("/"): POST 본문, X-Forwarded-For, hop-by-hop 헤더 제거를 CGI 로 확인
start_server PA -P "/=127.0.0.1:$B1_PORT" || exit 1
UPA=http://127.0.0.1:$PA_PORT
check "POST 1MB 본문이 그대로 전달" same \
    "$($C --data-binary @"$WORK/body1m.bin" "$UPA/cgi-bin/echo_post.sh" | cmp -s - "$WORK/body1m.bin" && echo same)"
PENV=$($C -H 'Connection: keep-alive, X-Hop' -H 'X-Hop: secret' -H 'X-Keep: yes' "$UPA/cgi-bin/env.sh")
check_has "백엔드가 X-Forwarded-For 를 받음" "HTTP_X_FORWARDED_FOR=127.0.0.1" "$PENV"
check_has "일반 헤더는 전달" "HTTP_X_KEEP=yes" "$PENV"
check "Connection 에 적힌 헤더(X-Hop)는 전달 안 함" "" "$(printf '%s\n' "$PENV" | grep '^HTTP_X_HOP=')"
check_has "백엔드에는 HTTP/1.0 + Connection: close 로" "SERVER_PROTOCOL=HTTP/1.0" "$PENV"

# ⑤ 보낸 뒤 끊김: GET 은 다음 백엔드로 재시도, POST 는 재시도 안 함 (멱등성)
CL_PORT=$(free_port)
python3 -I "$REPO/tests/fake_backend.py" close "$CL_PORT" "$WORK/closer.log" &
SRV_PIDS+=("$!")
for ((i = 0; i < 50; i++)); do [ -f "$WORK/closer.log" ] && break; sleep 0.1; done
start_server PC -P "/=127.0.0.1:$CL_PORT,127.0.0.1:$B2_PORT" || exit 1
UPC=http://127.0.0.1:$PC_PORT
# 백엔드가 둘이라 연속 두 요청 중 하나는 반드시 끊는 백엔드가 먼저 걸린다 (어느 쪽인지는 번호표에 따라)
G1=$(status "$UPC/api/who.txt")
G2=$(status "$UPC/api/who.txt")
check "GET 2번: 끊는 백엔드에 걸려도 다음 백엔드로 재시도해서 둘 다 200" "200 200" "$G1 $G2"
check_has "로그: 끊김 → 다음 백엔드로 재시도" "응답 없이 연결이 끊김 → 다음 백엔드로 재시도" "$(cat "$PC_LOG")"
P1=$(status -X POST --data 'order=1' "$UPC/cgi-bin/echo_post.sh")
P2=$(status -X POST --data 'order=2' "$UPC/cgi-bin/echo_post.sh")
check "POST 2번: 끊는 백엔드에 걸린 쪽은 재시도 없이 502" "200 502" "$(printf '%s\n' "$P1" "$P2" | sort | tr '\n' ' ' | sed 's/ $//')"
check "끊는 백엔드가 받은 POST 는 1번" 1 "$(grep -c '^POST' "$WORK/closer.log")"
check_has "로그: 멱등 아님 → 재시도 안 함" "멱등 아님 → 재시도 안 함" "$(cat "$PC_LOG")"

# ⑥ 응답 시간 초과 → 504 (재시도 없이, -W 초 뒤)
HG_PORT=$(free_port)
python3 -I "$REPO/tests/fake_backend.py" hang "$HG_PORT" "$WORK/hang.log" &
SRV_PIDS+=("$!")
for ((i = 0; i < 50; i++)); do [ -f "$WORK/hang.log" ] && break; sleep 0.1; done
start_server PH -W 1 -P "/api=127.0.0.1:$HG_PORT,127.0.0.1:$B1_PORT" || exit 1
t0=$(date +%s%N)
check "답 없는 백엔드 → 504" 504 "$(status "http://127.0.0.1:$PH_PORT/api/who.txt")"
took=$(( ($(date +%s%N) - t0) / 1000000 ))
check_time "-W 1 초 뒤에 504 (재시도로 2배가 되지 않음) (ms)" 1900 "$took"
check_lt "시한보다 일찍 끝나지는 않음 (ms)" "$took" 900

# ⑦ 프록시 안전 종료: 백엔드에서 처리 중인 요청(2초 CGI)은 끝까지 전달한 뒤 끝난다
$C -o "$WORK/p_inflight.out" "$UPA/cgi-bin/slow.sh" &
PP=$!
sleep 0.5
stop_server "$PA_PID" TERM
wait "$PP"
check_has "종료 중에도 처리 중이던 프록시 요청은 끝까지 응답" "slow done" "$(cat "$WORK/p_inflight.out")"
check "프록시 정상 종료 (종료 코드 0)" 0 "$STOP_RC"

# ⑧ 백엔드 장애: 하나 죽이면 살아 있는 쪽으로, 둘 다 죽으면 502
stop_server "$B2_PID"
FO=$(for i in 1 2 3 4; do $C "$UP/api/who.txt"; done | tr '\n' ' ')
check "b2 를 끄면 모든 요청을 b1 이 처리" "b1 b1 b1 b1 " "$FO"
check_has "로그: 연결 실패 → 재시도" "다음 백엔드로 재시도" "$(cat "$P_LOG")"
stop_server "$B1_PID"
check "백엔드가 모두 꺼지면 502" 502 "$(status "$UP/api/who.txt")"
check "그래도 prefix 밖은 계속 200" 200 "$(status "$UP/hello.txt")"

# ⑨ 잘못된 -P 는 시작하지 않는다
check "백엔드가 자기 자신이면 시작 거부 (종료 코드 1)" 1 \
    "$("$E2E_BIN" -p 18999 -r "$ROOT" -P /api=127.0.0.1:18999 > /dev/null 2>&1; echo $?)"
check "형식이 틀린 -P 는 시작 거부" 1 "$("$E2E_BIN" -p 18998 -r "$ROOT" -P api > /dev/null 2>&1; echo $?)"
check_has "거부 이유를 알려 줌" "이 서버 자신" "$("$E2E_BIN" -p 18999 -r "$ROOT" -P /api=127.0.0.1:18999 2>&1)"

# ================================================================== 메모리 검사 집계
section "메모리 검사"
# (1) ASan/LSan/UBSan/TSan 보고: 어떤 빌드든 서버 로그에 나오면 안 된다
SAN=$(cat "$WORK"/*.log 2>/dev/null | grep -aE 'ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:|WARNING: ThreadSanitizer' | head -3)
check "서버 로그에 sanitizer 보고 없음" "" "$SAN"

# (2) valgrind: 서버 프로세스마다 집계 ("ERROR SUMMARY" 가 있는 로그 = 끝까지 실행된 서버.
#     CGI 를 위해 fork 한 자식은 exec 순간 valgrind 를 벗어나므로 요약이 없다)
if [ "$E2E_VALGRIND" = 1 ]; then
    num() { grep -aE "$1" "$2" | tail -1 | sed -E 's/.*: *([0-9,]+) bytes.*/\1/; s/.*ERROR SUMMARY: ([0-9,]+) errors.*/\1/' | tr -d ','; }
    printf '  %-10s %8s %12s %12s %12s %12s %10s\n' pid 에러 definitely indirectly possibly reachable "fd열림/상속"
    VG_N=0
    for f in "$WORK"/vg/*.log; do
        grep -q "ERROR SUMMARY" "$f" || continue
        VG_N=$((VG_N + 1))
        pid=$(basename "$f" .log)
        e=$(num 'ERROR SUMMARY' "$f")
        d=$(num 'definitely lost:' "$f"); d=${d:-0}
        i=$(num 'indirectly lost:' "$f"); i=${i:-0}
        p=$(num 'possibly lost:' "$f"); p=${p:-0}
        r=$(num 'still reachable:' "$f"); r=${r:-0}
        # 예: "FILE DESCRIPTORS: 3 open (3 inherited) at exit."  (옛 버전은 "(3 std)")
        #     → "열린 수/물려받은 수". 물려받은 3개(표준 입출력)만 열려 있어야 정상
        fds=$(grep -aE 'FILE DESCRIPTORS: [0-9]+ open' "$f" | tail -1 |
              sed -E 's/.*FILE DESCRIPTORS: ([0-9]+) open \(([0-9]+) (inherited|std)\).*/\1\/\2/')
        printf '  %-10s %8s %12s %12s %12s %12s %10s\n' "$pid" "$e" "$d" "$i" "$p" "$r" "$fds"
        check "valgrind[$pid] 메모리 에러 0" 0 "$e"
        check "valgrind[$pid] definitely lost 0" 0 "$d"
        check "valgrind[$pid] indirectly lost 0" 0 "$i"
        # 즉시 종료(_exit) 같은 경우가 아니면, 표준 입출력 3개 외에 열린 fd 가 없어야 한다
        check "valgrind[$pid] 종료 때 열린 fd = 물려받은 표준 입출력 3개뿐" "3/3" "$fds"
    done
    if [ "$VG_N" -ge 1 ]; then ok "valgrind 로 끝까지 실행된 서버 ${VG_N}개 집계"; else ng "valgrind 요약을 찾지 못함"; fi
    rm -rf "$REPO/build/valgrind-logs"
    mkdir -p "$REPO/build/valgrind-logs" && cp "$WORK"/vg/*.log "$REPO/build/valgrind-logs/" 2>/dev/null
    echo "  (전체 valgrind 로그: build/valgrind-logs/)"
fi

# ------------------------------------------------------------------ 결과
printf '\n== e2e: %d개 중 %d개 실패\n' $((PASS + FAIL)) "$FAIL"
if [ "$FAIL" -gt 0 ]; then
    printf '   실패: %s\n' "${FAILED_NAMES[@]}"
    echo "   서버 로그: (아래 마지막 20줄)"
    tail -20 "$S_LOG"
    exit 1
fi
exit 0
