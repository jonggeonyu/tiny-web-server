#!/usr/bin/env bash
# graceful shutdown 기능 테스트 (N4)
source "$(dirname "$0")/lib.sh"
PORT=18022

# 2초 걸리는 CGI
cat >"$WWW/cgi-bin/nap2" <<'CGI'
#!/bin/sh
sleep 2
printf 'Content-Type: text/plain\r\n\r\nnap2 done\n'
CGI
chmod 755 "$WWW/cgi-bin/nap2"

now() { date +%s.%N; }
elapsed() { echo "$(now) - $1" | bc; }
is_between() { [ "$(echo "$1 >= $2 && $1 < $3" | bc)" = 1 ]; }

# stop_and_time <시그널> : 서버에 시그널을 보내고 끝날 때까지 기다린다.
# 종료 코드는 $TMP/exit_code, 걸린 시간은 $TMP/stop_time
stop_and_time() {
    local s
    s=$(now)
    kill "-$1" "$SERVER_PID"
    wait "$SERVER_PID"
    echo $? >"$TMP/exit_code"
    elapsed "$s" >"$TMP/stop_time"
    SERVER_PID=""
}

# --- 처리 중인 요청은 끝까지 응답하고, 새 연결은 거부하고, 0으로 끝남 ---
SERVER_OPTIONS="-t 1" start_server "$PORT" || { fail "server_starts"; exit 1; }
curl_local -D "$TMP/inflight_head" -o "$TMP/inflight_body" \
    "http://127.0.0.1:$PORT/cgi-bin/nap2" &
INFLIGHT=$!
sleep 0.3
curl_local -D "$TMP/queued_head" -o "$TMP/queued_body" \
    "http://127.0.0.1:$PORT/split.txt" &     # 일꾼 1개라 큐에서 기다림
QUEUED=$!
sleep 0.3
# 종료 시작 0.3초 뒤 새 연결: listen 소켓이 닫혀 연결 자체가 거부된다 (curl 7)
( sleep 0.3; curl_local -o /dev/null "http://127.0.0.1:$PORT/"; echo $? >"$TMP/refused" ) &
LATE=$!
stop_and_time TERM
wait "$INFLIGHT" "$QUEUED" "$LATE"
refused=$(cat "$TMP/refused")

grep -q "nap2 done" "$TMP/inflight_body" \
    && pass "n4_in_flight_request_finishes" || fail "n4_in_flight_request_finishes"
grep -q "split ok" "$TMP/queued_body" \
    && pass "n4_queued_connection_served" || fail "n4_queued_connection_served"
grep -qi "^Connection: close" "$TMP/queued_head" \
    && pass "n4_no_keep_alive_while_stopping" || fail "n4_no_keep_alive_while_stopping"
[ "$refused" = 7 ] && pass "n4_new_connection_refused" \
    || fail "n4_new_connection_refused" "curl exit $refused"
code=$(cat "$TMP/exit_code"); took=$(cat "$TMP/stop_time")
if [ "$code" = 0 ] && is_between "$took" 1 5; then
    pass "n4_exits_0_after_finishing (${took}s)"
else
    fail "n4_exits_0_after_finishing" "exit $code, ${took}s"
fi
grep -q "info: shutdown complete" "$TMP/server_$PORT.log" \
    && pass "n4_logs_shutdown" || fail "n4_logs_shutdown"

# --- 할 일이 없으면 Ctrl+C(SIGINT)로 바로 끝남 ---
start_server "$PORT" || { fail "server_restarts"; exit 1; }
stop_and_time INT
code=$(cat "$TMP/exit_code"); took=$(cat "$TMP/stop_time")
if [ "$code" = 0 ] && is_between "$took" 0 1; then
    pass "n4_idle_server_stops_on_sigint (${took}s)"
else
    fail "n4_idle_server_stops_on_sigint" "exit $code, ${took}s"
fi

# --- 쉬는 keep-alive 연결이 있어도 유휴 시간(5초) 안에 끝남 ---
start_server "$PORT" || { fail "server_restarts"; exit 1; }
exec 3<>"/dev/tcp/127.0.0.1/$PORT"
printf 'GET /split.txt HTTP/1.1\r\nHost: x\r\n\r\n' >&3
sleep 0.3
stop_and_time TERM
exec 3>&-
code=$(cat "$TMP/exit_code"); took=$(cat "$TMP/stop_time")
if [ "$code" = 0 ] && is_between "$took" 0 6; then
    pass "n4_idle_keep_alive_does_not_block_shutdown (${took}s)"
else
    fail "n4_idle_keep_alive_does_not_block_shutdown" "exit $code, ${took}s"
fi

# --- CGI 프로그램은 서버가 막아 둔 시그널을 물려받지 않는다 ---
cat >"$WWW/cgi-bin/sigmask" <<'CGI'
#!/bin/sh
printf 'Content-Type: text/plain\r\n\r\n'
grep -E '^Sig(Blk|Ign):' /proc/$$/status
CGI
chmod 755 "$WWW/cgi-bin/sigmask"
start_server "$PORT" || { fail "server_restarts"; exit 1; }
masks=$(curl_local "http://127.0.0.1:$PORT/cgi-bin/sigmask")
blocked=$(printf '%s' "$masks" | awk '/SigBlk/{print $2}')
[ "$blocked" = "0000000000000000" ] && pass "n4_cgi_gets_default_signal_mask" \
    || fail "n4_cgi_gets_default_signal_mask" "SigBlk $blocked"
stop_server

echo "e2e shutdown: $E2E_PASS passed, $E2E_FAIL failed"
[ "$E2E_FAIL" = 0 ]
