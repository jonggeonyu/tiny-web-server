#!/bin/sh
# Status 헤더로 상태 코드를 직접 정하는 CGI
printf 'Status: 404 Not Found\r\n'
printf 'Content-Type: text/plain\r\n'
printf 'X-Custom: hello\r\n'
printf '\r\n'
printf 'no such item\n'
