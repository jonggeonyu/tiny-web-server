/* 연결 하나를 처리한다: 요청 읽기 → 응답 보내기 (F2~F5 연결부).
 * 지금은 요청 하나에 응답 하나를 보내고 끝낸다. keep-alive는 F11에서 붙인다. */
#ifndef CONNECTION_H
#define CONNECTION_H

/* connfd에서 HTTP 요청 하나를 받아 응답한다.
 * connfd는 닫지 않는다 (닫는 건 부른 쪽).
 * 반환: 0, 실패 -1. 실패 원인은 이 함수가 이미 stderr에 찍었으므로
 * 부른 쪽은 errno를 읽지 말고 연결만 닫는다. */
int connection_serve(int connfd);

#endif
