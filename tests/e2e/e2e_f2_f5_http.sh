#!/usr/bin/env bash
# HTTP 요청 파싱·응답 기능 테스트 (F2, F3, F5, N3 일부)
source "$(dirname "$0")/lib.sh"
PORT=18002

start_server "$PORT" || { fail "server_starts"; exit 1; }

# --- 정상 요청 (curl) ---
headers=$(curl_local -D - -o "$TMP/body.txt" "http://127.0.0.1:$PORT/hello?x=1")
expect_status "f2_curl_get_200" 200 "$(status_of "$headers")"
grep -q "uri: /hello?x=1" "$TMP/body.txt" && pass "f2_uri_with_query_parsed" \
    || fail "f2_uri_with_query_parsed"
grep -q "method: GET" "$TMP/body.txt" && pass "f2_method_parsed" \
    || fail "f2_method_parsed"

# --- F5 응답 헤더 ---
for h in "Content-Type: text/plain" "Content-Length: " "Connection: close" "Date: "; do
    printf '%s' "$headers" | grep -qi "^$h" && pass "f5_header ${h%%:*}" \
        || fail "f5_header ${h%%:*}"
done
declared=$(printf '%s' "$headers" | tr -d '\r' | awk -F': ' 'tolower($1)=="content-length"{print $2}')
actual=$(wc -c <"$TMP/body.txt")
[ "$declared" = "$actual" ] && pass "f5_content_length_matches_body ($actual)" \
    || fail "f5_content_length_matches_body" "header $declared, body $actual"

# --- POST + 본문 (본문을 읽어 버려서 클라이언트가 응답을 정상 수신) ---
reply=$(curl_local -i -X POST --data-binary "a=1&b=2" "http://127.0.0.1:$PORT/form")
expect_status "f3_post_with_body_200" 200 "$(status_of "$reply")"
printf '%s' "$reply" | grep -q "content-length: 7" \
    && pass "f3_post_content_length_parsed" || fail "f3_post_content_length_parsed"

# --- 오류 응답 (raw 바이트) ---
check() {   # check <이름> <기대 코드> <요청 printf 문자열>
    local reply
    reply=$(raw_request "$PORT" "$3")
    expect_status "$1" "$2" "$(status_of "$reply")"
}
check "f2_garbage_returns_400"         400 'hello\r\n\r\n'
check "f2_relative_uri_returns_400"    400 'GET index.html HTTP/1.1\r\nHost: x\r\n\r\n'
check "f2_put_returns_501"             501 'PUT / HTTP/1.1\r\nHost: x\r\n\r\n'
check "f2_http2_returns_505"           505 'GET / HTTP/2.0\r\nHost: x\r\n\r\n'
check "f3_missing_host_returns_400"    400 'GET / HTTP/1.1\r\n\r\n'
check "f3_bad_header_returns_400"      400 'GET / HTTP/1.1\r\nHost x\r\n\r\n'
check "f3_chunked_returns_501"         501 'POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n'
check "n3_body_too_large_returns_413"  413 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\n\r\n'

long_uri=$(head -c 9000 /dev/zero | tr '\0' 'a')
check "n3_long_uri_returns_414"        414 "GET /$long_uri HTTP/1.1\r\nHost: x\r\n\r\n"

many=""
for i in $(seq 1 70); do many="${many}X-$i: v\r\n"; done
check "n3_too_many_headers_returns_431" 431 "GET / HTTP/1.1\r\nHost: x\r\n${many}\r\n"

# --- 오류 응답 본문은 HTML ---
reply=$(raw_request "$PORT" 'PUT / HTTP/1.1\r\nHost: x\r\n\r\n')
printf '%s' "$reply" | grep -q "<h1>501 Not Implemented</h1>" \
    && pass "f5_error_body_is_html" || fail "f5_error_body_is_html"

is_server_alive && pass "server_alive_after_bad_requests" \
    || fail "server_alive_after_bad_requests"
stop_server

echo "e2e http: $E2E_PASS passed, $E2E_FAIL failed"
[ "$E2E_FAIL" -eq 0 ]
