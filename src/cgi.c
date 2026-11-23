/* CGI 실행 (F10, N3 CGI 실행 시간 한도) */
#include "cgi.h"
#include "server_limits.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* 환경 변수 문자열과 execve에 넘길 포인터 배열. 요청 하나마다 malloc한다.
 * 내부 구조체지만 cgi_run이 크기를 알아야 해서 도우미 구역 대신 여기 둔다 */
enum { CGI_ENVIRONMENT_COUNT = 5 };

typedef struct {
    char query[MAX_LINE + MAX_CGI_CONTENT_TYPE];
    char method[MAX_CGI_CONTENT_TYPE];
    char content_length[MAX_CGI_CONTENT_TYPE];
    char content_type[MAX_CGI_CONTENT_TYPE];
    char *pointers[CGI_ENVIRONMENT_COUNT + 1];   /* 마지막은 NULL */
} CgiEnvironment;

static int build_environment(const CgiRequest *request,
                             CgiEnvironment *environment);
static int format_variable(char *out, size_t out_size, const char *name,
                           const char *value);
static FILE *write_body_file(const char *body, size_t length);
static void run_child(const char *program, int stdin_fd, int stdout_fd,
                      char *const environment[], long max_fd);
static int collect_output(pid_t pid, int fd, CgiResult *result);
static int read_output(int fd, CgiResult *result,
                       const struct timespec *deadline);
static int wait_child(pid_t pid, const struct timespec *deadline);
static bool is_successful_exit(int wait_status);
static void kill_and_reap(pid_t pid);
static int make_deadline(struct timespec *deadline);
static int remaining_ms(const struct timespec *deadline);
static size_t find_header_end(const char *output, size_t length,
                              size_t *separator_length);
static int find_content_type(const char *headers, char *out,
                             size_t out_size);

/* ---------------------------------------------------------------------------
 * 공개: 헤더에 선언된 함수. 모양을 바꾸려면 팀과 먼저 정한다.
 * ------------------------------------------------------------------------- */

int cgi_run(const char *program, const CgiRequest *request,
            CgiResult *result)
{
    CgiEnvironment *environment = NULL;
    FILE *body_file = NULL;
    int pipe_fds[2] = {-1, -1};
    int status = 500;

    memset(result, 0, sizeof(*result));
    environment = malloc(sizeof(*environment));
    if (environment == NULL || build_environment(request, environment) < 0) {
        fprintf(stderr, "warn: cgi environment for %s failed\n", program);
        goto cleanup;
    }
    body_file = write_body_file(request->body, request->body_length);
    if (body_file == NULL) {
        goto cleanup;
    }
    if (pipe(pipe_fds) < 0) {
        perror("warn: cgi pipe");
        goto cleanup;
    }

    /* fork 뒤 자식에서는 async-signal-safe 함수만 써야 해서
     * (다른 스레드가 malloc 락을 잡고 있었을 수 있다) 필요한 값은 미리 구한다 */
    long max_fd = sysconf(_SC_OPEN_MAX);
    pid_t pid = fork();
    if (pid < 0) {
        perror("warn: cgi fork");
        goto cleanup;
    }
    if (pid == 0) {
        run_child(program, fileno(body_file), pipe_fds[1],
                  environment->pointers, max_fd);
    }

    /* 쓰는 쪽을 닫아야 자식이 끝날 때 읽는 쪽에서 EOF를 본다 */
    (void)close(pipe_fds[1]);   /* 실패해도 할 수 있는 게 없다 */
    pipe_fds[1] = -1;
    status = collect_output(pid, pipe_fds[0], result);

cleanup:
    if (pipe_fds[0] >= 0) {
        (void)close(pipe_fds[0]);   /* 정리 중이라 실패는 무시 */
    }
    if (pipe_fds[1] >= 0) {
        (void)close(pipe_fds[1]);
    }
    if (body_file != NULL) {
        (void)fclose(body_file);    /* 읽기 전용으로 넘긴 임시 파일 */
    }
    free(environment);
    return status;
}

int cgi_parse_output(CgiResult *result)
{
    size_t separator_length = 0;
    char headers[MAX_CGI_HEADER_BYTES];

    size_t header_length = find_header_end(result->output,
                                           result->output_length,
                                           &separator_length);
    if (separator_length == 0 || header_length >= sizeof(headers)) {
        fprintf(stderr, "warn: cgi output has no header end\n");
        return 500;
    }
    memcpy(headers, result->output, header_length);
    headers[header_length] = '\0';

    if (find_content_type(headers, result->content_type,
                          sizeof(result->content_type)) < 0) {
        fprintf(stderr, "warn: cgi output has no Content-Type\n");
        return 500;
    }
    size_t body_start = header_length + separator_length;
    result->body = result->output + body_start;
    result->body_length = result->output_length - body_start;
    return 200;
}

void cgi_free_result(CgiResult *result)
{
    free(result->output);
    result->output = NULL;
    result->output_length = 0;
    result->body = NULL;
    result->body_length = 0;
}

/* ---------------------------------------------------------------------------
 * 도우미: 이 파일 안에서만 쓴다. 전부 static.
 * ------------------------------------------------------------------------- */


/* #!/usr/bin/env python3 같은 스크립트가 인터프리터를 찾을 수 있게 */
static const char CGI_PATH[] = "PATH=/usr/local/bin:/usr/bin:/bin";
static const char CONTENT_TYPE_NAME[] = "Content-Type:";
static const int EXEC_FAILED_EXIT = 127;   /* 셸과 같은 관례 */
static const long MILLIS_PER_SECOND = 1000;
static const long NANOS_PER_MILLI = 1000 * 1000;

/* REQUEST_METHOD, QUERY_STRING, CONTENT_LENGTH, CONTENT_TYPE, PATH.
 * 반환: 0, 값이 버퍼보다 길면 -1 */
static int build_environment(const CgiRequest *request,
                             CgiEnvironment *environment)
{
    const char *content_type = request->content_type;
    if (content_type == NULL) {
        content_type = "";
    }
    if (format_variable(environment->method, sizeof(environment->method),
                        "REQUEST_METHOD=", request->method) < 0
        || format_variable(environment->query, sizeof(environment->query),
                           "QUERY_STRING=", request->query) < 0
        || format_variable(environment->content_type,
                           sizeof(environment->content_type),
                           "CONTENT_TYPE=", content_type) < 0) {
        return -1;
    }
    int length = snprintf(environment->content_length,
                          sizeof(environment->content_length),
                          "CONTENT_LENGTH=%zu", request->body_length);
    if (length < 0 || (size_t)length >= sizeof(environment->content_length)) {
        return -1;
    }
    environment->pointers[0] = environment->method;
    environment->pointers[1] = environment->query;
    environment->pointers[2] = environment->content_length;
    environment->pointers[3] = environment->content_type;
    environment->pointers[4] = (char *)CGI_PATH;
    environment->pointers[CGI_ENVIRONMENT_COUNT] = NULL;
    return 0;
}

/* "NAME=" + value를 out에 쓴다. 반환: 0, 넘치면 -1 */
static int format_variable(char *out, size_t out_size, const char *name,
                           const char *value)
{
    int length = snprintf(out, out_size, "%s%s", name, value);
    if (length < 0 || (size_t)length >= out_size) {
        return -1;
    }
    return 0;
}

/* POST 본문을 임시 파일에 써 두고 처음으로 되감는다. 이 파일이 자식의
 * stdin이 된다. 파이프로 넘기면 본문(최대 1 MB)이 파이프 버퍼(64 KB)보다
 * 커서, 자식이 출력을 쓰다 막히고 서버는 본문을 쓰다 막히는 교착이 생긴다.
 * 반환: 열린 파일, 실패 NULL (원인은 여기서 찍는다) */
static FILE *write_body_file(const char *body, size_t length)
{
    FILE *file = tmpfile();   /* 이름 없이 만들어지고 닫으면 지워진다 */
    if (file == NULL) {
        perror("warn: cgi body tmpfile");
        return NULL;
    }
    if (length > 0 && fwrite(body, 1, length, file) != length) {
        perror("warn: cgi body write");
        (void)fclose(file);
        return NULL;
    }
    if (fflush(file) != 0 || fseek(file, 0, SEEK_SET) != 0) {
        perror("warn: cgi body rewind");
        (void)fclose(file);
        return NULL;
    }
    return file;
}

/* 자식 프로세스: stdin은 본문 파일, stdout은 파이프로 바꾸고 exec한다.
 * 연결 소켓·listen 소켓 같은 서버 fd는 프로그램에 물려주지 않게 다 닫는다.
 * 자기 프로세스 그룹을 만들어서, 시간 초과 때 프로그램이 띄운 자식(셸 스크립트의
 * sleep 등)까지 한 번에 죽일 수 있게 한다.
 * async-signal-safe 함수(setpgid, dup2, close, execve, _exit)만 쓴다.
 * 돌아오지 않는다 */
static void run_child(const char *program, int stdin_fd, int stdout_fd,
                      char *const environment[], long max_fd)
{
    if (setpgid(0, 0) < 0
        || dup2(stdin_fd, STDIN_FILENO) < 0
        || dup2(stdout_fd, STDOUT_FILENO) < 0) {
        _exit(EXEC_FAILED_EXIT);
    }
    for (long fd = STDERR_FILENO + 1; fd < max_fd; fd++) {
        (void)close((int)fd);   /* 대부분은 열려 있지 않은 번호라 실패가 정상 */
    }
    char *const arguments[] = {(char *)program, NULL};
    execve(program, arguments, environment);
    _exit(EXEC_FAILED_EXIT);    /* execve는 실패할 때만 돌아온다 */
}

/* 출력을 끝까지 받고 자식이 정상 종료했는지 본다. 전체가 CGI_TIMEOUT_SEC 안에
 * 끝나야 한다. 반환: 200, 500 */
static int collect_output(pid_t pid, int fd, CgiResult *result)
{
    struct timespec deadline;

    result->output = malloc(MAX_CGI_OUTPUT_BYTES);
    if (result->output == NULL || make_deadline(&deadline) < 0) {
        perror("warn: cgi output buffer");
        kill_and_reap(pid);
        return 500;
    }
    if (read_output(fd, result, &deadline) < 0) {
        kill_and_reap(pid);
        return 500;
    }
    if (wait_child(pid, &deadline) < 0) {
        return 500;
    }
    return cgi_parse_output(result);
}

/* fd에서 EOF까지 읽어 result->output에 쌓는다. 데이터가 없으면 마감까지
 * poll로 기다린다. 반환: 0, 시간 초과·한도 초과·읽기 실패 -1 */
static int read_output(int fd, CgiResult *result,
                       const struct timespec *deadline)
{
    struct pollfd watch = {fd, POLLIN, 0};

    for (;;) {
        int wait_ms = remaining_ms(deadline);
        if (wait_ms <= 0) {
            fprintf(stderr, "warn: cgi timed out after %d s\n",
                    CGI_TIMEOUT_SEC);
            return -1;
        }
        /* poll이 시그널로 끊겨도(EINTR) 남은 시간을 다시 계산해 기다린다 */
        int ready = poll(&watch, 1, wait_ms);
        if (ready < 0 && errno != EINTR) {
            perror("warn: cgi poll");
            return -1;
        }
        if (ready <= 0) {
            continue;
        }
        size_t space = MAX_CGI_OUTPUT_BYTES - result->output_length;
        ssize_t n = read(fd, result->output + result->output_length, space);
        if (n < 0 && errno != EINTR) {
            perror("warn: cgi read");
            return -1;
        }
        if (n == 0) {
            return 0;
        }
        if (n > 0) {
            result->output_length += (size_t)n;
        }
        if (result->output_length == MAX_CGI_OUTPUT_BYTES) {
            fprintf(stderr, "warn: cgi output over %d bytes\n",
                    MAX_CGI_OUTPUT_BYTES);
            return -1;
        }
    }
}

/* 마감까지 자식이 끝나길 기다려 거둔다. 출력을 닫고도 안 끝나면 죽인다.
 * 반환: 정상 종료(코드 0) 0, 그 밖 -1 */
static int wait_child(pid_t pid, const struct timespec *deadline)
{
    struct timespec pause = {0, CGI_WAIT_POLL_NSEC};
    int wait_status = 0;

    for (;;) {
        pid_t done = waitpid(pid, &wait_status, WNOHANG);
        if (done == pid) {
            return is_successful_exit(wait_status) ? 0 : -1;
        }
        if (done < 0 && errno != EINTR) {
            perror("warn: cgi waitpid");
            return -1;
        }
        if (remaining_ms(deadline) <= 0) {
            fprintf(stderr, "warn: cgi did not exit within %d s\n",
                    CGI_TIMEOUT_SEC);
            kill_and_reap(pid);
            return -1;
        }
        (void)nanosleep(&pause, NULL);   /* 일찍 깨도 다시 확인하면 된다 */
    }
}

static bool is_successful_exit(int wait_status)
{
    if (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0) {
        return true;
    }
    if (WIFEXITED(wait_status)) {
        fprintf(stderr, "warn: cgi exited with status %d\n",
                WEXITSTATUS(wait_status));
    } else if (WIFSIGNALED(wait_status)) {
        fprintf(stderr, "warn: cgi killed by signal %d\n",
                WTERMSIG(wait_status));
    }
    return false;
}

/* 자식과 그 프로세스 그룹을 강제로 끝내고 거둔다.
 * 거두지 않으면 좀비 프로세스로 남는다 */
static void kill_and_reap(pid_t pid)
{
    (void)kill(-pid, SIGKILL);  /* 그룹 전체. 이미 끝났으면 실패해도 상관없다 */
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
        continue;
    }
}

/* 지금부터 CGI_TIMEOUT_SEC 뒤. 시계를 바꿔도 영향 없는 MONOTONIC 시계 */
static int make_deadline(struct timespec *deadline)
{
    if (clock_gettime(CLOCK_MONOTONIC, deadline) < 0) {
        return -1;
    }
    deadline->tv_sec += CGI_TIMEOUT_SEC;
    return 0;
}

/* 마감까지 남은 밀리초. 지났으면 0 이하 */
static int remaining_ms(const struct timespec *deadline)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return 0;
    }
    long ms = (deadline->tv_sec - now.tv_sec) * MILLIS_PER_SECOND
              + (deadline->tv_nsec - now.tv_nsec) / NANOS_PER_MILLI;
    return (int)ms;
}

/* 헤더 끝(빈 줄)의 위치. "\r\n\r\n"과 "\n\n"을 모두 받는다.
 * 찾으면 separator_length에 빈 줄 길이(4 또는 2), 못 찾으면 0 */
static size_t find_header_end(const char *output, size_t length,
                              size_t *separator_length)
{
    static const char CRLF_END[] = "\r\n\r\n";
    static const char LF_END[] = "\n\n";

    *separator_length = 0;
    for (size_t i = 0; i < length; i++) {
        size_t left = length - i;
        if (left >= strlen(CRLF_END)
            && memcmp(output + i, CRLF_END, strlen(CRLF_END)) == 0) {
            *separator_length = strlen(CRLF_END);
            return i;
        }
        if (left >= strlen(LF_END)
            && memcmp(output + i, LF_END, strlen(LF_END)) == 0) {
            *separator_length = strlen(LF_END);
            return i;
        }
    }
    return length;
}

/* 헤더 줄들에서 "Content-Type:" 값을 찾아 out에 복사한다 (대소문자 무시).
 * 반환: 0, 없거나 값이 너무 길면 -1 */
static int find_content_type(const char *headers, char *out,
                             size_t out_size)
{
    size_t name_length = strlen(CONTENT_TYPE_NAME);
    const char *line = headers;

    while (*line != '\0') {
        size_t line_length = strcspn(line, "\r\n");
        if (line_length > name_length
            && strncasecmp(line, CONTENT_TYPE_NAME, name_length) == 0) {
            const char *value = line + name_length;
            size_t value_length = line_length - name_length;
            size_t skip = strspn(value, " \t");
            int written = snprintf(out, out_size, "%.*s",
                                   (int)(value_length - skip), value + skip);
            return written > 0 && (size_t)written < out_size ? 0 : -1;
        }
        line += line_length;
        line += strspn(line, "\r\n");
    }
    return -1;
}
