# PLAN.md — 1단계: 필수 구현 항목

> 2단계(Proxy / Reverse Proxy)는 이 계획에 포함하지 않는다. 다만 나중에 붙이기 쉽도록 설계한다 (맨 아래 "2단계 대비" 참고).

## 0. 실행 방법 (목표 모습)

```bash
make
./build/tiny -p 8080 -r ./www -c ./cgi-bin -t 8
#  -p 포트   -r 정적 파일 루트   -c CGI 디렉토리   -t 워커 스레드 수
```

- 정적 파일: `GET /index.html` → `./www/index.html`
- CGI: `GET|POST /cgi-bin/<이름>` → `./cgi-bin/<이름>` 실행 결과
- 인자 파싱은 POSIX `getopt()` 사용. 2단계의 라우팅 규칙은 설정 파일(`-f`)로 받을 예정.

---

## 1. 디렉토리 / 파일 구조

핵심 원칙: **소켓을 다루는 코드**와 **HTTP 규칙을 다루는 코드**를 분리한다.
파서·MIME·경로 처리는 "문자열을 넣으면 결과가 나오는" 순수 함수라서 서버를 띄우지 않고도 테스트할 수 있다.

```
tiny-web-server/
├── Makefile
├── PROJECT.md / CLAUDE.md / PLAN.md
├── src/
│   ├── main.c            # CLI 인자 파싱, 시그널 설정, 서버 시작/종료
│   ├── config.c/.h       # 설정 구조체 (포트, 루트, 스레드 수, 각종 제한값)
│   │
│   │   ── 네트워크 계층 (소켓을 아는 코드) ──
│   ├── server.c/.h       # socket → bind → listen → accept 루프
│   ├── threadpool.c/.h   # 작업 큐 + pthread 워커
│   ├── conn.c/.h         # 연결 1개 처리: 읽기 버퍼, keep-alive 반복, 타임아웃
│   ├── netio.c/.h        # read/write 래퍼 (EINTR 재시도, 부분 쓰기 처리)
│   │
│   │   ── HTTP 계층 (소켓을 모르는 코드, 단위 테스트 대상) ──
│   ├── http_parser.c/.h  # 바이트 버퍼 → http_request 구조체
│   ├── http_response.c/.h# 상태 라인/헤더 문자열 생성, 상태코드 → 문구
│   ├── path.c/.h         # URI 퍼센트 디코딩, 경로 정규화 (../ 차단)
│   ├── mime.c/.h         # 확장자 → Content-Type
│   │
│   │   ── 처리기(handler) 계층 ──
│   ├── router.c/.h       # 요청 → 어떤 handler로 보낼지 결정
│   ├── handler_static.c/.h
│   ├── handler_cgi.c/.h
│   └── log.c/.h          # 스레드 안전 로그 (접속 로그, 에러 로그)
├── tests/
│   ├── test.h            # assert 기반 미니 테스트 매크로
│   ├── test_http_parser.c
│   ├── test_http_response.c
│   ├── test_path.c
│   ├── test_mime.c
│   ├── test_router.c
│   └── e2e.sh            # curl 기반 기능 테스트 스크립트 (서버 띄워서 확인)
├── www/                  # 테스트용 정적 파일 (index.html, style.css, app.js, img.png, img.jpg, secret.txt(권한 000))
├── cgi-bin/              # 테스트용 CGI (hello.sh, echo_post.sh, env.sh, slow.sh, adder.c)
└── build/                # 빌드 결과물 (git에 올리지 않음)
```

### 계층 간 의존 방향 (한쪽으로만)
```
main → server → threadpool → conn → router → handler_static / handler_cgi
                                 ↘ http_parser, http_response, path, mime (순수 함수)
```
- `http_parser`, `path`, `mime`, `http_response`는 `<sys/socket.h>`를 include 하지 않는다.
- handler는 소켓 fd에 직접 쓰지 않고 `conn`이 넘겨준 "응답 쓰기 도구"(writer)를 통해 쓴다.

---

## 2. Makefile 타깃

| 타깃 | 하는 일 |
|---|---|
| `make` | `build/tiny` 빌드. `-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -O2 -pthread` |
| `make test` | `tests/test_*.c` 각각을 빌드해 실행, 하나라도 실패하면 종료 코드 ≠ 0 |
| `make asan` | `-fsanitize=address,undefined -g -O1` 로 `build/tiny-asan` + 테스트를 빌드하고 단위 테스트까지 실행 |
| `make clean` | `build/` 삭제 |
| (선택) `make e2e` | 서버를 띄우고 `tests/e2e.sh` 실행 후 종료 |

### 단위 테스트 방식
- 외부 프레임워크 없음. `tests/test.h`에 assert 스타일 매크로 몇 개만 둔다.
  ```c
  #define CHECK(cond)       /* 실패 시 파일:줄 + 조건식 출력, 실패 카운트 증가 */
  #define CHECK_EQ_INT(a,b)
  #define CHECK_EQ_STR(a,b)
  ```
  (`assert()`는 첫 실패에서 바로 멈추므로, 실패를 모두 보고 끝에 `exit(1)` 하는 형태로 감싼다.)
- 테스트 파일은 순수 함수 모듈의 `.c`만 함께 링크 → 소켓 없이 실행된다.

---

## 3. 에러 처리 표

| 코드 | 언제 | 예시 |
|---|---|---|
| **400** Bad Request | 요청 형식이 깨짐 | 요청 라인 토큰 수 틀림, 헤더에 `:` 없음, HTTP/1.1인데 `Host` 없음, `Content-Length`가 숫자가 아니거나 여러 개가 서로 다름, `Content-Length`와 `Transfer-Encoding` 동시 존재, URI 디코딩 실패(`%zz`), 본문이 Content-Length보다 짧게 끊김 |
| **403** Forbidden | 접근 불가 | `../`로 루트 밖 접근 시도, 읽기 권한 없는 파일, index 없는 디렉토리, 실행 권한 없는 CGI |
| **404** Not Found | 파일 없음 | 존재하지 않는 경로 |
| **405** Method Not Allowed | 아는 메서드지만 이 자원에 안 됨 | 정적 파일에 `POST` → `Allow: GET, HEAD` 헤더 포함 |
| **413** Content Too Large | 본문이 제한 초과 | `Content-Length` > 1 MiB (설정값) → 본문을 읽기 전에 거절 |
| **414** URI Too Long | URI가 너무 김 | 요청 타깃 > 4 KiB |
| **431** Request Header Fields Too Large | 헤더가 너무 큼 | 헤더 전체 > 8 KiB 또는 헤더 개수 > 64 |
| **500** Internal Server Error | 서버 내부 실패 | `malloc` 실패, CGI `fork`/`exec` 실패, CGI 비정상 종료·타임아웃, CGI 출력 형식 오류 |
| **501** Not Implemented | 서버가 모르는/구현 안 한 기능 | `PUT`, `DELETE`, `PATCH` 등 미구현 메서드, 요청의 `Transfer-Encoding: chunked` |
| **505** HTTP Version Not Supported | 지원 안 하는 버전 | `HTTP/2.0`, `HTTP/0.9` (형식 자체가 `HTTP/x.y`가 아니면 400) |

- 지원 메서드: `GET`, `POST`, `HEAD`(GET과 같되 본문 없이 — 구현 비용이 작아서 포함).
- 에러 응답도 정상 HTTP 응답 형식(상태 라인 + `Content-Type: text/html` + `Content-Length` + 짧은 HTML 본문).
- 400/413/414/431처럼 요청 경계가 불확실해진 경우 응답 후 **연결을 닫는다** (`Connection: close`).
- 제한값(1 MiB, 4 KiB, 8 KiB 등)은 `config.h`에 상수로 모은다.

---

## 4. 구현 순서 (작게 동작하는 것부터)

각 Step이 끝나면 멈추고 사용자 확인을 받는다.

### Step 0. 환경 준비
- 디렉토리 구조, `Makefile`, `.gitignore`(`build/`), 빈 `main.c`, `tests/test.h`, 샘플 테스트 1개.
- **확인**
  ```bash
  make            # 경고 없이 build/tiny 생성
  make test       # "1 passed" 류 출력
  make asan       # sanitizer 빌드 성공
  make clean && ls build   # build/ 없음
  ```

### Step 1. 에코 서버 (TCP 기본 흐름)
- `socket → setsockopt(SO_REUSEADDR) → bind → listen → accept` 후 받은 바이트를 그대로 돌려줌.
- `SIGPIPE` 무시, bind 실패 시 이유(`EADDRINUSE` 등) 출력 후 종료.
- 아직 동시 처리 없음 (한 번에 한 클라이언트).
- **확인**
  ```bash
  ./build/tiny -p 8080 &
  nc localhost 8080        # 아무거나 입력 → 그대로 돌아옴, Ctrl+C로 종료
  ./build/tiny -p 8080     # 이미 사용 중 → "Address already in use" 메시지
  ```

### Step 2. 고정 HTTP 응답
- 요청 내용과 무관하게 `HTTP/1.1 200 OK` + `Content-Length` + `Hello, world!` 응답 후 close.
- 요청의 끝(`\r\n\r\n`)까지 읽는 법을 여기서 처음 다룸.
- **확인**
  ```bash
  curl -v http://localhost:8080/     # < HTTP/1.1 200 OK, 본문 Hello, world!
  ```
  브라우저에서 `http://localhost:8080` 열기 (WSL2는 Windows 브라우저에서 localhost로 접근 가능).

### Step 3. HTTP 요청 파싱 (단위 테스트 중심)
- `http_parser`: 요청 라인(메서드/URI/버전) + 헤더 목록 파싱. 헤더 이름은 대소문자 무시.
  - 반환값: `PARSE_OK` / `PARSE_INCOMPLETE`(더 읽어야 함) / `PARSE_ERROR`(+ 상태코드 400/414/431/501/505)
  - URI를 `path`와 `query`로 분리. `Host`, `Content-Length`, `Connection`은 편의 필드로도 저장.
- `path`: 퍼센트 디코딩, `.`/`..` 정규화.
- 서버는 파싱 결과를 로그로 출력하고 여전히 고정 응답.
- **확인**
  ```bash
  make test     # test_http_parser, test_path 통과
                # 정상 요청 / 헤더 여러 개 / 잘린 요청(INCOMPLETE) / 잘못된 버전 / 너무 긴 URI / 헤더 초과 등
  curl -v "http://localhost:8080/a/b?x=1"   # 서버 로그에 method=GET path=/a/b query=x=1
  ```

### Step 4. HTTP 응답 생성 + 에러 응답
- `http_response`: 상태 라인 + `Date`, `Server`, `Content-Type`, `Content-Length`, `Connection` 헤더 생성.
- 파싱 에러를 실제 400/414/431/501/505 응답으로 돌려줌.
- **확인**
  ```bash
  make test
  printf 'GARBAGE\r\n\r\n' | nc -q1 localhost 8080              # 400
  printf 'GET / HTTP/2.0\r\nHost: x\r\n\r\n' | nc -q1 localhost 8080   # 505
  curl -i -X DELETE http://localhost:8080/                       # 501
  curl -i "http://localhost:8080/$(head -c 5000 /dev/zero | tr '\0' a)"   # 414
  curl -i -H "X-Big: $(head -c 9000 /dev/zero | tr '\0' a)" http://localhost:8080/  # 431
  printf 'GET / HTTP/1.1\r\n\r\n' | nc -q1 localhost 8080        # 400 (Host 없음)
  ```

### Step 5. 정적 파일 서빙
- `mime`: html, css, js, json, txt, png, jpg/jpeg, gif, svg, ico, 기타 → `application/octet-stream`.
- 경로 → 루트 기준 실제 파일. `realpath()`로 루트 밖이면 403. 디렉토리면 `index.html`.
- 큰 파일은 고정 크기 버퍼로 나눠 읽어 전송 (전체를 메모리에 올리지 않음). `HEAD` 지원.
- **확인**
  ```bash
  make test                                              # test_mime
  curl -i http://localhost:8080/                         # index.html, text/html
  curl -sI http://localhost:8080/style.css | grep -i content-type   # text/css
  curl -s http://localhost:8080/img.png | cmp - www/img.png && echo SAME   # 바이너리 동일
  curl -i http://localhost:8080/nope.html                # 404
  curl -i --path-as-is http://localhost:8080/../PROJECT.md   # 403
  curl -i http://localhost:8080/%2e%2e/PROJECT.md        # 403
  curl -i http://localhost:8080/secret.txt               # 403 (권한 000)
  ```
  브라우저로 `index.html` 열어 CSS/JS/이미지가 모두 로드되는지 확인.

### Step 6. POST 본문 처리
- `Content-Length`만큼 정확히 읽기 (헤더와 함께 이미 읽힌 부분 + 나머지).
- 제한 초과 → 413, 본문이 덜 오고 연결 끊김 → 400(또는 조용히 close), 정적 파일에 POST → 405.
- 확인용 내장 handler `POST /echo` (받은 본문을 그대로 돌려줌) — Step 7 이후 남겨둘지 결정.
- **확인**
  ```bash
  curl -i -X POST -d 'name=kim&age=20' http://localhost:8080/echo      # 본문 그대로
  head -c 500000 /dev/urandom > /tmp/b.bin && curl -s --data-binary @/tmp/b.bin http://localhost:8080/echo | cmp - /tmp/b.bin && echo SAME
  head -c 2000000 /dev/zero | curl -i --data-binary @- http://localhost:8080/echo   # 413
  curl -i -X POST -d 'x' http://localhost:8080/index.html               # 405 + Allow 헤더
  ```

### Step 7. CGI (동적 요청 처리)
- `/cgi-bin/<이름>` → `fork()` + `execve()`.
  - 환경 변수: `REQUEST_METHOD`, `QUERY_STRING`, `CONTENT_LENGTH`, `CONTENT_TYPE`, `SCRIPT_NAME`, `SERVER_PROTOCOL`, `SERVER_PORT`, `REMOTE_ADDR`, `HTTP_*`.
  - POST 본문 → 파이프로 CGI의 stdin, CGI stdout → 파이프로 읽어서 응답.
  - CGI 출력의 헤더(`Content-Type`, `Status:`)를 해석하고 `Content-Length`는 서버가 계산.
  - `waitpid()`로 좀비 회수, 실행 시간 제한(예: 5초) 초과 시 `kill` 후 500.
  - 멀티스레드에서 fork하므로: env/argv 준비는 fork **전에**, 자식은 dup2/exec만. 소켓 fd에는 `FD_CLOEXEC`.
- **확인**
  ```bash
  curl -i "http://localhost:8080/cgi-bin/hello.sh"
  curl -i "http://localhost:8080/cgi-bin/adder?a=3&b=4"           # 결과 7
  curl -i -X POST -d 'hello cgi' http://localhost:8080/cgi-bin/echo_post.sh
  curl -s "http://localhost:8080/cgi-bin/env.sh?x=1" | grep QUERY_STRING
  curl -i http://localhost:8080/cgi-bin/not_exist                  # 404
  curl -i http://localhost:8080/cgi-bin/noexec.sh                  # 403
  ps -ef | grep defunct                                            # 좀비 없음
  ```

### Step 8. 동시 처리 (스레드 풀)
- `threadpool`: 고정 개수 워커 + 크기 제한 있는 원형 큐 + `pthread_mutex` + `pthread_cond`.
  - 메인 스레드: `accept()` → fd를 큐에 넣음. 워커: 큐에서 fd를 꺼내 `conn` 처리.
  - 큐가 가득 차면 503 응답 후 close (과부하 시 무한정 쌓지 않음).
  - 공유 자원(로그 등)은 뮤텍스 보호. `strtok` 대신 `strtok_r` 등 스레드 안전 함수만 사용.
- **확인**
  ```bash
  # 2초 걸리는 CGI를 10개 동시에 → 직렬이면 20초, 풀(8개)이면 약 4초
  time (for i in $(seq 10); do curl -s http://localhost:8080/cgi-bin/slow.sh > /dev/null & done; wait)
  ab -n 2000 -c 50 http://localhost:8080/index.html     # Failed requests: 0
  ```
  (`ab`가 없으면 `sudo apt install apache2-utils`)

### Step 9. Keep-Alive
- 한 연결에서 요청을 반복 처리. 규칙:
  - HTTP/1.1 기본 유지, `Connection: close`면 종료. HTTP/1.0은 `Connection: keep-alive`일 때만 유지.
  - 앞 요청 뒤에 이미 읽힌 바이트(파이프라이닝)는 버리지 않고 다음 요청 파싱에 사용.
  - 유휴 타임아웃(예: 5초, `SO_RCVTIMEO`)과 연결당 최대 요청 수(예: 100) → 워커가 영원히 묶이지 않게.
- **확인**
  ```bash
  curl -v http://localhost:8080/ http://localhost:8080/style.css 2>&1 | grep -i "re-us"   # Re-using existing connection
  printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /style.css HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | nc localhost 8080   # 응답 2개 연속
  ab -k -n 5000 -c 50 http://localhost:8080/index.html   # Keep-Alive requests 수 확인
  nc localhost 8080   # 아무것도 안 보내고 5초 뒤 서버가 끊는지
  ```

### Step 10. 안정화
- **종료 처리**: `SIGINT`/`SIGTERM` → 플래그 설정 → accept 루프 탈출 → 큐 정리 → 워커 join → 자원 해제. (`SA_RESTART` 없이 등록해 `accept`가 `EINTR`로 깨어나게)
- **자원 회수**: 클라이언트가 중간에 끊어도 fd/메모리/CGI 자식 프로세스가 남지 않게.
- **느린 클라이언트**: 헤더를 아주 천천히 보내는 경우 타임아웃으로 정리.
- **검사**
  ```bash
  make asan && ./build/tiny-asan -p 8080 -r ./www -c ./cgi-bin &   # e2e 실행 후 Ctrl+C → 누수 리포트 없어야 함
  valgrind --leak-check=full ./build/tiny -p 8080 ...              # (선택)
  ./tests/e2e.sh                                                    # 위 모든 curl 확인을 자동화
  ab -n 20000 -c 100 http://localhost:8080/index.html               # 처리량 기록 (README용)
  (printf 'GET / HTTP/1.1\r\nHost: x\r\n'; sleep 1) | nc -q0 localhost 8080   # 요청 도중 끊기 → 서버 정상
  ```
- 결과(처리량, 테스트 목록)를 README에 정리.

---

## 5. 2단계(Proxy) 대비 — 지금 신경 쓸 점

1. **파서는 요청/응답 공용으로**: 헤더 파싱 부분을 별도 함수로 분리 → 2단계에서 백엔드의 *응답*을 파싱할 때 재사용.
2. **헤더를 전부 순서대로 보관**: Host/Content-Length 같은 것만 뽑지 말고 원본 이름·값 목록을 유지 → 프록시가 그대로 전달 가능.
3. **요청 타깃 원문 보존 + absolute-form 대비**: `GET http://host:port/path HTTP/1.1` (포워드 프록시 형식)도 파싱 단계에서 구분할 수 있게 `raw_target` 필드를 둔다. 1단계에서는 origin-form(`/path`)만 처리.
4. **router + handler 인터페이스**: `handler(req, writer, ctx)` 형태의 함수 포인터로 통일 → 2단계에서 `handler_proxy`만 추가하면 됨. 라우팅 규칙은 "경로 prefix → handler" 표 형태로.
5. **응답 쓰기 도구(writer) 추상화**: handler가 fd에 직접 쓰지 않음 → 백엔드 응답을 받아 스트리밍으로 흘려보내기 쉬움.
6. **netio 모듈을 클라이언트 쪽에도 쓸 수 있게**: `write_all`, `read_some`, 타임아웃 설정 함수는 서버 소켓 전용이 아니게 작성 → 백엔드 연결(`connect`)에도 재사용.
7. **hop-by-hop 헤더 개념**: `Connection`, `Keep-Alive`, `Transfer-Encoding` 등은 연결 단위 헤더. 지금부터 이 처리를 한곳(`conn`)에 모아둔다.
8. **설정 구조체 확장 가능하게**: `config`에 나중에 `routes[]`, `backends[]`를 추가할 자리를 고려. CLI 인자 외에 설정 파일 읽기(`-f`)를 넣기 쉬운 구조.
9. **타임아웃을 설정값으로**: 클라이언트 읽기 타임아웃을 상수 대신 config로 → 백엔드 응답 타임아웃(→ 502/504)에도 같은 방식 적용.
10. **에러 응답 함수 일반화**: `send_error(writer, status)`가 상태코드 표를 사용 → 502/504 추가가 한 줄로 끝나게.
