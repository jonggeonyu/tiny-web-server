#!/usr/bin/env bash
# 정적 파일 기능 테스트 (F6, F7, F8, F9)
source "$(dirname "$0")/lib.sh"
PORT=18003

# 테스트 파일: 텍스트, 하위 디렉터리, 바이너리(무작위 1 MB + 홀수 바이트)
mkdir -p "$WWW/docs" "$WWW/empty"
printf 'body { color: red; }\n' >"$WWW/style.css"
printf '<p>docs</p>\n' >"$WWW/docs/index.html"
head -c 1048579 /dev/urandom >"$WWW/random.png"
ln -sf /etc/passwd "$WWW/escape"

start_server "$PORT" || { fail "server_starts"; exit 1; }
URL="http://127.0.0.1:$PORT"

# get <경로> : 상태 코드만 출력, 본문은 $TMP/got 에 저장
get() { curl_local -o "$TMP/got" -w '%{http_code}' "$URL$1"; }
# content_type <경로>
content_type() { curl_local -o /dev/null -w '%{content_type}' "$URL$1"; }

# --- F6 파일 전송 ---
expect_status "f6_root_serves_index_html" 200 "$(get /)"
grep -q "<h1>index</h1>" "$TMP/got" && pass "f6_index_html_body" \
    || fail "f6_index_html_body"
expect_status "f6_subdirectory_serves_index_html" 200 "$(get /docs)"
grep -q "docs" "$TMP/got" && pass "f6_subdirectory_body" \
    || fail "f6_subdirectory_body"

expect_status "f6_binary_file_200" 200 "$(get /random.png)"
want=$(sha256sum <"$WWW/random.png" | cut -d' ' -f1)
got=$(sha256sum <"$TMP/got" | cut -d' ' -f1)
[ "$want" = "$got" ] && pass "f6_binary_sha256_matches" \
    || fail "f6_binary_sha256_matches" "want $want got $got"

# --- F7 Content-Type ---
expect_status "f7_html_type" "text/html; charset=utf-8" "$(content_type /)"
expect_status "f7_css_type" "text/css; charset=utf-8" "$(content_type /style.css)"
expect_status "f7_png_type" "image/png" "$(content_type /random.png)"

# --- F8 경로 탈출 (curl은 ..를 스스로 지우므로 nc로 그대로 보낸다) ---
check() {   # check <이름> <기대 코드> <요청 printf 문자열>
    expect_status "$1" "$2" "$(status_of "$(raw_request "$PORT" "$3")")"
}
check "f8_dot_dot_returns_403"         403 'GET /../etc/passwd HTTP/1.0\r\n\r\n'
# raw_request가 printf 형식으로 쓰므로 %는 %%로 적는다
check "f8_encoded_dot_dot_returns_403" 403 'GET /%%2e%%2e/%%2e%%2e/etc/passwd HTTP/1.0\r\n\r\n'
check "f8_symlink_escape_returns_403"  403 'GET /escape HTTP/1.0\r\n\r\n'
# root 사용자이거나, 윈도우 폴더를 마운트해서 chmod가 안 먹으면 건너뛴다
printf 's\n' >"$WWW/secret.txt"
chmod 000 "$WWW/secret.txt"
if [ -r "$WWW/secret.txt" ]; then
    echo "skip f8_unreadable_file_returns_403 (이 환경은 chmod 000이어도 읽힘)"
else
    check "f8_unreadable_file_returns_403" 403 'GET /secret.txt HTTP/1.0\r\n\r\n'
fi

# --- F9 오류 응답 ---
check "f9_missing_file_returns_404"    404 'GET /nope.html HTTP/1.0\r\n\r\n'
check "f9_dir_without_index_returns_404" 404 'GET /empty HTTP/1.0\r\n\r\n'
reply=$(raw_request "$PORT" 'GET /nope.html HTTP/1.0\r\n\r\n')
printf '%s' "$reply" | grep -q "<h1>404 Not Found</h1>" \
    && pass "f9_error_body_is_html" || fail "f9_error_body_is_html"

# --- 큰 파일을 받다가 끊어도 서버는 산다 (N5) ---
timeout 1 curl --noproxy '*' -s --limit-rate 50k -o /dev/null "$URL/random.png"
sleep 0.2
is_server_alive && pass "n5_abort_during_file_server_survives" \
    || fail "n5_abort_during_file_server_survives"
expect_status "n5_server_works_after_abort" 200 "$(get /)"

stop_server
echo "e2e static: $E2E_PASS passed, $E2E_FAIL failed"
[ "$E2E_FAIL" -eq 0 ]
