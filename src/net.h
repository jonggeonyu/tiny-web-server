/* 소켓 열기 (F1). socket -> bind -> listen 흐름을 한 곳에 둔다. */
#ifndef NET_H
#define NET_H

#include <stdbool.h>

/* port에서 연결을 받는 listen 소켓을 연다. SO_REUSEADDR를 켠다.
 * 실패하면 원인을 stderr에 찍고 -1. 반환된 fd는 부른 쪽이 닫는다. */
int net_open_listenfd(const char *port);

/* 포트 문자열이 1~65535 사이 숫자인지 */
bool net_is_valid_port(const char *port);

#endif
