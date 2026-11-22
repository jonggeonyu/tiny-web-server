/* 연결 하나를 처리한다: 요청 읽기 → 응답 보내기 (F2~F5 연결부).
 * 지금은 요청 하나에 응답 하나를 보내고 끝낸다. keep-alive는 F11에서 붙인다. */
#ifndef CONNECTION_H
#define CONNECTION_H

/* connfd에서 HTTP 요청 하나를 받아 응답한다.
 * connfd는 닫지 않는다 (닫는 건 부른 쪽). 반환: 0, 소켓 에러 -1 */
int connection_serve(int connfd);

#endif
