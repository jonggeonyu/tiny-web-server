#!/bin/sh
# 가장 간단한 CGI: 헤더 + 빈 줄 + 본문
printf 'Content-Type: text/html; charset=utf-8\r\n'
printf '\r\n'
printf '<h1>Hello from CGI!</h1>\n'
printf '<p>현재 시각: %s</p>\n' "$(date '+%Y-%m-%d %H:%M:%S')"
printf '<p>요청 방식: %s, 쿼리: %s</p>\n' "$REQUEST_METHOD" "$QUERY_STRING"
