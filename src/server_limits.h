/* 자원 한도 상수 (N3). 숫자를 코드에 바로 쓰지 않고 여기 상수를 쓴다. */
#ifndef SERVER_LIMITS_H
#define SERVER_LIMITS_H

/* rio 내부 버퍼 크기. 한 번의 read 시스템 콜로 가져오는 최대 바이트 수 */
#define RIO_BUFFER_SIZE 8192

/* 한 줄 최대 길이. HTTP 요청 라인 한도(8 KB)와 맞춘다 */
#define MAX_LINE 8192

/* listen 대기 큐 크기 (커널이 handshake를 끝낸 연결을 쌓아두는 곳) */
#define LISTEN_BACKLOG 1024

/* 로그에 찍는 클라이언트 주소 문자열 길이 (IPv4 "255.255.255.255", 포트 "65535") */
#define MAX_HOST_TEXT 64
#define MAX_PORT_TEXT 8

#endif
