#!/bin/sh
# POST 본문(표준 입력)을 그대로 돌려준다. 4KB 씩 읽는 즉시 출력한다 (dd).
# → 큰 본문이면 서버가 "넣기"와 "받기"를 동시에 해야 멈추지 않는다 (교착 방지 확인용)
printf 'Content-Type: application/octet-stream\r\n'
printf 'X-Received-Length: %s\r\n' "${CONTENT_LENGTH:-0}"
printf '\r\n'
dd bs=4096 2>/dev/null
