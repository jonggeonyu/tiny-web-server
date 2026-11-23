#!/usr/bin/env bash
# CGI 기능 테스트 (F10, N3 CGI 실행 시간)
source "$(dirname "$0")/lib.sh"
PORT=18010

# 테스트용 CGI 프로그램들 ($WWW/cgi-bin/echo는 lib.sh가 만든다)
cgi() {   # cgi <이름> : 표준 입력의 스크립트를 실행 가능하게 저장
    cat >"$WWW/cgi-bin/$1"; chmod 755 "$WWW/cgi-bin/$1"
}
cgi adder <<'CGI'
#!/bin/sh
# CSAPP adder와 같다: QUERY_STRING "15&27" → 두 수의 합
a=${QUERY_STRING%%&*}; b=${QUERY_STRING#*&}
printf 'Content-type: text/html\r\n\r\n'
printf '<p>%s + %s = %s</p>\n' "$a" "$b" $((a + b))
CGI
cgi fails <<'CGI'
#!/bin/sh
printf 'Content-Type: text/plain\r\n\r\nhalf'
exit 3
CGI
cgi slow <<'CGI'
#!/bin/sh
sleep 30
CGI
cgi noheader <<'CGI'
#!/bin/sh
echo "no blank line, no header"
CGI
cgi upper <<'CGI'
#!/bin/sh
printf 'Content-Type: text/plain\r\n\r\n'
tr 'a-z' 'A-Z'
CGI
printf '#!/bin/sh\necho secret source\n' >"$WWW/cgi-bin/readonly"
chmod 644 "$WWW/cgi-bin/readonly"

start_server "$PORT" || { fail "server_starts"; exit 1; }
URL="http://127.0.0.1:$PORT"
check() {   # check <이름> <기대 코드> <요청 printf 문자열>
    expect_status "$1" "$2" "$(status_of "$(raw_request "$PORT" "$3")")"
}

# --- GET: QUERY_STRING ---
reply=$(curl_local -i "$URL/cgi-bin/adder?15&27")
expect_status "f10_get_200" 200 "$(status_of "$reply")"
printf '%s' "$reply" | grep -q "15 + 27 = 42" && pass "f10_get_query_string" \
    || fail "f10_get_query_string" "$reply"
printf '%s' "$reply" | grep -qi "^Content-Type: text/html" \
    && pass "f10_cgi_content_type_passed" || fail "f10_cgi_content_type_passed"
declared=$(printf '%s' "$reply" | tr -d '\r' \
           | awk -F': ' 'tolower($1)=="content-length"{print $2}')
[ "$declared" = "$(printf '<p>15 + 27 = 42</p>\n' | wc -c)" ] \
    && pass "f10_content_length_counts_body_only" \
    || fail "f10_content_length_counts_body_only" "got $declared"

# --- POST: 본문이 stdin으로 간다 ---
body=$(curl_local -X POST --data-binary "hello cgi" "$URL/cgi-bin/upper")
expect_status "f10_post_body_to_stdin" "HELLO CGI" "$body"
reply=$(curl_local -X POST -H 'Content-Type: text/plain' --data-binary "abc" \
        "$URL/cgi-bin/echo?x=1")
printf '%s' "$reply" | grep -q "method: POST" && printf '%s' "$reply" \
    | grep -q "query: x=1" && printf '%s' "$reply" | grep -q "body-bytes: 3" \
    && pass "f10_post_environment" || fail "f10_post_environment" "$reply"

# --- 실패하면 500 ---
check "f10_nonzero_exit_returns_500" 500 'GET /cgi-bin/fails HTTP/1.0\r\n\r\n'
check "f10_no_header_returns_500"    500 'GET /cgi-bin/noheader HTTP/1.0\r\n\r\n'
start=$(date +%s)
reply=$(printf 'GET /cgi-bin/slow HTTP/1.0\r\n\r\n' | timeout 10 nc -N 127.0.0.1 "$PORT")
elapsed=$(( $(date +%s) - start ))
expect_status "f10_timeout_returns_500" 500 "$(status_of "$reply")"
[ "$elapsed" -le 7 ] && pass "f10_timeout_within_limit (${elapsed}s)" \
    || fail "f10_timeout_within_limit" "${elapsed}s"
check "f10_missing_program_returns_404" 404 'GET /cgi-bin/nope HTTP/1.0\r\n\r\n'

# 실행 권한이 없는 프로그램: 403, 소스도 새면 안 된다
if [ -x "$WWW/cgi-bin/readonly" ]; then
    echo "skip f10_not_executable_returns_403 (이 환경은 chmod 644여도 실행 가능)"
else
    check "f10_not_executable_returns_403" 403 'GET /cgi-bin/readonly HTTP/1.0\r\n\r\n'
fi

# --- 인코딩해도 CGI로 간다 (소스가 정적 파일로 새지 않음) ---
reply=$(raw_request "$PORT" 'GET /%%63gi-bin/adder?1&2 HTTP/1.0\r\n\r\n')
printf '%s' "$reply" | grep -q "1 + 2 = 3" && pass "f10_encoded_cgi_path_runs" \
    || fail "f10_encoded_cgi_path_runs" "$reply"

# --- 정적 파일에 POST는 501 ---
check "f10_post_to_static_returns_501" 501 \
      'POST /index.html HTTP/1.0\r\nContent-Length: 1\r\n\r\nx'

# --- 좀비 프로세스가 남지 않는다 ---
sleep 0.2
zombies=$(ps -o stat= --ppid "$SERVER_PID" 2>/dev/null | grep -c '^Z')
expect_status "f10_no_zombie_processes" 0 "$zombies"
children=$(ps -o pid= --ppid "$SERVER_PID" 2>/dev/null | wc -l)
expect_status "f10_no_leftover_children" 0 "$children"
# 시간 초과로 죽인 스크립트가 띄운 sleep 30도 같이 죽었는지
leftover=$(pgrep -f "^sleep 30$" | wc -l)
expect_status "f10_timeout_kills_grandchildren" 0 "$leftover"

is_server_alive && pass "server_alive_after_cgi" || fail "server_alive_after_cgi"
stop_server
echo "e2e cgi: $E2E_PASS passed, $E2E_FAIL failed"
[ "$E2E_FAIL" -eq 0 ]
