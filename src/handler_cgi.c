#include "handler_cgi.h"

#include "cgi.h"
#include "fdutil.h"
#include "netio.h"
#include "path.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CGI_PREFIX "/cgi-bin/"
#define CGI_PREFIX_LEN 9

/* 자식에서 한 번 더 닫아 볼 fd 범위의 상한 CHILD_CLOSE_FD_MAX 는 config.h 에 있다
 * (서버가 동시에 여는 fd 수가 그보다 작도록 함께 관리하기 위해) */

typedef enum {
    RUN_OK,
    RUN_TIMEOUT,      /* 504 */
    RUN_SPAWN_FAIL,   /* 500: pipe/fork 실패 (서버 쪽 문제) */
    RUN_OUTPUT_LIMIT, /* 502: 출력이 너무 큼 */
    RUN_IO_ERROR      /* 502 */
} run_result;

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------------ */
/* 1) 스크립트 찾기                                                    */
/* ------------------------------------------------------------------ */

/* "/cgi-bin/hello.sh/a/b" → name="hello.sh", path_info="/a/b"
 * real 에 실제 경로. 반환: 0 성공, 그 밖에는 응답할 상태 코드 */
static int find_script(const request_ctx *ctx, char *name, size_t name_cap,
                       const char **path_info, char *real, http_writer *w)
{
    const char *cgi_dir = ctx->cfg->cgi_dir;
    if (cgi_dir[0] == '\0') {
        w->note = "CGI 디렉토리가 설정되지 않음";
        return 404;
    }

    const char *p = ctx->path;
    if (strncmp(p, CGI_PREFIX, CGI_PREFIX_LEN) != 0 || p[CGI_PREFIX_LEN] == '\0') {
        w->note = "스크립트 이름 없음";
        return 403; /* "/cgi-bin" 또는 "/cgi-bin/" : 목록은 보여주지 않는다 */
    }
    const char *start = p + CGI_PREFIX_LEN;
    size_t len = strcspn(start, "/");
    if (len >= name_cap)
        return 414;
    memcpy(name, start, len);
    name[len] = '\0';
    *path_info = start + len;

    if (name[0] == '.') {
        w->note = "숨김 파일은 실행하지 않음";
        return 403;
    }

    char joined[PATH_MAX];
    int n = snprintf(joined, sizeof(joined), "%s/%s", cgi_dir, name);
    if (n < 0 || (size_t)n >= sizeof(joined))
        return 414;

    /* 정적 파일과 같은 방식: 진짜 위치를 구하고, cgi_dir 안쪽인지 확인 */
    if (realpath(joined, real) == NULL) {
        if (errno == ENOENT || errno == ENOTDIR)
            return 404;
        w->note = "스크립트 경로 접근 불가";
        return (errno == EACCES || errno == ELOOP) ? 403 : 500;
    }
    if (!path_is_inside(cgi_dir, real) || strcmp(real, cgi_dir) == 0) {
        w->note = "실제 위치가 CGI 디렉토리 밖 (심볼릭 링크)";
        return 403;
    }

    struct stat sb;
    if (stat(real, &sb) < 0)
        return 404;
    if (!S_ISREG(sb.st_mode)) {
        w->note = "일반 파일이 아님";
        return 403;
    }
    if (access(real, X_OK) != 0) {
        w->note = "실행 권한 없음";
        return 403;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 3~5) 프로세스 실행                                                  */
/* ------------------------------------------------------------------ */

/* 자식이 끝나기를 deadline 까지 기다렸다가, 안 끝나면 프로세스 그룹째 강제 종료하고 회수한다.
 * 어떤 경우든 waitpid 로 반드시 회수하므로 좀비가 남지 않는다. 반환: 강제 종료했으면 1 */
static int reap_child(pid_t pid, long long deadline, int *status)
{
    long wait = 1;
    for (;;) {
        pid_t r = waitpid(pid, status, WNOHANG); /* WNOHANG: 아직 안 끝났으면 기다리지 말고 0 */
        if (r == pid)
            return 0;
        if (r < 0 && errno != EINTR)
            return 0; /* 이미 회수됨 등 (정상적으로는 일어나지 않음) */
        if (net_now_ms() >= deadline)
            break;
        sleep_ms(wait);
        if (wait < 50)
            wait *= 2; /* 1, 2, 4 ... 50ms: 금방 끝나는 경우는 빨리, 오래 걸리면 덜 자주 확인 */
    }
    kill(-pid, SIGKILL); /* 음수 pid = 그 프로세스 그룹 전체 (CGI 가 만든 손자 프로세스까지) */
    while (waitpid(pid, status, 0) < 0 && errno == EINTR) {
    }
    return 1;
}

/* 출력 버퍼를 늘린다. 최대 크기에 닿으면 -1 */
static int grow(char **buf, size_t *cap, size_t len)
{
    if (len < *cap)
        return 0;
    if (*cap >= CGI_OUTPUT_MAX)
        return -1;
    size_t ncap = *cap ? *cap * 2 : 16384;
    if (ncap > CGI_OUTPUT_MAX)
        ncap = CGI_OUTPUT_MAX;
    char *nb = realloc(*buf, ncap);
    if (!nb)
        return -1;
    *buf = nb;
    *cap = ncap;
    return 0;
}

static run_result run_cgi(const char *script, const char *dir, char **envp,
                          const http_body *body, int timeout_ms, char **out, size_t *out_len,
                          int *wstatus)
{
    /* pipe: 한쪽 끝에 쓰면 다른 쪽 끝에서 읽을 수 있는 "관". [0]=읽는 쪽, [1]=쓰는 쪽
     *   to_cgi   : 서버 →(본문)→ CGI 의 표준 입력
     *   from_cgi : CGI 의 표준 출력 →(응답)→ 서버 */
    int to_cgi[2], from_cgi[2];
    /* 만들면서 바로 FD_CLOEXEC: 다른 일꾼이 지금 이 순간 fork 해도 이 파이프는 새지 않는다 */
    if (fd_pipe_cloexec(to_cgi) < 0)
        return RUN_SPAWN_FAIL;
    if (fd_pipe_cloexec(from_cgi) < 0) {
        close(to_cgi[0]);
        close(to_cgi[1]);
        return RUN_SPAWN_FAIL;
    }

    char *argv[] = {(char *)script, NULL};

    /* fork: 지금 프로세스를 통째로 복제한다. 부모에게는 자식 pid, 자식에게는 0 이 돌아온다 */
    pid_t pid = fork();
    if (pid < 0) {
        close(to_cgi[0]);
        close(to_cgi[1]);
        close(from_cgi[0]);
        close(from_cgi[1]);
        return RUN_SPAWN_FAIL;
    }

    if (pid == 0) {
        /* ===== 자식 프로세스 =====
         * 멀티스레드 프로그램에서 fork 한 자식은 exec 전까지 "async-signal-safe" 함수만 써야 한다
         * (malloc, printf 금지: 다른 스레드가 잡고 있던 잠금이 풀리지 않은 채 복제됐을 수 있음).
         * 그래서 필요한 문자열(envp, argv, dir)은 모두 fork 전에 준비해 두었다. */
        setpgid(0, 0); /* 새 프로세스 그룹: 나중에 손자까지 한 번에 종료할 수 있게 */

        dup2(to_cgi[0], STDIN_FILENO);    /* 표준 입력 ← 서버가 넣어 주는 본문 */
        dup2(from_cgi[1], STDOUT_FILENO); /* 표준 출력 → 서버가 읽어 갈 응답 */
        /* 표준 에러(2)는 서버의 에러 로그로 그대로 둔다 */

        /* 보조 방어: 0,1,2 외의 fd 를 모두 닫는다 (주 방어는 FD_CLOEXEC) */
        for (int fd = 3; fd < CHILD_CLOSE_FD_MAX; fd++)
            close(fd);

        /* 서버가 바꿔 둔 시그널 설정을 원래대로 (무시 설정은 exec 후에도 남기 때문) */
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        sigaction(SIGPIPE, &sa, NULL);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);

        if (chdir(dir) < 0) /* CGI 규칙: 스크립트가 있는 디렉토리에서 실행 */
            _exit(126);

        /* exec: 이 프로세스의 내용을 CGI 프로그램으로 "갈아 끼운다". 성공하면 돌아오지 않는다 */
        execve(script, argv, envp);
        _exit(127); /* exec 실패 (예: #! 의 인터프리터가 없음) */
    }

    /* ===== 부모 (서버) ===== */
    setpgid(pid, pid); /* 자식과 같은 일을 부모도: 누가 먼저 실행되든 그룹이 확실히 만들어지게 */

    /* 자식이 쓸 끝은 부모에게 필요 없다. 특히 from_cgi[1] 을 안 닫으면
     * "쓰는 쪽이 아직 열려 있다"고 판단되어 EOF 가 영원히 오지 않는다 */
    close(to_cgi[0]);
    close(from_cgi[1]);
    int wfd = to_cgi[1];
    int rfd = from_cgi[0];

    /* 논블로킹: 쓸 공간/읽을 데이터가 없으면 기다리지 않고 바로 돌아오게 → poll 로 둘을 번갈아 처리 */
    net_set_nonblock(wfd);
    net_set_nonblock(rfd);

    size_t body_len = body ? body->len : 0;
    size_t written = 0;
    if (body_len == 0) {
        close(wfd); /* 넣을 본문이 없으면 바로 닫아서 CGI 에게 "입력 끝(EOF)"을 알린다 */
        wfd = -1;
    }

    char *buf = NULL;
    size_t len = 0, cap = 0;
    long long deadline = net_now_ms() + timeout_ms;
    run_result result = RUN_OK;

    /* 4) 교착 방지 루프: "쓸 수 있으면 쓰고, 읽을 게 있으면 읽는다"를 동시에 */
    while (rfd >= 0) {
        long long left = deadline - net_now_ms();
        if (left <= 0) {
            result = RUN_TIMEOUT;
            break;
        }

        struct pollfd pfd[2];
        int n = 0;
        pfd[n++] = (struct pollfd){.fd = rfd, .events = POLLIN};
        int wi = -1;
        if (wfd >= 0) {
            wi = n;
            pfd[n++] = (struct pollfd){.fd = wfd, .events = POLLOUT};
        }

        int pr = poll(pfd, (nfds_t)n, (int)left);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            result = RUN_IO_ERROR;
            break;
        }
        if (pr == 0)
            continue; /* 시간 초과는 위에서 판단 */

        /* (가) 본문 넣기 */
        if (wi >= 0 && pfd[wi].revents) {
            if (pfd[wi].revents & POLLOUT) {
                ssize_t k = write(wfd, body->data + written, body_len - written);
                if (k > 0)
                    written += (size_t)k;
                else if (k < 0 && errno != EAGAIN && errno != EINTR)
                    written = body_len; /* EPIPE: CGI 가 입력을 안 읽고 닫음 → 넣기 중단 (오류 아님) */
            } else {
                written = body_len; /* POLLERR/POLLHUP: 읽는 쪽이 닫힘 */
            }
            if (written >= body_len) {
                close(wfd);
                wfd = -1;
            }
        }

        /* (나) 출력 받기 */
        if (pfd[0].revents) {
            if (grow(&buf, &cap, len) < 0) {
                result = RUN_OUTPUT_LIMIT;
                break;
            }
            ssize_t k = read(rfd, buf + len, cap - len);
            if (k > 0) {
                len += (size_t)k;
            } else if (k == 0) {
                close(rfd); /* EOF: CGI 가 출력을 다 쓰고 닫음 → 루프 끝 */
                rfd = -1;
            } else if (errno != EAGAIN && errno != EINTR) {
                result = RUN_IO_ERROR;
                break;
            }
        }
    }
    if (wfd >= 0)
        close(wfd);
    if (rfd >= 0)
        close(rfd);

    /* 5) 회수. 문제가 있었으면 즉시 강제 종료, 정상이면 남은 시간 동안 끝나기를 기다린다 */
    if (result != RUN_OK)
        kill(-pid, SIGKILL);
    if (reap_child(pid, result == RUN_OK ? deadline : 0, wstatus) && result == RUN_OK)
        result = RUN_TIMEOUT; /* 출력은 닫았지만 프로세스가 안 끝나서 강제 종료함 */

    *out = buf;
    *out_len = len;
    return result;
}

/* ------------------------------------------------------------------ */
/* 전체                                                                */
/* ------------------------------------------------------------------ */

void cgi_serve(const request_ctx *ctx, http_writer *w)
{
    const http_request *req = ctx->req;

    /* 1) 스크립트 찾기 */
    char name[256];
    const char *path_info = "";
    char real[PATH_MAX];
    int st = find_script(ctx, name, sizeof(name), &path_info, real, w);
    if (st != 0) {
        writer_send_error(w, st);
        return;
    }

    /* 2) 환경 변수 + 실행 디렉토리 준비 (fork 전에 전부) */
    char script_name[300];
    snprintf(script_name, sizeof(script_name), CGI_PREFIX "%s", name);

    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", real);
    char *slash = strrchr(dir, '/');
    if (slash)
        *(slash == dir ? slash + 1 : slash) = '\0';

    /* SERVER_NAME: Host 헤더에서 포트를 뺀 부분 */
    char server_name[256] = "localhost";
    if (req->host && req->host[0]) {
        snprintf(server_name, sizeof(server_name), "%s", req->host);
        char *colon = strrchr(server_name, ':');
        if (colon && !strchr(server_name, ']')) /* IPv6 "[::1]:80" 은 그대로 둔다 */
            *colon = '\0';
    }

    cgi_env_info info = {
        .method = req->method,
        .script_name = script_name,
        .script_filename = real,
        .path_info = path_info,
        .query = req->query,
        .server_name = server_name,
        .server_port = ctx->cfg->port,
        .version_minor = req->version_minor,
        .remote_addr = ctx->remote_addr,
        .remote_port = ctx->remote_port,
        .content_length = (ctx->body && ctx->body->data) ? (long long)ctx->body->len : -1,
        .content_type = http_get_header(req, "Content-Type"),
        .headers = req->headers,
        .header_count = req->header_count,
    };
    char **envp = cgi_build_env(&info);
    if (!envp) {
        writer_send_error(w, 500);
        return;
    }

    /* 3~5) 실행 */
    char *out = NULL;
    size_t out_len = 0;
    int wstatus = 0;
    run_result rr = run_cgi(real, dir, envp, ctx->body, ctx->cfg->cgi_timeout_ms, &out,
                            &out_len, &wstatus);
    cgi_free_env(envp);

    /* 6) 결과 판단 */
    switch (rr) {
    case RUN_OK:
        break;
    case RUN_TIMEOUT:
        w->note = "CGI 시간 초과 → 강제 종료";
        writer_send_error(w, 504);
        free(out);
        return;
    case RUN_SPAWN_FAIL:
        w->note = "pipe/fork 실패";
        writer_send_error(w, 500);
        free(out);
        return;
    case RUN_OUTPUT_LIMIT:
        w->note = "CGI 출력이 너무 큼 → 강제 종료";
        writer_send_error(w, 502);
        free(out);
        return;
    case RUN_IO_ERROR:
        w->note = "CGI 와 통신 중 오류";
        writer_send_error(w, 502);
        free(out);
        return;
    }

    if (WIFSIGNALED(wstatus)) {
        snprintf(w->note_buf, sizeof(w->note_buf), "CGI 가 시그널 %d 로 죽음", WTERMSIG(wstatus));
        w->note = w->note_buf;
        writer_send_error(w, 502);
        free(out);
        return;
    }
    if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
        snprintf(w->note_buf, sizeof(w->note_buf), "CGI 종료 코드 %d%s", WEXITSTATUS(wstatus),
                 WEXITSTATUS(wstatus) == 127 ? " (실행 실패)" : "");
        w->note = w->note_buf;
        writer_send_error(w, 502);
        free(out);
        return;
    }

    cgi_response res;
    const char *reason = NULL;
    if (out == NULL || cgi_parse_output(out, out_len, &res, &reason) != 0) {
        w->note = reason ? reason : "CGI 출력이 비어 있음";
        writer_send_error(w, 502);
        free(out);
        return;
    }

    for (int i = 0; i < res.header_count; i++)
        writer_add_header(w, res.headers[i].name, res.headers[i].value);
    if (writer_send_all(w, res.status, res.content_type, res.body, res.body_len) < 0 &&
        w->status == 0) {
        /* 헤더 조립 실패 (너무 길거나 잘못된 값) → 아무것도 안 나갔으므로 502 로 대신 */
        w->extra_count = 0;
        w->note = "CGI 헤더로 응답을 만들 수 없음";
        writer_send_error(w, 502);
    }
    free(out);
}
