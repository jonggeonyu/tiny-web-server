#!/usr/bin/env bash
# POST 본문 기능 테스트 (F4, N3 본문 한도)
source "$(dirname "$0")/lib.sh"
PORT=18004

start_server "$PORT" || { fail "server_starts"; exit 1; }
URL="http://127.0.0.1:$PORT"

# post_file <파일> : 본문으로 보내고 응답 전체를 $TMP/post_reply 에 저장, 상태 코드 출력
# curl은 1 KB가 넘는 본문에 "Expect: 100-continue"를 붙이고 답을 1초 기다린다.
# 서버가 100 Continue를 보내지 않으므로 헤더를 비워서 바로 보내게 한다
post_file() {
    curl_local -o "$TMP/post_reply" -w '%{http_code}' -H 'Expect:' \
        --data-binary "@$1" "$URL/cgi-bin/echo"
}
body_bytes() { awk -F': ' '$1=="body-bytes"{print $2}' "$TMP/post_reply"; }

# --- 1바이트 ---
printf 'x' >"$TMP/one.bin"
expect_status "f4_one_byte_body_200" 200 "$(post_file "$TMP/one.bin")"
expect_status "f4_one_byte_body_read" 1 "$(body_bytes)"

# --- 한도 딱 맞음 (1 MB) ---
head -c 1048576 /dev/urandom >"$TMP/limit.bin"
expect_status "f4_body_at_limit_200" 200 "$(post_file "$TMP/limit.bin")"
expect_status "f4_body_at_limit_read" 1048576 "$(body_bytes)"

# --- 한도 + 1바이트 → 413 ---
head -c 1048577 /dev/urandom >"$TMP/over.bin"
expect_status "f4_body_over_limit_413" 413 "$(post_file "$TMP/over.bin")"

# --- 길이 없음 → 411 ---
reply=$(raw_request "$PORT" 'POST /cgi-bin/echo HTTP/1.1\r\nHost: x\r\n\r\n')
expect_status "f4_missing_length_returns_411" 411 "$(status_of "$reply")"

# --- 헤더와 본문이 따로 도착해도 끝까지 읽는다 (N6) ---
reply=$( (printf 'POST /cgi-bin/echo HTTP/1.0\r\nContent-Length: 10\r\n\r\nhello'
          sleep 0.3; printf 'world') | timeout 3 nc -N 127.0.0.1 "$PORT")
expect_status "f4_split_body_200" 200 "$(status_of "$reply")"
printf '%s' "$reply" | grep -q "body-bytes: 10" \
    && pass "f4_split_body_fully_read" || fail "f4_split_body_fully_read"

# --- 본문을 덜 보내고 끊어도 서버는 산다 (N5) ---
printf 'POST /cgi-bin/echo HTTP/1.0\r\nContent-Length: 100\r\n\r\nabc' \
    | timeout 1 nc -q 0 127.0.0.1 "$PORT" >/dev/null
sleep 0.2
is_server_alive && pass "n5_short_body_server_survives" \
    || fail "n5_short_body_server_survives"
grep -q "client closed during body (got 3 of 100 bytes)" "$TMP/server_$PORT.log" \
    && pass "f4_short_body_logged" || fail "f4_short_body_logged"
expect_status "n5_server_works_after_short_body" 200 "$(post_file "$TMP/one.bin")"

stop_server
echo "e2e post: $E2E_PASS passed, $E2E_FAIL failed"
[ "$E2E_FAIL" -eq 0 ]
