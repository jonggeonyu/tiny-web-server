#!/usr/bin/env bash
# 소켓 계층 기능 테스트 (F1, F12 일부, N5, N6, N7 일부)
source "$(dirname "$0")/lib.sh"
PORT=18001

# --- 인자 검사 (F12) ---
if "$SERVER" >/dev/null 2>&1; then fail "f12_no_port_exits_with_usage"
else pass "f12_no_port_exits_with_usage"; fi
if "$SERVER" 99999 >/dev/null 2>&1; then fail "f12_bad_port_exits_with_usage"
else pass "f12_bad_port_exits_with_usage"; fi

start_server "$PORT" || { fail "f1_server_starts"; exit 1; }
pass "f1_server_starts"
FDS_BEFORE=$(count_fds)

# --- 연결 수락 후 응답 ---
reply=$(raw_request "$PORT" 'GET / HTTP/1.0\r\n\r\n')
expect_status "f1_accept_and_reply" 200 "$(status_of "$reply")"

# --- 요청을 1바이트씩 나눠 보내도 하나의 요청으로 처리 (N6) ---
printf -v request 'GET /split HTTP/1.0\r\n\r\n'   # $( ) 는 끝 줄바꿈을 지워서 쓰지 않음
reply=$( (for ((i = 0; i < ${#request}; i++)); do
            printf '%s' "${request:i:1}"; sleep 0.01; done) \
         | timeout 5 nc -N 127.0.0.1 "$PORT")
expect_status "n6_split_bytes_one_request" 200 "$(status_of "$reply")"
printf '%s' "$reply" | grep -q "uri: /split" \
    && pass "n6_split_bytes_uri_intact" || fail "n6_split_bytes_uri_intact"

# --- 클라이언트가 보내다 끊어도 서버 생존 (N5) ---
printf 'GET / HTTP/1.1\r\nHost: x' | timeout 1 nc -q 0 127.0.0.1 "$PORT" >/dev/null
# 응답은 안 읽고 바로 끊기
printf 'GET / HTTP/1.0\r\n\r\n' | timeout 1 nc -q 0 -d 127.0.0.1 "$PORT" \
    >/dev/null 2>&1
sleep 0.2
is_server_alive && pass "n5_client_disconnect_server_survives" \
    || fail "n5_client_disconnect_server_survives"
reply=$(raw_request "$PORT" 'GET / HTTP/1.0\r\n\r\n')
expect_status "n5_server_works_after_disconnects" 200 "$(status_of "$reply")"

# --- fd 누수 없음 (N7) ---
FDS_AFTER=$(wait_fds "$FDS_BEFORE")
[ "$FDS_BEFORE" = "$FDS_AFTER" ] && pass "n7_no_fd_leak ($FDS_AFTER fds)" \
    || fail "n7_no_fd_leak" "before $FDS_BEFORE after $FDS_AFTER"

# --- 같은 포트로 두 번 띄우면 에러 메시지 후 종료 (F1) ---
if "$SERVER" "$PORT" >"$TMP/second.log" 2>&1; then
    fail "f1_same_port_twice_fails"
elif grep -q "Address already in use" "$TMP/second.log"; then
    pass "f1_same_port_twice_fails"
else
    fail "f1_same_port_twice_fails" "$(cat "$TMP/second.log")"
fi

# --- 끄고 바로 다시 켜도 bind 성공 ---
raw_request "$PORT" 'GET / HTTP/1.0\r\n\r\n' >/dev/null
stop_server
if start_server "$PORT"; then pass "f1_restart_immediately_rebinds"
else fail "f1_restart_immediately_rebinds" "$(cat "$TMP/server_$PORT.log")"; fi
stop_server

echo "e2e f1: $E2E_PASS passed, $E2E_FAIL failed"
[ "$E2E_FAIL" -eq 0 ]
