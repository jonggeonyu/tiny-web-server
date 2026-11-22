/* 연결 하나를 echo로 처리한다 (F1 단계용). HTTP 처리가 생기면 대체된다. */
#ifndef ECHO_H
#define ECHO_H

/* connfd에서 한 줄씩 읽어 그대로 돌려보낸다. 상대가 닫거나 에러가 나면 끝.
 * connfd는 닫지 않는다 (닫는 건 부른 쪽). 반환: 정상 종료 0, 에러 -1 */
int echo_serve_connection(int connfd);

#endif
