#!/usr/bin/env bash
# 동시 처리·과부하·시간 제한·keep-alive 기능 테스트 (N1, N2, N3, F11)
source "$(dirname "$0")/lib.sh"
PORT=18020          # 일꾼 4개 (기본 테스트)
BUSY_PORT=18021     # 일꾼 1개 + 큐 1칸 (과부하 테스트)

# 1초 걸리는 CGI
cat >"$WWW/cgi-bin/nap" <<'CGI'
#!/bin/sh
sleep 1
printf 'Content-Type: text/plain\r\n\r\nnap done\n'
CGI
chmod 755 "$WWW/cgi-bin/nap"

now() { date +%s.%N; }
elapsed() { echo "$(now) - $1" | bc; }
# is_between <값> <최소> <최대>
is_between() { [ "$(echo "$1 >= $2 && $1 < $3" | bc)" = 1 ]; }
count_accepted() { grep -c "accepted connection" "$TMP/server_$1.log"; }

# --- 인자 검사 (-t, -q) ---
for bad in "-t 0" "-t 65" "-t x" "-q 0" "-q 5000"; do
    # shellcheck disable=SC2086
    if "$SERVER" $bad "$PORT" "$WWW" >/dev/null 2>&1; then
        fail "n1_bad_option_rejected ($bad)"
    else
        pass "n1_bad_option_rejected ($bad)"
    fi
done

SERVER_OPTIONS="-t 4" start_server "$PORT" || { fail "server_starts"; exit 1; }
grep -q "workers 4, queue 256" "$TMP/server_$PORT.log" \
    && pass "n1_options_applied" || fail "n1_options_applied"
FDS_BEFORE=$(count_fds)

# --- N1: 아무것도 안 보내는 연결이 있어도 다른 요청은 바로 처리 ---
sleep 3 | nc 127.0.0.1 "$PORT" >/dev/null &
IDLE_CLIENT=$!
sleep 0.2
start=$(now)
status=$(curl_local -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/")
took=$(elapsed "$start")
if [ "$status" = 200 ] && is_between "$took" 0 1; then
    pass "n1_idle_client_does_not_block_others (${took}s)"
else
    fail "n1_idle_client_does_not_block_others" "status $status, ${took}s"
fi
kill "$IDLE_CLIENT" 2>/dev/null; wait "$IDLE_CLIENT" 2>/dev/null

# --- N1: 1초 걸리는 CGI 4개를 동시에 → 4초가 아니라 약 1초 ---
# (그냥 wait하면 백그라운드로 띄운 서버까지 기다리므로 pid를 모아 기다린다)
rm -f "$TMP"/nap_*
start=$(now)
pids=()
for i in 1 2 3 4; do
    curl_local -o "$TMP/nap_$i" "http://127.0.0.1:$PORT/cgi-bin/nap" &
    pids+=($!)
done
wait "${pids[@]}"
took=$(elapsed "$start")
done_count=$(cat "$TMP"/nap_* | grep -c "nap done")
if [ "$done_count" = 4 ] && is_between "$took" 0.9 2.5; then
    pass "n1_four_slow_requests_in_parallel (${took}s)"
else
    fail "n1_four_slow_requests_in_parallel" "done $done_count, ${took}s"
fi

# --- F11: keep-alive 응답 헤더 ---
headers=$(curl_local -D - -o /dev/null "http://127.0.0.1:$PORT/")
printf '%s' "$headers" | grep -qi "^Connection: keep-alive" \
    && pass "f11_http11_keeps_alive" || fail "f11_http11_keeps_alive"
printf '%s' "$headers" | grep -qi "^Keep-Alive: timeout=5" \
    && pass "f11_keep_alive_timeout_header" || fail "f11_keep_alive_timeout_header"
headers=$(curl_local -D - -o /dev/null -H 'Connection: close' "http://127.0.0.1:$PORT/")
printf '%s' "$headers" | grep -qi "^Connection: close" \
    && pass "f11_connection_close_honored" || fail "f11_connection_close_honored"
headers=$(curl_local -0 -D - -o /dev/null "http://127.0.0.1:$PORT/")
printf '%s' "$headers" | grep -qi "^Connection: close" \
    && pass "f11_http10_closes_by_default" || fail "f11_http10_closes_by_default"

# --- F11: 요청 3개를 연결 하나로 (curl이 연결을 재사용) ---
before=$(count_accepted "$PORT")
curl_local -o /dev/null -o /dev/null -o /dev/null \
    "http://127.0.0.1:$PORT/" "http://127.0.0.1:$PORT/split.txt" \
    "http://127.0.0.1:$PORT/cgi-bin/echo"
sleep 0.2
used=$(( $(count_accepted "$PORT") - before ))
[ "$used" = 1 ] && pass "f11_three_requests_one_connection" \
    || fail "f11_three_requests_one_connection" "$used connections"

# --- F11: 한 번에 몰아 보낸 요청 두 개(파이프라이닝)도 차례로 응답 ---
reply=$(raw_request "$PORT" 'GET /split.txt HTTP/1.1\r\nHost: x\r\n\r\nGET /split.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n')
count=$(printf '%s' "$reply" | grep -c "^HTTP/1.1 200")
[ "$count" = 2 ] && pass "f11_pipelined_requests_both_answered" \
    || fail "f11_pipelined_requests_both_answered" "$count responses"

# --- F11: 연결 하나에서 100개까지. 101번째는 새 연결 ---
urls=()
for _ in $(seq 1 101); do urls+=("http://127.0.0.1:$PORT/split.txt"); done
before=$(count_accepted "$PORT")
# 요청 101개라 ASan 빌드에서는 curl_local의 5초를 넘을 수 있어 30초로 늘린다
timeout 30 curl --noproxy '*' -s "${urls[@]}" >/dev/null
sleep 0.2
used=$(( $(count_accepted "$PORT") - before ))
[ "$used" = 2 ] && pass "f11_max_100_requests_per_connection" \
    || fail "f11_max_100_requests_per_connection" "$used connections"

# --- 시간 제한 3가지를 동시에 돌린다 (기다리는 시간을 한 번만 쓰려고) ---
# nc는 서버가 닫아도 보낼 쪽(stdin)이 열려 있으면 안 끝나서, bash의
# /dev/tcp로 연결하고 "서버가 닫을 때까지 읽기(cat)"에 걸린 시간을 잰다.
# timed_read <이름> <보내기 함수> : 보내기는 백그라운드, 읽기는 닫힐 때까지
timed_read() {
    local name=$1 sender=$2 s
    exec 3<>"/dev/tcp/127.0.0.1/$PORT"
    "$sender" >&3 2>/dev/null &
    local sender_pid=$!
    s=$(now)
    timeout 15 cat <&3 >"$TMP/${name}_reply"
    elapsed "$s" >"$TMP/${name}_time"
    kill "$sender_pid" 2>/dev/null
    exec 3>&-
}
# 1) 응답 뒤 쉬는 연결: 5초 뒤 408 없이 조용히 닫힘
send_one_then_idle() { printf 'GET /split.txt HTTP/1.1\r\nHost: x\r\n\r\n'; sleep 14; }
# 2) 연결만 하고 아무것도 안 보냄: 10초 뒤 408
send_nothing() { sleep 14; }
# 3) 헤더를 1초에 한 줄씩 끝없이 보냄 (slowloris): 요청 시작 10초 뒤 408
send_drip() {
    printf 'GET / HTTP/1.1\r\n'
    for i in $(seq 1 14); do sleep 1; printf 'X-Drip-%d: a\r\n' "$i"; done
}
pids=()
timed_read idle send_one_then_idle & pids+=($!)
timed_read silent send_nothing & pids+=($!)
timed_read drip send_drip & pids+=($!)
wait "${pids[@]}"

count=$(grep -c "^HTTP/1.1" "$TMP/idle_reply")
took=$(cat "$TMP/idle_time")
if [ "$count" = 1 ] && ! grep -q " 408 " "$TMP/idle_reply" && is_between "$took" 4.5 7; then
    pass "f11_idle_keep_alive_closed_quietly (${took}s)"
else
    fail "f11_idle_keep_alive_closed_quietly" "$count responses, ${took}s"
fi
took=$(cat "$TMP/silent_time")
if [ "$(status_of "$(cat "$TMP/silent_reply")")" = 408 ] && is_between "$took" 9.5 12; then
    pass "n3_no_request_returns_408 (${took}s)"
else
    fail "n3_no_request_returns_408" "$(head -1 "$TMP/silent_reply"), ${took}s"
fi
took=$(cat "$TMP/drip_time")
if [ "$(status_of "$(cat "$TMP/drip_reply")")" = 408 ] && is_between "$took" 9.5 12; then
    pass "n3_slow_drip_headers_returns_408 (${took}s)"
else
    fail "n3_slow_drip_headers_returns_408" "$(head -1 "$TMP/drip_reply"), ${took}s"
fi

FDS_AFTER=$(wait_fds "$FDS_BEFORE")
[ "$FDS_BEFORE" = "$FDS_AFTER" ] && pass "n7_no_fd_leak_after_pool_tests ($FDS_AFTER fds)" \
    || fail "n7_no_fd_leak_after_pool_tests" "before $FDS_BEFORE after $FDS_AFTER"
stop_server

# --- N2: 일꾼 1 + 큐 1이 다 차면 503. 50번 반복해서 매번 503이 보이는지 ---
SERVER_OPTIONS="-t 1 -q 1" start_server "$BUSY_PORT" || { fail "busy_server_starts"; exit 1; }
sleep 6 | nc 127.0.0.1 "$BUSY_PORT" >/dev/null &   # 일꾼이 잡고 있는 연결
HOLD_WORKER=$!
sleep 0.2
sleep 6 | nc 127.0.0.1 "$BUSY_PORT" >/dev/null &   # 큐에서 기다리는 연결
HOLD_QUEUE=$!
sleep 0.2
got_503=0
for _ in $(seq 1 50); do
    code=$(curl_local -o /dev/null -w '%{http_code}' "http://127.0.0.1:$BUSY_PORT/")
    [ "$code" = 503 ] && got_503=$((got_503 + 1))
done
[ "$got_503" = 50 ] && pass "n2_full_queue_returns_503 (50/50)" \
    || fail "n2_full_queue_returns_503" "$got_503/50 saw 503"
# POST 본문이 붙은 요청도 거절 응답이 보이는지
code=$(curl_local -o /dev/null -w '%{http_code}' -H 'Expect:' \
       --data-binary "a=1&b=2" "http://127.0.0.1:$BUSY_PORT/cgi-bin/echo")
expect_status "n2_full_queue_post_returns_503" 503 "$code"

kill "$HOLD_WORKER" "$HOLD_QUEUE" 2>/dev/null
wait "$HOLD_WORKER" "$HOLD_QUEUE" 2>/dev/null
sleep 0.3
status=$(curl_local -o /dev/null -w '%{http_code}' "http://127.0.0.1:$BUSY_PORT/")
expect_status "n2_serves_again_after_load" 200 "$status"
is_server_alive && pass "n2_server_survives_overload" || fail "n2_server_survives_overload"
stop_server

echo "e2e pool: $E2E_PASS passed, $E2E_FAIL failed"
[ "$E2E_FAIL" = 0 ]
