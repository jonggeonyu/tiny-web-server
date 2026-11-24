#!/bin/sh
# CGI 가 서버가 막아 둔 시그널(SIGINT/SIGTERM)을 물려받지 않았는지 확인용: 자기 시그널 마스크를 출력
printf 'Content-Type: text/plain\r\n\r\n'
grep -E '^Sig(Blk|Ign):' /proc/$$/status
