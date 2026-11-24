/* 스레드 풀 단위 테스트 (N1, N2). 소켓 대신 pipe fd를 큐에 넣는다. */
#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>

#include "pool.h"
#include "test.h"

/* 일꾼이 부르는 job이 기록하는 값. lock이 지킨다 */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int handled;            /* 처리한 연결 수 */
    int running;            /* 지금 job 안에 있는 일꾼 수 */
    int max_running;        /* 동시에 job 안에 있었던 최대 일꾼 수 */
    bool is_released;       /* false면 job이 풀릴 때까지 기다린다 */
} JobLog;

static void init_log(JobLog *log, bool is_released)
{
    pthread_mutex_init(&log->lock, NULL);
    pthread_cond_init(&log->changed, NULL);
    log->handled = 0;
    log->running = 0;
    log->max_running = 0;
    log->is_released = is_released;
}

static void destroy_log(JobLog *log)
{
    pthread_cond_destroy(&log->changed);
    pthread_mutex_destroy(&log->lock);
}

/* 연결 하나 처리: 기록하고, 풀릴 때까지 기다렸다가 fd를 닫는다 */
static void record_job(int connfd, void *context)
{
    JobLog *log = context;

    pthread_mutex_lock(&log->lock);
    log->running++;
    if (log->running > log->max_running) {
        log->max_running = log->running;
    }
    pthread_cond_broadcast(&log->changed);
    while (!log->is_released) {
        pthread_cond_wait(&log->changed, &log->lock);
    }
    log->running--;
    log->handled++;
    pthread_mutex_unlock(&log->lock);
    close(connfd);
}

/* 일꾼 count개가 job 안에 들어올 때까지 기다린다 */
static void wait_running(JobLog *log, int count)
{
    pthread_mutex_lock(&log->lock);
    while (log->running < count) {
        pthread_cond_wait(&log->changed, &log->lock);
    }
    pthread_mutex_unlock(&log->lock);
}

static void release_jobs(JobLog *log)
{
    pthread_mutex_lock(&log->lock);
    log->is_released = true;
    pthread_cond_broadcast(&log->changed);
    pthread_mutex_unlock(&log->lock);
}

/* 큐에 넣을 fd 하나 (pipe의 읽는 쪽). 쓰는 쪽은 바로 닫는다 */
static int make_fd(void)
{
    int fds[2];
    CHECK(pipe(fds) == 0);
    close(fds[1]);
    return fds[0];
}

static void test_n1_create_rejects_zero_sizes(void)
{
    JobLog log;
    init_log(&log, true);
    CHECK(pool_create(0, 1, record_job, &log) == NULL);
    CHECK(pool_create(1, 0, record_job, &log) == NULL);
    CHECK(pool_create(1, 1, NULL, &log) == NULL);
    destroy_log(&log);
}

/* 넣은 연결은 하나도 빠짐없이 처리되고, destroy는 남은 것까지 처리한다 */
static void test_n1_every_submitted_connection_is_handled(void)
{
    enum { JOBS = 50 };
    JobLog log;
    init_log(&log, true);
    WorkerPool *pool = pool_create(4, JOBS, record_job, &log);
    CHECK(pool != NULL);
    for (int i = 0; i < JOBS; i++) {
        CHECK(pool_submit(pool, make_fd()) == 0);
    }
    pool_destroy(pool);
    CHECK(log.handled == JOBS);
    destroy_log(&log);
}

/* 일꾼 4개면 오래 걸리는 연결 4개를 동시에 처리한다 */
static void test_n1_workers_run_in_parallel(void)
{
    JobLog log;
    init_log(&log, false);
    WorkerPool *pool = pool_create(4, 4, record_job, &log);
    CHECK(pool != NULL);
    for (int i = 0; i < 4; i++) {
        CHECK(pool_submit(pool, make_fd()) == 0);
    }
    wait_running(&log, 4);      /* 넷이 동시에 안 들어오면 여기서 멈춘다 */
    CHECK(log.max_running == 4);
    release_jobs(&log);
    pool_destroy(pool);
    CHECK(log.handled == 4);
    destroy_log(&log);
}

/* 일꾼 1개 + 큐 1칸: 처리 중 1 + 대기 1까지만 받고 그다음은 거절 */
static void test_n2_submit_fails_when_queue_full(void)
{
    JobLog log;
    init_log(&log, false);
    WorkerPool *pool = pool_create(1, 1, record_job, &log);
    CHECK(pool != NULL);
    CHECK(!pool_has_waiting(pool));

    CHECK(pool_submit(pool, make_fd()) == 0);
    wait_running(&log, 1);              /* 일꾼이 첫 연결을 꺼내 감 */
    CHECK(!pool_has_waiting(pool));
    CHECK(pool_submit(pool, make_fd()) == 0);
    CHECK(pool_has_waiting(pool));      /* 큐에 하나 대기 */

    int extra = make_fd();
    CHECK(pool_submit(pool, extra) == -1);
    close(extra);                       /* 거절되면 부른 쪽이 닫는다 */

    release_jobs(&log);
    pool_destroy(pool);
    CHECK(log.handled == 2);
    destroy_log(&log);
}

/* 일꾼이 안 끝나면 pool_stop은 정한 시간만 기다리고 -1 (N4 강제 종료 판단) */
static void test_n4_stop_times_out_when_worker_busy(void)
{
    JobLog log;
    init_log(&log, false);
    WorkerPool *pool = pool_create(1, 1, record_job, &log);
    CHECK(pool != NULL);
    CHECK(pool_submit(pool, make_fd()) == 0);
    wait_running(&log, 1);
    CHECK(!pool_is_stopping(pool));

    struct timespec start;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    CHECK(pool_stop(pool, 1) == -1);
    clock_gettime(CLOCK_MONOTONIC, &end);
    CHECK(end.tv_sec - start.tv_sec >= 1 && end.tv_sec - start.tv_sec < 3);
    CHECK(pool_is_stopping(pool));
    CHECK(pool_submit(pool, 0) == -1);      /* 종료 중에는 안 받는다 */

    release_jobs(&log);
    CHECK(pool_stop(pool, 1) == 0);         /* 풀리면 시간 안에 끝난다 */
    pool_destroy(pool);
    CHECK(log.handled == 1);
    destroy_log(&log);
}

/* 종료해도 큐에 이미 들어온 연결은 버리지 않고 처리한다 (N4) */
static void test_n4_stop_finishes_queued_connections(void)
{
    JobLog log;
    init_log(&log, false);
    WorkerPool *pool = pool_create(1, 3, record_job, &log);
    CHECK(pool != NULL);
    for (int i = 0; i < 4; i++) {           /* 처리 중 1 + 대기 3 */
        CHECK(pool_submit(pool, make_fd()) == 0);
        if (i == 0) {
            wait_running(&log, 1);
        }
    }
    release_jobs(&log);
    CHECK(pool_stop(pool, 2) == 0);
    CHECK(log.handled == 4);
    pool_destroy(pool);
    destroy_log(&log);
}

int main(void)
{
    RUN(test_n1_create_rejects_zero_sizes);
    RUN(test_n1_every_submitted_connection_is_handled);
    RUN(test_n1_workers_run_in_parallel);
    RUN(test_n2_submit_fails_when_queue_full);
    RUN(test_n4_stop_times_out_when_worker_busy);
    RUN(test_n4_stop_finishes_queued_connections);
    printf("pool: %d checks, %d failed\n", test_checks, test_failures);
    return TEST_EXIT_CODE();
}
