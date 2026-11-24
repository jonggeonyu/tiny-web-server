#!/bin/sh
# 출력 도중 스스로 죽는 CGI (SIGSEGV) → 502
printf 'Content-Type: text/plain\r\n\r\npartial output...\n'
kill -SEGV $$
