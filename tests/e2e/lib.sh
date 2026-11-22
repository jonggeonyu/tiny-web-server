# 기능 테스트 공용 도우미. 각 e2e 스크립트가 source 한다.
# 테스트 포트는 18000~18999만 쓴다.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SERVER="$ROOT/bin/webserver"
TMP="$ROOT/tests/tmp"
mkdir -p "$TMP"

E2E_PASS=0
E2E_FAIL=0
SERVER_PID=""

pass() { E2E_PASS=$((E2E_PASS + 1)); echo "PASS $1"; }
fail() { E2E_FAIL=$((E2E_FAIL + 1)); echo "FAIL $1${2:+: $2}"; }

# start_server <port> : 백그라운드로 띄우고 listen 할 때까지 기다린다
start_server() {
    # 지난 실행의 로그가 남아 있으면 "listening"을 미리 찾아버려서, 서버가
    # 뜨기 전에 테스트가 시작된다. 그래서 로그를 먼저 지운다
    rm -f "$TMP/server_$1.log"
    "$SERVER" "$1" >"$TMP/server_$1.log" 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 1 50); do
        if grep -q "listening" "$TMP/server_$1.log" 2>/dev/null; then
            return 0
        fi
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            return 1
        fi
        sleep 0.05
    done
    return 1
}

stop_server() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID"
        wait "$SERVER_PID" 2>/dev/null
    fi
    SERVER_PID=""
}

is_server_alive() { kill -0 "$SERVER_PID" 2>/dev/null; }

# 열린 fd 수 (0,1,2와 listen 소켓 포함)
count_fds() { ls "/proc/$SERVER_PID/fd" 2>/dev/null | wc -l; }

# wait_fds <기대 개수> : 서버가 마지막 연결을 닫을 때까지 최대 2초 기다린 뒤 개수 출력
wait_fds() {
    local count
    for _ in $(seq 1 20); do
        count=$(count_fds)
        [ "$count" = "$1" ] && break
        sleep 0.1
    done
    echo "$count"
}

trap stop_server EXIT

# raw_request <port> <printf 형식 문자열> : nc로 바이트를 그대로 보내고 응답 전체를 출력
raw_request() {
    printf "$2" | timeout 3 nc -N 127.0.0.1 "$1"
}

# status_of <응답> : 첫 줄의 상태 코드
status_of() { printf '%s' "$1" | head -1 | awk '{print $2}'; }

# expect_status <이름> <기대 코드> <실제 코드>
expect_status() {
    if [ "$3" = "$2" ]; then pass "$1"; else fail "$1" "expected $2, got '$3'"; fi
}

# 로컬 서버용 curl. 환경에 http_proxy가 있으면 127.0.0.1 요청도 프록시로 가서 막히므로 끈다
curl_local() { timeout 5 curl --noproxy '*' -s "$@"; }
