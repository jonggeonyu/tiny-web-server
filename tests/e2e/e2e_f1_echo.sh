#!/usr/bin/env bash
# F1 echo 서버 기능 테스트 (F1, F12 일부, N5, N6, N7 일부)
source "$(dirname "$0")/lib.sh"
PORT=18001

# --- 인자 검사 (F12) ---
if "$SERVER" >/dev/null 2>&1; then
    fail "f12_no_port_exits_with_usage"
else
    pass "f12_no_port_exits_with_usage"
fi
if "$SERVER" 99999 >/dev/null 2>&1; then
    fail "f12_bad_port_exits_with_usage"
else
    pass "f12_bad_port_exits_with_usage"
fi

start_server "$PORT" || { fail "f1_server_starts"; exit 1; }
pass "f1_server_starts"
FDS_BEFORE=$(count_fds)

# --- 한 줄 echo ---
reply=$(printf 'hello\n' | timeout 3 nc -N 127.0.0.1 "$PORT")
[ "$reply" = "hello" ] && pass "f1_echo_one_line" \
    || fail "f1_echo_one_line" "got '$reply'"

# --- 여러 줄, CRLF 그대로 ---
reply=$(printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\n' \
    | timeout 3 nc -N 127.0.0.1 "$PORT" | od -c | head -3)
expected=$(printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\n' | od -c | head -3)
[ "$reply" = "$expected" ] && pass "f1_echo_keeps_crlf" \
    || fail "f1_echo_keeps_crlf"

# --- 1바이트씩 나눠 보내도 한 줄로 돌아옴 (N6) ---
reply=$( (for c in a b c d; do printf '%s' "$c"; sleep 0.05; done; printf '\n') \
    | timeout 3 nc -N 127.0.0.1 "$PORT")
[ "$reply" = "abcd" ] && pass "n6_split_bytes_echo_as_one_line" \
    || fail "n6_split_bytes_echo_as_one_line" "got '$reply'"

# --- 큰 입력 (버퍼 8 KB보다 큼) 그대로 돌아옴 ---
head -c 200000 /dev/urandom | base64 -w 100 >"$TMP/big.txt"
timeout 5 nc -N 127.0.0.1 "$PORT" <"$TMP/big.txt" >"$TMP/big_reply.txt"
if cmp -s "$TMP/big.txt" "$TMP/big_reply.txt"; then
    pass "n6_large_input_echo_intact"
else
    fail "n6_large_input_echo_intact"
fi

# --- 클라이언트가 보내다 끊어도 서버 생존 (N5) ---
printf 'half line without newline' | timeout 1 nc -q 0 127.0.0.1 "$PORT" >/dev/null
# 많이 보내고 응답은 안 읽고 바로 끊기: 서버 write가 EPIPE/ECONNRESET을 맞음
timeout 1 nc -q 0 127.0.0.1 "$PORT" <"$TMP/big.txt" >/dev/null 2>&1
sleep 0.2
is_server_alive && pass "n5_client_disconnect_server_survives" \
    || fail "n5_client_disconnect_server_survives"

reply=$(printf 'still here\n' | timeout 3 nc -N 127.0.0.1 "$PORT")
[ "$reply" = "still here" ] && pass "n5_server_works_after_disconnects" \
    || fail "n5_server_works_after_disconnects" "got '$reply'"

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
printf 'x\n' | timeout 3 nc -N 127.0.0.1 "$PORT" >/dev/null
stop_server
if start_server "$PORT"; then
    pass "f1_restart_immediately_rebinds"
else
    fail "f1_restart_immediately_rebinds" "$(cat "$TMP/server_$PORT.log")"
fi
stop_server

echo "e2e f1: $E2E_PASS passed, $E2E_FAIL failed"
[ "$E2E_FAIL" -eq 0 ]
