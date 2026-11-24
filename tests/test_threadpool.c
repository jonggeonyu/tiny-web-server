/*
 * threadpool 테스트: 일이 빠짐없이 한 번씩 처리되는지, 정말 동시에 도는지,
 * 가득 차면 거절하는지, 종료 시 남은 일을 처리하는지
 */
#include "test.h"

#include "netio.h"
#include "threadpool.h"

#include <pthread.h>
#include <stdint.h>
#include <time.h>

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* ---- 공유 기록 (mutex 로 보호) ---- */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int seen[1000];       /* seen[fd] = 그 일이 처리된 횟수 */
static int done_count;
static int running_now, running_max; /* 동시에 실행 중인 일의 수와 그 최댓값 */
static int bad_worker_id;

static void reset(void)
{
    pthread_mutex_lock(&mu);
    for (int i = 0; i < 1000; i++)
        seen[i] = 0;
    done_count = running_now = running_max = bad_worker_id = 0;
    pthread_mutex_unlock(&mu);
}

/* 일: arg 만큼(ms) 자면서 기록 */
static void job(int fd, int tag, void *arg)
{
    (void)tag;
    long ms = (long)(intptr_t)arg;
    int wid = threadpool_worker_id();

    pthread_mutex_lock(&mu);
    if (wid < 1)
        bad_worker_id++;
    running_now++;
    if (running_now > running_max)
        running_max = running_now;
    pthread_mutex_unlock(&mu);

    if (ms > 0)
        sleep_ms(ms);

    pthread_mutex_lock(&mu);
    running_now--;
    seen[fd]++;
    done_count++;
    pthread_mutex_unlock(&mu);
}

static int get_done(void)
{
    pthread_mutex_lock(&mu);
    int d = done_count;
    pthread_mutex_unlock(&mu);
    return d;
}

static void test_all_jobs_once(void)
{
    reset();
    threadpool *tp = threadpool_create(8, 1000, job, (void *)(intptr_t)0);
    CHECK(tp != NULL);
    for (int i = 0; i < 1000; i++)
        CHECK_EQ_INT(threadpool_submit(tp, i, 0), 0);
    threadpool_destroy(tp); /* 남은 일 모두 처리 후 종료 */

    CHECK_EQ_INT(done_count, 1000);
    int wrong = 0;
    for (int i = 0; i < 1000; i++)
        if (seen[i] != 1)
            wrong++;
    CHECK_EQ_INT(wrong, 0);       /* 빠짐도, 두 번 처리도 없음 */
    CHECK_EQ_INT(bad_worker_id, 0); /* 일꾼 번호는 1 이상 */
    CHECK_EQ_INT(threadpool_worker_id(), 0); /* 메인 스레드는 일꾼이 아님 */
}

static void test_runs_in_parallel(void)
{
    /* 100ms 걸리는 일 8개 + 일꾼 8개 → 한꺼번에 돌면 약 100ms, 하나씩이면 800ms */
    reset();
    threadpool *tp = threadpool_create(8, 16, job, (void *)(intptr_t)100);
    long long t0 = net_now_ms();
    for (int i = 0; i < 8; i++)
        threadpool_submit(tp, i, 0);
    while (get_done() < 8)
        sleep_ms(5);
    long long dt = net_now_ms() - t0;
    threadpool_destroy(tp);
    CHECK_EQ_INT(running_max, 8);
    CHECK(dt < 400);
}

static void test_queue_full_rejects(void)
{
    /* 일꾼 1개가 200ms 짜리 일을 하는 동안 대기열 2칸 → 3번째부터는 거절 */
    reset();
    threadpool *tp = threadpool_create(1, 2, job, (void *)(intptr_t)200);
    CHECK_EQ_INT(threadpool_submit(tp, 1, 0), 0);
    sleep_ms(50); /* 일꾼이 1번을 가져가 처리 중 → 대기열은 빈 상태 */
    CHECK_EQ_INT(threadpool_submit(tp, 2, 0), 0);
    CHECK_EQ_INT(threadpool_submit(tp, 3, 0), 0);
    CHECK_EQ_INT(threadpool_submit(tp, 4, 0), -1); /* 가득 참: 기다리지 않고 바로 실패 */
    CHECK_EQ_INT(threadpool_submit(tp, 5, 0), -1);
    threadpool_destroy(tp);
    CHECK_EQ_INT(done_count, 3);
    CHECK_EQ_INT(seen[4], 0);
}

static void test_bad_args(void)
{
    CHECK(threadpool_create(0, 10, job, NULL) == NULL);
    CHECK(threadpool_create(4, 0, job, NULL) == NULL);
}

int main(void)
{
    RUN_TEST(test_all_jobs_once);
    RUN_TEST(test_runs_in_parallel);
    RUN_TEST(test_queue_full_rejects);
    RUN_TEST(test_bad_args);
    return TEST_REPORT();
}
