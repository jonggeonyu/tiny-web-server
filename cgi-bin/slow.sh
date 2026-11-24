#!/bin/sh
# 2초 걸리는 CGI (Step 8 동시 처리 확인용)
sleep 2
printf 'Content-Type: text/plain\r\n\r\nslow done\n'
