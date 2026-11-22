#!/bin/sh
# 실행 권한이 없는 파일 → 403 이어야 한다
printf 'Content-Type: text/plain\r\n\r\nYOU SHOULD NOT SEE THIS\n'
