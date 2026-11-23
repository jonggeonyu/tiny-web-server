/* 스레드 풀과 연결 큐 (N1, N2).
 * accept한 연결(connfd)을 고정 크기 큐에 넣고, 미리 만든 일꾼 스레드가
 * 하나씩 꺼내 처리한다. 큐가 가득 차면 넣기가 실패하고 부른 쪽이 503으로
 * 거절한다. 큐 크기와 일꾼 수는 만들 때 정하고 바뀌지 않는다. */
#ifndef POOL_H
#define POOL_H

#include <stdbool.h>
#include <stddef.h>

/* 일꾼이 연결 하나를 처리하는 함수. connfd는 이 함수가 닫는다.
 * context는 pool_create에 준 값 그대로 온다. */
typedef void (*PoolJob)(int connfd, void *context);

/* 내부는 pool.c에만 있다. 밖에서는 포인터로만 쓴다. */
typedef struct WorkerPool WorkerPool;

/* 일꾼 worker_count개와 크기 queue_capacity인 큐를 만든다.
 * 반환: 풀, 실패하면 NULL (errno에 원인) */
WorkerPool *pool_create(size_t worker_count, size_t queue_capacity,
                        PoolJob job, void *context);

/* connfd를 큐에 넣는다. 기다리지 않는다.
 * 반환: 0이면 connfd 소유권이 풀로 넘어간다 (일꾼의 job이 닫는다).
 *       큐가 가득 찼거나 종료 중이면 -1이고, connfd는 부른 쪽이 닫는다. */
int pool_submit(WorkerPool *pool, int connfd);

/* 큐에 아직 아무 일꾼도 꺼내 가지 않은 연결이 있는지.
 * 있으면 keep-alive를 끊어 일꾼을 양보한다 (F11). */
bool pool_has_waiting(WorkerPool *pool);

/* 새 연결을 더 받지 않고, 큐에 남은 연결까지 다 처리한 뒤 일꾼을 끝내고
 * 풀을 해제한다. 모든 일꾼이 끝날 때까지 돌아오지 않는다. */
void pool_destroy(WorkerPool *pool);

#endif
