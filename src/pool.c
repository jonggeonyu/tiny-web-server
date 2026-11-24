/* 스레드 풀과 연결 큐 (N1, N2) */
#include "pool.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

/* 공개 함수가 필드를 쓰므로 구조체를 맨 위에 둔다. 헤더에는 이름만 있다.
 * 여러 스레드가 같이 쓰는 값에는 지키는 락을 적었다. */
struct WorkerPool {
    pthread_mutex_t lock;
    pthread_cond_t not_empty;   /* 큐에 연결이 생겼거나 종료하라는 신호 */
    pthread_cond_t all_exited;  /* 마지막 일꾼이 끝났다는 신호 (MONOTONIC) */
    bool is_lock_ready;         /* 만들 때만 쓴다. 실패한 락은 destroy 금지 */
    bool is_not_empty_ready;
    bool is_all_exited_ready;

    int *queue;                 /* lock: 원형 큐. queue[head]가 다음 차례 */
    size_t capacity;
    size_t head;                /* lock */
    size_t count;               /* lock: 큐에 있는 연결 수 */
    bool is_stopping;           /* lock: true면 더 안 받고 남은 것만 처리 */
    size_t running_count;       /* lock: 아직 끝나지 않은 일꾼 수 */

    pthread_t *workers;
    size_t worker_count;        /* 실제로 시작한 일꾼 수 */
    PoolJob job;
    void *context;
};

static int init_pool(WorkerPool *pool, size_t queue_capacity,
                     size_t worker_count);
static int start_workers(WorkerPool *pool, size_t worker_count);
static void *run_worker(void *argument);
static bool take_connection(WorkerPool *pool, int *connfd);
static int init_conditions(WorkerPool *pool);
static void mark_worker_exited(WorkerPool *pool);
static void request_stop(WorkerPool *pool);
static int wait_workers_exited(WorkerPool *pool, int timeout_seconds);
static void join_workers(WorkerPool *pool);
static void free_pool(WorkerPool *pool);
static void lock_pool(WorkerPool *pool);
static void unlock_pool(WorkerPool *pool);

/* ---------------------------------------------------------------------------
 * 공개: 헤더에 선언된 함수. 모양을 바꾸려면 팀과 먼저 정한다.
 * ------------------------------------------------------------------------- */

WorkerPool *pool_create(size_t worker_count, size_t queue_capacity,
                        PoolJob job, void *context)
{
    if (worker_count == 0 || queue_capacity == 0 || job == NULL) {
        errno = EINVAL;
        return NULL;
    }
    WorkerPool *pool = calloc(1, sizeof(*pool));
    if (pool == NULL) {
        return NULL;
    }
    pool->job = job;
    pool->context = context;
    if (init_pool(pool, queue_capacity, worker_count) < 0) {
        goto cleanup;
    }
    if (start_workers(pool, worker_count) < 0) {
        goto cleanup;
    }
    return pool;

cleanup:
    free_pool(pool);
    return NULL;
}

int pool_submit(WorkerPool *pool, int connfd)
{
    lock_pool(pool);
    if (pool->is_stopping || pool->count == pool->capacity) {
        unlock_pool(pool);
        return -1;
    }
    size_t tail = (pool->head + pool->count) % pool->capacity;
    pool->queue[tail] = connfd;
    pool->count++;
    /* 연결이 하나 늘었으니 잠든 일꾼 하나만 깨운다.
     * 기본 조건 변수의 signal은 실패하지 않는다 */
    (void)pthread_cond_signal(&pool->not_empty);
    unlock_pool(pool);
    return 0;
}

bool pool_has_waiting(WorkerPool *pool)
{
    lock_pool(pool);
    bool has_waiting = pool->count > 0;
    unlock_pool(pool);
    return has_waiting;
}

bool pool_is_stopping(WorkerPool *pool)
{
    lock_pool(pool);
    bool is_stopping = pool->is_stopping;
    unlock_pool(pool);
    return is_stopping;
}

int pool_stop(WorkerPool *pool, int timeout_seconds)
{
    request_stop(pool);
    if (wait_workers_exited(pool, timeout_seconds) < 0) {
        return -1;
    }
    join_workers(pool);
    return 0;
}

void pool_destroy(WorkerPool *pool)
{
    if (pool == NULL) {
        return;
    }
    (void)pool_stop(pool, 0);   /* 시간 제한 0은 끝없이 기다려 항상 0 */
    free_pool(pool);
}

/* ---------------------------------------------------------------------------
 * 도우미: 이 파일 안에서만 쓴다. 전부 static.
 * ------------------------------------------------------------------------- */

/* 큐와 일꾼 목록 메모리, 락, 조건 변수를 준비한다.
 * 크기는 서버 설정(-t, -q)으로 정해지고 클라이언트 값과 상관없다.
 * 반환: 0, 실패 -1 (errno에 원인) */
static int init_pool(WorkerPool *pool, size_t queue_capacity,
                     size_t worker_count)
{
    pool->queue = calloc(queue_capacity, sizeof(*pool->queue));
    pool->workers = calloc(worker_count, sizeof(*pool->workers));
    if (pool->queue == NULL || pool->workers == NULL) {
        return -1;
    }
    pool->capacity = queue_capacity;

    /* pthread 함수는 errno 대신 에러 번호를 반환값으로 돌려준다 */
    int error = pthread_mutex_init(&pool->lock, NULL);
    if (error != 0) {
        errno = error;
        return -1;
    }
    pool->is_lock_ready = true;
    return init_conditions(pool);
}

/* 조건 변수 두 개를 만든다. all_exited는 시간 제한 기다리기에 쓰므로
 * 시계를 바꿔도 영향이 없는 CLOCK_MONOTONIC으로 맞춘다.
 * 반환: 0, 실패 -1 (errno에 원인) */
static int init_conditions(WorkerPool *pool)
{
    pthread_condattr_t attributes;

    int error = pthread_cond_init(&pool->not_empty, NULL);
    if (error != 0) {
        errno = error;
        return -1;
    }
    pool->is_not_empty_ready = true;

    error = pthread_condattr_init(&attributes);
    if (error != 0) {
        errno = error;
        return -1;
    }
    error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (error == 0) {
        error = pthread_cond_init(&pool->all_exited, &attributes);
    }
    /* 다 쓴 속성 객체. 초기화에 성공한 객체라 destroy는 실패하지 않는다 */
    (void)pthread_condattr_destroy(&attributes);
    if (error != 0) {
        errno = error;
        return -1;
    }
    pool->is_all_exited_ready = true;
    return 0;
}

/* 일꾼을 worker_count개 만든다. 중간에 실패하면 이미 만든 일꾼을 끝내고
 * -1 (errno에 원인) */
static int start_workers(WorkerPool *pool, size_t worker_count)
{
    for (size_t i = 0; i < worker_count; i++) {
        /* 일꾼은 끝날 때 running_count를 줄이므로 만들기 전에 센다 */
        lock_pool(pool);
        pool->running_count++;
        unlock_pool(pool);
        int error = pthread_create(&pool->workers[i], NULL, run_worker, pool);
        if (error != 0) {
            mark_worker_exited(pool);   /* 못 만든 일꾼은 센 것을 되돌린다 */
            (void)pool_stop(pool, 0);
            errno = error;
            return -1;
        }
        pool->worker_count = i + 1;
    }
    return 0;
}

/* 일꾼 하나의 일생: 연결을 꺼내 처리하기를 종료할 때까지 반복한다.
 * job은 락을 놓은 뒤에 부른다. 락을 쥔 채 I/O를 하면 다른 일꾼이 큐를
 * 못 만져서 한 번에 하나씩만 처리하게 된다 */
static void *run_worker(void *argument)
{
    WorkerPool *pool = argument;
    int connfd;

    while (take_connection(pool, &connfd)) {
        pool->job(connfd, pool->context);
    }
    mark_worker_exited(pool);
    return NULL;
}

/* 큐에서 연결 하나를 꺼낸다. 비어 있으면 생길 때까지 잔다.
 * 반환: 꺼냈으면 true, 종료 중이고 큐도 비었으면 false */
static bool take_connection(WorkerPool *pool, int *connfd)
{
    lock_pool(pool);
    /* if가 아니라 while: 깨어났는데 다른 일꾼이 먼저 가져갔을 수 있고,
     * 이유 없이 깨어나는 경우(spurious wakeup)도 있다.
     * 기본 조건 변수의 wait는 잘못 쓸 때만 실패한다 */
    while (pool->count == 0 && !pool->is_stopping) {
        (void)pthread_cond_wait(&pool->not_empty, &pool->lock);
    }
    if (pool->count == 0) {
        unlock_pool(pool);
        return false;
    }
    *connfd = pool->queue[pool->head];
    pool->head = (pool->head + 1) % pool->capacity;
    pool->count--;
    unlock_pool(pool);
    return true;
}

/* 일꾼 하나가 끝났다고 센다. 마지막 일꾼이면 기다리는 쪽을 깨운다 */
static void mark_worker_exited(WorkerPool *pool)
{
    lock_pool(pool);
    pool->running_count--;
    if (pool->running_count == 0) {
        (void)pthread_cond_broadcast(&pool->all_exited);
    }
    unlock_pool(pool);
}

/* 종료 표시를 하고 잠든 일꾼을 모두 깨운다. 일꾼은 큐에 남은 연결을
 * 다 처리한 뒤 끝난다 */
static void request_stop(WorkerPool *pool)
{
    lock_pool(pool);
    pool->is_stopping = true;
    (void)pthread_cond_broadcast(&pool->not_empty);
    unlock_pool(pool);
}

/* 모든 일꾼이 끝날 때까지 최대 timeout_seconds초 기다린다 (0이면 끝없이).
 * 반환: 모두 끝남 0, 시간 초과 -1 */
static int wait_workers_exited(WorkerPool *pool, int timeout_seconds)
{
    struct timespec deadline;

    /* CLOCK_MONOTONIC은 POSIX 필수 시계라 실패하지 않는다 */
    (void)clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_seconds;

    lock_pool(pool);
    int error = 0;
    while (pool->running_count > 0 && error != ETIMEDOUT) {
        if (timeout_seconds == 0) {
            (void)pthread_cond_wait(&pool->all_exited, &pool->lock);
            continue;
        }
        error = pthread_cond_timedwait(&pool->all_exited, &pool->lock,
                                       &deadline);
    }
    bool is_done = pool->running_count == 0;
    unlock_pool(pool);
    return is_done ? 0 : -1;
}

/* 끝난 일꾼 스레드를 회수한다. 우리가 만든, 아직 join하지 않은 스레드라
 * 실패하지 않는다 */
static void join_workers(WorkerPool *pool)
{
    for (size_t i = 0; i < pool->worker_count; i++) {
        (void)pthread_join(pool->workers[i], NULL);
    }
    pool->worker_count = 0;
}

static void free_pool(WorkerPool *pool)
{
    /* 아래 destroy는 쓰는 스레드가 없는 상태라 실패하지 않는다 */
    if (pool->is_all_exited_ready) {
        (void)pthread_cond_destroy(&pool->all_exited);
    }
    if (pool->is_not_empty_ready) {
        (void)pthread_cond_destroy(&pool->not_empty);
    }
    if (pool->is_lock_ready) {
        (void)pthread_mutex_destroy(&pool->lock);
    }
    free(pool->queue);
    free(pool->workers);
    free(pool);
}

/* 기본 뮤텍스의 lock/unlock은 잘못 쓸 때(초기화 안 함, 남의 락 풀기)만
 * 실패한다. 이 파일은 항상 짝을 맞춰 쓰므로 결과를 보지 않는다 */
static void lock_pool(WorkerPool *pool)
{
    (void)pthread_mutex_lock(&pool->lock);
}

static void unlock_pool(WorkerPool *pool)
{
    (void)pthread_mutex_unlock(&pool->lock);
}
