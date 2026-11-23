#include "threadpool.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

/* 일꾼 스레드의 스택 크기. 기본값(보통 8MB)은 스레드마다 가상 메모리를 크게 잡는다.
 * 우리 코드가 스택에 올리는 것은 많아야 100KB 정도(파일 조각 64KB 등)라서 1MB 면 충분하다. */
#define WORKER_STACK_SIZE (1024 * 1024)

typedef struct {
    threadpool *tp;
    int id;
} worker_start;

typedef struct {
    int fd;
    int tag;
} tp_item;

struct threadpool {
    /* mutex: 아래 대기열 변수들을 "한 번에 한 스레드만" 만지게 하는 자물쇠 */
    pthread_mutex_t lock;
    /* 조건 변수: "대기열에 일이 생겼다(또는 종료하라)"를 잠든 일꾼에게 알리는 초인종 */
    pthread_cond_t not_empty;

    /* 원형 대기열: queue[head] 가 다음에 꺼낼 일. 끝에 닿으면 0 번 칸으로 돌아간다 */
    tp_item *queue;
    int cap;
    int head;
    int count;
    int shutdown; /* 1 이면 더 받지 않고, 남은 일을 처리한 뒤 일꾼 종료 */
    int busy;     /* 지금 일하는 중인 일꾼 수 */

    pthread_t *threads;
    worker_start *starts; /* 일꾼에게 넘긴 시작 정보 (풀이 끝날 때까지 살아 있어야 함) */
    int nthreads;
    tp_job_fn fn;
    void *arg;
};

/* 스레드마다 따로 갖는 변수 (_Thread_local): 같은 이름이지만 일꾼마다 값이 다르다 */
static _Thread_local int tl_worker_id = 0;

int threadpool_worker_id(void)
{
    return tl_worker_id;
}

static void *worker_main(void *p)
{
    worker_start *ws = p;
    threadpool *tp = ws->tp;
    tl_worker_id = ws->id;

    for (;;) {
        pthread_mutex_lock(&tp->lock);
        /* 일이 없으면 잠든다. pthread_cond_wait 는 "자물쇠를 풀고 잠들기"를 한 번에 하고,
         * 깨어나면 자물쇠를 다시 잡은 상태로 돌아온다.
         * if 가 아니라 while 인 이유: 깨어났는데 다른 일꾼이 먼저 일을 가져갔을 수 있고,
         * 이유 없이 깨어나는 경우(spurious wakeup)도 있기 때문 */
        while (tp->count == 0 && !tp->shutdown)
            pthread_cond_wait(&tp->not_empty, &tp->lock);

        if (tp->count == 0) { /* 여기 왔다면 shutdown 이고 남은 일도 없음 */
            pthread_mutex_unlock(&tp->lock);
            break;
        }

        tp_item item = tp->queue[tp->head];
        tp->head = (tp->head + 1) % tp->cap;
        tp->count--;
        tp->busy++;
        pthread_mutex_unlock(&tp->lock);

        /* 실제 일은 자물쇠를 푼 뒤에 한다. 쥔 채로 하면 다른 일꾼이 대기열을 못 만져서
         * 사실상 한 번에 하나씩만 처리하게 된다 */
        tp->fn(item.fd, item.tag, tp->arg);

        pthread_mutex_lock(&tp->lock);
        tp->busy--;
        pthread_mutex_unlock(&tp->lock);
    }
    return NULL;
}

int threadpool_submit(threadpool *tp, int fd, int tag)
{
    pthread_mutex_lock(&tp->lock);
    if (tp->shutdown || tp->count == tp->cap) {
        pthread_mutex_unlock(&tp->lock);
        return -1; /* 가득 참: 기다리지 않는다 → 호출자가 503 */
    }
    tp->queue[(tp->head + tp->count) % tp->cap] = (tp_item){fd, tag};
    tp->count++;
    /* 잠든 일꾼 "하나"를 깨운다 (일이 하나 늘었으니 하나면 충분) */
    pthread_cond_signal(&tp->not_empty);
    pthread_mutex_unlock(&tp->lock);
    return 0;
}

void threadpool_stats(threadpool *tp, int *busy, int *queued)
{
    pthread_mutex_lock(&tp->lock);
    *busy = tp->busy;
    *queued = tp->count;
    pthread_mutex_unlock(&tp->lock);
}

static void free_pool(threadpool *tp)
{
    pthread_mutex_destroy(&tp->lock);
    pthread_cond_destroy(&tp->not_empty);
    free(tp->queue);
    free(tp->threads);
    free(tp->starts);
    free(tp);
}

void threadpool_destroy(threadpool *tp)
{
    if (!tp)
        return;
    pthread_mutex_lock(&tp->lock);
    tp->shutdown = 1;
    /* 잠든 일꾼 "모두"를 깨운다: 남은 일을 처리하거나, 일이 없으면 종료하도록 */
    pthread_cond_broadcast(&tp->not_empty);
    pthread_mutex_unlock(&tp->lock);

    for (int i = 0; i < tp->nthreads; i++)
        pthread_join(tp->threads[i], NULL); /* 일꾼이 끝날 때까지 기다린다 */
    free_pool(tp);
}

threadpool *threadpool_create(int nthreads, int queue_cap, tp_job_fn fn, void *arg)
{
    if (nthreads < 1 || queue_cap < 1)
        return NULL;

    threadpool *tp = calloc(1, sizeof(*tp));
    if (!tp)
        return NULL;
    tp->queue = calloc((size_t)queue_cap, sizeof(tp_item));
    tp->threads = calloc((size_t)nthreads, sizeof(pthread_t));
    tp->starts = calloc((size_t)nthreads, sizeof(worker_start));
    pthread_mutex_init(&tp->lock, NULL);
    pthread_cond_init(&tp->not_empty, NULL);
    if (!tp->queue || !tp->threads || !tp->starts) {
        free_pool(tp);
        return NULL;
    }
    tp->cap = queue_cap;
    tp->fn = fn;
    tp->arg = arg;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, WORKER_STACK_SIZE);

    for (int i = 0; i < nthreads; i++) {
        tp->starts[i].tp = tp;
        tp->starts[i].id = i + 1;
        if (pthread_create(&tp->threads[i], &attr, worker_main, &tp->starts[i]) != 0) {
            fprintf(stderr, "일꾼 스레드 생성 실패 (%d번째)\n", i + 1);
            pthread_attr_destroy(&attr);
            threadpool_destroy(tp); /* 이미 만든 일꾼(tp->nthreads 개)만 정리 */
            return NULL;
        }
        tp->nthreads = i + 1;
    }
    pthread_attr_destroy(&attr);
    return tp;
}
