#!/bin/sh
# 서버가 넘겨준 환경 변수를 모두 보여준다
printf 'Content-Type: text/plain; charset=utf-8\r\n\r\n'
env | sort
