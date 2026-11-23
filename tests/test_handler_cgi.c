/*
 * handler_cgi 테스트: 임시 디렉토리에 진짜 스크립트를 만들어 실행한다.
 * 교착 방지, 시간 초과(504), 좀비 회수, fd 누출, 502, 403/404 를 확인한다.
 */
#include "test.h"

#include "handler_cgi.h"
#include "netio.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char base[256];
static server_config cfg;
static http_request req;
static char *got;            /* 응답 전체 (큰 출력 테스트 때문에 동적으로) */
static size_t got_len;

/* ---------------- 준비 ---------------- */

static void write_script(const char *name, const char *content, mode_t mode)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/cgi/%s", base, name);
    FILE *f = fopen(p, "w");
    CHECK(f != NULL);
    if (!f)
        return;
    fputs(content, f);
    fclose(f);
    chmod(p, mode);
}

static void setup(void)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(base, sizeof(base), "%s/tiny-cgi-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    CHECK(mkdtemp(base) != NULL);
    char p[512];
    snprintf(p, sizeof(p), "%s/cgi", base);
    CHECK(mkdir(p, 0755) == 0);
    snprintf(p, sizeof(p), "%s/outside.sh", base);
    FILE *f = fopen(p, "w");
    if (f) {
        fputs("#!/bin/sh\nprintf 'Content-Type: text/plain\\r\\n\\r\\nOUTSIDE'\n", f);
        fclose(f);
    }
    chmod(p, 0755);

    write_script("hello.sh",
                 "#!/bin/sh\nprintf 'Content-Type: text/plain\\r\\nX-Test: yes\\r\\n\\r\\n'\n"
                 "printf 'hi %s %s %s' \"$REQUEST_METHOD\" \"$QUERY_STRING\" \"$PATH_INFO\"\n",
                 0755);
    write_script("cat.sh", "#!/bin/sh\nprintf 'Content-Type: application/octet-stream\\r\\n\\r\\n'\ncat\n",
                 0755);
    /* 4KB 씩 읽고 바로 쓰는 복사기. (주의: 일부 cat 구현은 splice 로 파이프끼리 직접 옮겨서
     * 순진한 서버 구현에서도 교착이 안 생긴다 → 교착 테스트에는 평범한 dd 를 쓴다) */
    write_script("copy.sh",
                 "#!/bin/sh\nprintf 'Content-Type: application/octet-stream\\r\\n\\r\\n'\n"
                 "dd bs=4096 2>/dev/null\n", 0755);
    /* 본문을 하나도 읽지 않고 큰 출력을 내는 CGI (서버가 본문 넣기에 막히면 안 됨) */
    write_script("noread.sh",
                 "#!/bin/sh\nprintf 'Content-Type: text/plain\\r\\n\\r\\n'\n"
                 "head -c 300000 /dev/zero\n", 0755);
    write_script("forever.sh",
                 "#!/bin/sh\necho $$ > pid.txt\nprintf 'Content-Type: text/plain\\r\\n\\r\\n'\n"
                 "while :; do sleep 1; done\n", 0755);
    /* 출력은 닫고 끝나지 않는 CGI */
    write_script("closeout.sh",
                 "#!/bin/sh\nprintf 'Content-Type: text/plain\\r\\n\\r\\nbye'\nexec >&-\nsleep 30\n",
                 0755);
    write_script("crash.sh", "#!/bin/sh\nprintf 'Content-Type: text/plain\\r\\n\\r\\nx'\nkill -SEGV $$\n",
                 0755);
    write_script("exit1.sh", "#!/bin/sh\nprintf 'Content-Type: text/plain\\r\\n\\r\\nx'\nexit 1\n", 0755);
    write_script("bad.sh", "#!/bin/sh\necho no headers here\n", 0755);
    write_script("badinterp.sh", "#!/no/such/interpreter\n", 0755);
    write_script("huge.sh", "#!/bin/sh\nprintf 'Content-Type: text/plain\\r\\n\\r\\n'\n"
                            "head -c 9000000 /dev/zero\n", 0755);
    write_script("fds.sh", "#!/bin/sh\nprintf 'Content-Type: text/plain\\r\\n\\r\\n'\n"
                           "ls /proc/self/fd\n", 0755);
    write_script("noexec.sh", "#!/bin/sh\necho SHOULD NOT RUN\n", 0644);
    write_script("status.sh", "#!/bin/sh\nprintf 'Status: 404 Gone\\r\\nContent-Type: text/plain\\r\\n\\r\\nx'\n",
                 0755);
    write_script("pwd.sh", "#!/bin/sh\nprintf 'Content-Type: text/plain\\r\\n\\r\\n'\npwd\n", 0755);
    write_script(".hidden.sh", "#!/bin/sh\necho x\n", 0755);
    snprintf(p, sizeof(p), "%s/cgi/link-out.sh", base);
    CHECK(symlink("../outside.sh", p) == 0);
    snprintf(p, sizeof(p), "%s/cgi/subdir", base);
    CHECK(mkdir(p, 0755) == 0);

    snprintf(p, sizeof(p), "%s/cgi", base);
    CHECK(realpath(p, cfg.cgi_dir) != NULL);
    cfg.port = 8080;
    cfg.cgi_timeout_ms = 3000;
}

static void teardown(void)
{
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
    CHECK(system(cmd) == 0);
}

/* ---------------- 실행 도우미 ---------------- */

typedef struct {
    int fd;
} reader_arg;

/* 서버가 응답을 쓰는 동안 동시에 읽어 주는 스레드 (큰 응답에서 소켓 버퍼가 차도 막히지 않게) */
static void *reader_thread(void *arg)
{
    int fd = ((reader_arg *)arg)->fd;
    size_t cap = 1 << 20;
    got = malloc(cap);
    got_len = 0;
    for (;;) {
        if (got_len + 65536 + 1 > cap) {
            cap *= 2;
            got = realloc(got, cap);
        }
        ssize_t n = read(fd, got + got_len, 65536);
        if (n <= 0)
            break;
        got_len += (size_t)n;
    }
    got[got_len] = '\0';
    return NULL;
}

/* target 으로 요청을 만들어 cgi_serve 실행. body 가 있으면 POST */
static http_writer call(const char *target, const char *body, size_t body_len)
{
    char raw[512];
    snprintf(raw, sizeof(raw), "%s %s HTTP/1.1\r\nHost: test.local:8080\r\n\r\n",
             body ? "POST" : "GET", target);
    CHECK_EQ_INT(http_parse_request(raw, strlen(raw), &req), HTTP_PARSE_OK);

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    reader_arg ra = {sv[1]};
    pthread_t th;
    free(got); /* 이전 응답 버퍼는 읽기 스레드를 시작하기 "전에" 정리 (스레드가 새로 할당하므로) */
    got = NULL;
    pthread_create(&th, NULL, reader_thread, &ra);

    http_writer w;
    writer_init(&w, sv[0]);
    http_body hb = {(char *)body, body_len};
    request_ctx ctx = {
        .req = &req, .path = req.path, .body = &hb, .cfg = &cfg,
        .remote_addr = "127.0.0.1", .remote_port = 1234,
    };
    cgi_serve(&ctx, &w);

    close(sv[0]);
    pthread_join(th, NULL);
    close(sv[1]);
    return w;
}

static const char *body_of(void)
{
    const char *p = strstr(got, "\r\n\r\n");
    return p ? p + 4 : "";
}

/* ---------------- 테스트 ---------------- */

static void test_hello(void)
{
    http_writer w = call("/cgi-bin/hello.sh/extra?x=1", NULL, 0);
    CHECK_EQ_INT(w.status, 200);
    CHECK(strstr(got, "X-Test: yes\r\n") != NULL);
    const char *expect = "hi GET x=1 /extra";
    char cl[64];
    snprintf(cl, sizeof(cl), "Content-Length: %zu\r\n", strlen(expect)); /* 길이를 손으로 세지 않음 */
    CHECK(strstr(got, cl) != NULL);
    CHECK_EQ_STR(body_of(), expect);
}

static void test_runs_in_script_dir(void)
{
    http_writer w = call("/cgi-bin/pwd.sh", NULL, 0);
    CHECK_EQ_INT(w.status, 200);
    char expect[PATH_MAX + 2];
    snprintf(expect, sizeof(expect), "%s\n", cfg.cgi_dir);
    CHECK_EQ_STR(body_of(), expect);
}

static void test_post_body_to_stdin(void)
{
    http_writer w = call("/cgi-bin/cat.sh", "hello cgi", 9);
    CHECK_EQ_INT(w.status, 200);
    CHECK_EQ_STR(body_of(), "hello cgi");
}

static void test_no_deadlock_big_in_big_out(void)
{
    /* 1MB 를 넣는 동안 CGI(dd)도 1MB 를 출력한다.
     * 파이프 버퍼(보통 64KB)보다 훨씬 크므로, "다 넣고 나서 읽기" 방식이면 서로 기다리다 멈춘다.
     * 멈추면 시간 제한(3초)에 걸려 504 가 나오므로, 200 + 내용 일치면 교착이 없었다는 뜻 */
    size_t n = 1024 * 1024;
    char *big = malloc(n);
    for (size_t i = 0; i < n; i++)
        big[i] = (char)(i * 31 % 251);
    long long t0 = net_now_ms();
    http_writer w = call("/cgi-bin/copy.sh", big, n);
    long long dt = net_now_ms() - t0;
    CHECK_EQ_INT(w.status, 200);
    const char *b = body_of();
    CHECK_EQ_INT(got_len - (size_t)(b - got), n);
    CHECK(got_len - (size_t)(b - got) == n && memcmp(b, big, n) == 0);
    CHECK(dt < 2000);
    free(big);
}

static void test_cgi_ignores_stdin(void)
{
    /* CGI 가 본문을 읽지 않고 끝나도 서버는 멈추지 않고 출력을 전달한다 */
    size_t n = 500000;
    char *big = calloc(1, n);
    http_writer w = call("/cgi-bin/noread.sh", big, n);
    CHECK_EQ_INT(w.status, 200);
    CHECK_EQ_INT(got_len - (size_t)(body_of() - got), 300000);
    free(big);
}

static void test_timeout_504_and_reaped(void)
{
    cfg.cgi_timeout_ms = 300;
    long long t0 = net_now_ms();
    http_writer w = call("/cgi-bin/forever.sh", NULL, 0);
    long long dt = net_now_ms() - t0;
    cfg.cgi_timeout_ms = 3000;
    CHECK_EQ_INT(w.status, 504);
    CHECK(dt >= 250 && dt < 2000);

    /* 스크립트가 남긴 pid 로 확인: 회수까지 끝났으면 kill(pid, 0) 이 ESRCH.
     * (좀비로 남아 있으면 kill(pid, 0) 이 "성공"한다) */
    char p[PATH_MAX + 16];
    snprintf(p, sizeof(p), "%s/pid.txt", cfg.cgi_dir);
    FILE *f = fopen(p, "r");
    int pid = 0;
    if (f) {
        CHECK(fscanf(f, "%d", &pid) == 1);
        fclose(f);
    }
    CHECK(pid > 0);
    errno = 0;
    CHECK_EQ_INT(kill(pid, 0), -1);
    CHECK_EQ_INT(errno, ESRCH);
}

static void test_closed_output_but_running(void)
{
    /* 출력은 다 냈는데 프로세스가 안 끝남 → 시간 제한에 강제 종료 + 504 */
    cfg.cgi_timeout_ms = 300;
    http_writer w = call("/cgi-bin/closeout.sh", NULL, 0);
    cfg.cgi_timeout_ms = 3000;
    CHECK_EQ_INT(w.status, 504);
}

static void test_502_cases(void)
{
    CHECK_EQ_INT(call("/cgi-bin/crash.sh", NULL, 0).status, 502);     /* 시그널로 죽음 */
    CHECK(strstr(got, "partial") == NULL);
    CHECK_EQ_INT(call("/cgi-bin/exit1.sh", NULL, 0).status, 502);     /* 종료 코드 1 */
    CHECK_EQ_INT(call("/cgi-bin/bad.sh", NULL, 0).status, 502);       /* 헤더 없음 */
    CHECK_EQ_INT(call("/cgi-bin/badinterp.sh", NULL, 0).status, 502); /* exec 실패 */
    CHECK_EQ_INT(call("/cgi-bin/huge.sh", NULL, 0).status, 502);      /* 출력 8MB 초과 */
}

static void test_status_passthrough(void)
{
    http_writer w = call("/cgi-bin/status.sh", NULL, 0);
    CHECK_EQ_INT(w.status, 404);
    CHECK(strstr(got, "HTTP/1.1 404 Not Found\r\n") != NULL);
}

static void test_forbidden_and_missing(void)
{
    CHECK_EQ_INT(call("/cgi-bin/noexec.sh", NULL, 0).status, 403);   /* 실행 권한 없음 */
    CHECK(strstr(got, "SHOULD NOT RUN") == NULL);
    CHECK_EQ_INT(call("/cgi-bin/link-out.sh", NULL, 0).status, 403); /* 링크로 밖을 가리킴 */
    CHECK(strstr(got, "OUTSIDE") == NULL);
    CHECK_EQ_INT(call("/cgi-bin/subdir", NULL, 0).status, 403);      /* 디렉토리 */
    CHECK_EQ_INT(call("/cgi-bin/.hidden.sh", NULL, 0).status, 403);  /* 숨김 파일 */
    CHECK_EQ_INT(call("/cgi-bin/", NULL, 0).status, 403);            /* 이름 없음 */
    CHECK_EQ_INT(call("/cgi-bin/nope.sh", NULL, 0).status, 404);
}

static void test_no_fd_leak(void)
{
    if (access("/proc/self/fd", R_OK) != 0) {
        printf("    (/proc 없음: 생략)\n");
        return;
    }
    /* 일부러 CLOEXEC 없는 fd 를 하나 열어 둔다 → 자식 쪽 "보조 방어"가 닫아야 한다 */
    int leak = open("/dev/null", O_RDONLY);
    http_writer w = call("/cgi-bin/fds.sh", NULL, 0);
    close(leak);
    CHECK_EQ_INT(w.status, 200);
    /* ls 의 출력: 0 1 2 와 ls 자신이 연 3 만 있어야 한다 */
    CHECK_EQ_STR(body_of(), "0\n1\n2\n3\n");
}

static void test_client_gone_during_cgi(void)
{
    /* CGI 가 도는 동안 클라이언트가 사라짐 → 응답 전송은 실패하지만,
     * CGI 는 끝까지 회수되고(좀비 없음) 서버(이 함수)는 정상적으로 돌아와야 한다 */
    char raw[] = "GET /cgi-bin/noread.sh HTTP/1.1\r\nHost: t\r\n\r\n";
    CHECK_EQ_INT(http_parse_request(raw, strlen(raw), &req), HTTP_PARSE_OK);
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    close(sv[1]); /* 클라이언트가 이미 끊음 */

    http_writer w;
    writer_init(&w, sv[0]);
    http_body hb = {NULL, 0};
    request_ctx ctx = {
        .req = &req, .path = req.path, .body = &hb, .cfg = &cfg,
        .remote_addr = "127.0.0.1", .remote_port = 1234,
    };
    cgi_serve(&ctx, &w);
    close(sv[0]);

    CHECK_EQ_INT(w.failed, 1); /* 응답을 보내다 실패 (EPIPE) */
    int st;
    errno = 0;
    CHECK_EQ_INT(waitpid(-1, &st, WNOHANG), -1); /* 남은 자식 없음 */
    CHECK_EQ_INT(errno, ECHILD);
}

static void test_no_zombies_left(void)
{
    /* 이 테스트 프로세스의 자식 중 끝났는데 회수 안 된 것이 있으면 waitpid(-1, WNOHANG) 가 그 pid 를 준다 */
    int st;
    pid_t r = waitpid(-1, &st, WNOHANG);
    CHECK(r == -1 && errno == ECHILD); /* "자식이 하나도 없음" = 좀비 0개 */
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    setup();
    RUN_TEST(test_hello);
    RUN_TEST(test_runs_in_script_dir);
    RUN_TEST(test_post_body_to_stdin);
    RUN_TEST(test_no_deadlock_big_in_big_out);
    RUN_TEST(test_cgi_ignores_stdin);
    RUN_TEST(test_timeout_504_and_reaped);
    RUN_TEST(test_closed_output_but_running);
    RUN_TEST(test_502_cases);
    RUN_TEST(test_status_passthrough);
    RUN_TEST(test_forbidden_and_missing);
    RUN_TEST(test_no_fd_leak);
    RUN_TEST(test_client_gone_during_cgi);
    RUN_TEST(test_no_zombies_left);
    teardown();
    free(got);
    return TEST_REPORT();
}
