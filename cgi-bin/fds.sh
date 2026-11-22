#!/bin/sh
# CGI 가 물려받은 파일 디스크립터 목록 (0,1,2 만 있어야 정상. 3번은 ls 가 목록을 읽으려고 스스로 연 것)
printf 'Content-Type: text/plain\r\n\r\n'
ls -l /proc/self/fd
