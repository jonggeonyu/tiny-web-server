#!/bin/sh
# 끝나지 않는 CGI → 시간 제한 후 강제 종료 + 504
printf 'Content-Type: text/plain\r\n\r\nstarted\n'
while :; do sleep 1; done
