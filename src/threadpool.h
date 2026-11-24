/*
 * threadpool: 미리 만들어 둔 일꾼 스레드 N개 + 크기가 정해진 대기열.
 *
 *   accept 스레드 ──submit(fd)──▶ [대기열: fd fd fd ...] ──▶ 일꾼 1..N 이 하나씩 꺼내 처리
 *
 * - 대기열이 가득 차면 submit 은 기다리지 않고 바로 실패(-1)한다 → 호출한 쪽이 503 을 보낸다.
 *   (기다리게 하면 accept 스레드가 멈춰서 새 연결을 받지도, 거절하지도 못한다)
 * - 일꾼은 대기열이 비어 있으면 조건 변수에서 잠들어 CPU 를 쓰지 않는다.
 */
#ifndef THREADPOOL_H
#define THREADPOOL_H

/* 일꾼이 할 일: 대기열에서 꺼낸 fd 하나를 처리한다.
 * tag: fd 와 함께 넣은 숫자 (서버에서는 "이 연결에서 이미 처리한 요청 수") */
typedef void (*tp_job_fn)(int fd, int tag, void *arg);

typedef struct threadpool threadpool;

/* nthreads 개의 일꾼과 queue_cap 칸짜리 대기열을 만든다. 실패 시 NULL */
threadpool *threadpool_create(int nthreads, int queue_cap, tp_job_fn fn, void *arg);

/* 대기열에 (fd, tag) 를 넣는다. 반환: 0 = 넣음, -1 = 가득 참 또는 종료 중 (fd 는 호출자가 처리) */
int threadpool_submit(threadpool *tp, int fd, int tag);

/* 지금 일하는 중인 일꾼 수와 대기열에 기다리는 일 수 (종료 로그용) */
void threadpool_stats(threadpool *tp, int *busy, int *queued);

/* 대기열에 남은 일까지 모두 처리한 뒤 일꾼을 끝내고 정리한다 */
void threadpool_destroy(threadpool *tp);

/* 지금 실행 중인 스레드의 일꾼 번호 (1..N). 일꾼이 아니면 0 (로그용) */
int threadpool_worker_id(void);

#endif /* THREADPOOL_H */
