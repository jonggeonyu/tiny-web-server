# C 언어 웹서버 개발 사전 지식

## 0. 프로젝트 개요

### 목표

외부 HTTP 라이브러리나 웹 프레임워크 없이 C 언어와 POSIX 소켓 API를 사용하여 HTTP/1.1 웹 서버를 직접 구현한다.

### 필수 구현 사항

- TCP 소켓 서버
- HTTP/1.1 요청 파싱 및 응답 생성
- GET / POST 요청 처리
- 정적 파일 서빙 (HTML, CSS, JS, 이미지)
- 동적 요청 처리
- 동시 클라이언트 요청 처리
- Reverse Proxy
- 메모리 및 소켓 자원 관리
- 시그널 및 안전한 종료 처리
- 단위·기능·부하 테스트

### 현재 선택한 설계 방향

| 항목 | 선택 |
|---|---|
| 개발 언어 | C |
| 통신 방식 | TCP (POSIX Socket API) |
| HTTP 버전 | HTTP/1.1 |
| 동시성 모델 | pthread 기반 Thread Pool |
| I/O 모델 | Blocking I/O로 시작 |
| 동기화 | Mutex + Condition Variable |
| 프록시 | Reverse Proxy |
| 라우팅 | URI 경로 기반 |
| 종료 처리 | SIGINT / SIGTERM / SIGPIPE |
| 개발 환경 | macOS, Linux 이식성 고려 |

---

## 1. 소켓 프로그래밍 (Socket Programming)

### 1.1. 소켓이란?

**소켓(Socket)은 프로그램이 운영체제를 통해 네트워크 데이터를 주고받을 때 사용하는 통신 인터페이스다.**

예를 들어 브라우저와 웹서버가 HTTP 요청 및 응답을 주고받으려면 소켓을 사용한다.

#### TCP와 소켓의 차이

| 구분 | 의미 |
|---|---|
| TCP | 데이터를 순서대로 신뢰성 있게 전달하는 전송 프로토콜 |
| Socket | 프로그램이 TCP 등의 통신 기능을 사용하기 위한 인터페이스 |
| IP 주소 | 통신하려는 컴퓨터의 네트워크 주소 |
| Port | 컴퓨터 내 특정 네트워크 서비스의 통신 번호 |
| fd | 프로세스가 열린 소켓 등의 자원을 참조하는 정수 |

#### 파일 디스크립터(fd)

C에서 소켓을 생성하면 운영체제는 파일 디스크립터를 반환한다.

```c
int server_fd = socket(AF_INET, SOCK_STREAM, 0);
```

- `AF_INET`: IPv4 사용
- `SOCK_STREAM`: 스트림 소켓(TCP)
- `0`: 기본 프로토콜 선택
- `server_fd`: 소켓을 가리키는 파일 디스크립터

예를 들어 `server_fd = 3`이라면 프로그램은 fd 3을 이용해 소켓을 조작할 수 있다.

### 1.2. TCP 소켓 서버의 동작 과정

```text
socket()
   ↓
bind()
   ↓
listen()
   ↓
accept()
   ↓
recv()
   ↓
HTTP 요청 처리
   ↓
send()
   ↓
close()
```

| 함수 | 역할 |
|---|---|
| `socket()` | 소켓 생성 |
| `bind()` | 로컬 IP 주소와 포트 지정 |
| `listen()` | TCP 연결 요청을 받을 준비 |
| `accept()` | 완료된 클라이언트 연결 수락 |
| `recv()` | 데이터 수신 |
| `send()` | 데이터 송신 |
| `close()` | 파일 디스크립터 닫기 |
| `connect()` | 다른 서버에 연결 요청 |

#### 리스닝 소켓과 연결 소켓

**Listening Socket**

- `socket()`, `bind()`, `listen()`으로 준비
- 새로운 클라이언트 연결을 기다림
- 일반적으로 서버가 종료될 때까지 유지

**Connected Socket**

- `accept()`가 반환하는 새로운 소켓
- 특정 클라이언트와 실제 데이터를 송수신
- 연결 처리가 끝나면 닫음

```c
int client_fd = accept(server_fd, NULL, NULL);
```

`accept()`는 기존 `server_fd`를 없애지 않는다. 새로운 연결 소켓인 `client_fd`를 반환한다.

### 1.3. TCP 3-way Handshake

TCP는 데이터를 송수신하기 전에 연결을 수립한다.

```text
클라이언트                    서버
    │                         │
    │────── SYN ─────────────→│
    │                         │
    │←───── SYN + ACK ────────│
    │                         │
    │────── ACK ─────────────→│
    │                         │
       TCP 연결 수립 완료
```

1. SYN: 클라이언트가 연결을 요청한다.
2. SYN + ACK: 서버가 요청을 확인하고 응답한다.
3. ACK: 클라이언트가 서버의 응답을 확인한다.

이 과정은 애플리케이션 코드가 직접 구현하지 않고 운영체제의 TCP 스택에서 처리한다. 서버 프로그램은 `accept()`를 통해 수립된 연결을 받아 사용한다.

### 1.4. Blocking / Non-blocking I/O

#### Blocking I/O

```c
int client_fd = accept(server_fd, NULL, NULL);
```

- 연결이 없다면 해당 스레드는 대기한다.
- `recv()`도 데이터가 없다면 기다릴 수 있다.
- 상대적으로 구현이 간단하다.

#### Non-blocking I/O

- 즉시 처리할 수 없다면 기다리지 않고 반환한다.
- `EAGAIN`, `EWOULDBLOCK` 등을 처리해야 한다.
- `poll()`, `epoll()`, `kqueue()` 등의 I/O 감시 방식과 함께 활용할 수 있다.

#### SO_REUSEADDR

서버를 재시작할 때 이전 연결 상태 등으로 주소 재사용이 어려운 상황을 줄이기 위한 소켓 옵션이다.

```c
int opt = 1;

setsockopt(
    server_fd,
    SOL_SOCKET,
    SO_REUSEADDR,
    &opt,
    sizeof(opt)
);
```

`bind()` 전에 설정하며, 이미 실행 중인 다른 서버의 포트를 강제로 빼앗는 옵션은 아니다.

#### 반드시 기억할 점

**TCP는 메시지 단위가 아닌 바이트 스트림 방식이다.**

따라서 `recv()` 한 번으로 HTTP 요청 전체가 들어온다는 보장이 없으며, `send()` 한 번으로 모든 데이터가 전송된다는 보장도 없다. 반복 수신 및 반복 전송 처리가 필요하다.

---

## 2. HTTP/1.1 프로토콜

### 2.1. HTTP란?

HTTP(Hypertext Transfer Protocol)는 클라이언트와 서버가 요청과 응답을 어떤 형식으로 주고받을지 정의하는 애플리케이션 계층 프로토콜이다.

- TCP: 데이터를 전달하는 역할
- HTTP: 전달하는 데이터의 형식과 의미를 정의하는 역할

이번 과제에서는 HTTP/1.1을 직접 파싱하고 응답을 생성한다.

참고 명세:

- RFC 9110: HTTP Semantics
- RFC 9112: HTTP/1.1

기존 RFC 7230~7235에서 다루던 내용은 현재 갱신된 명세를 함께 참고한다.

### 2.2. HTTP 요청 구조

```http
POST /api/hello HTTP/1.1
Host: localhost:8080
Content-Type: text/plain
Content-Length: 5
Connection: close

hello
```

HTTP 요청은 세 부분으로 구성된다.

**① Request Line**

```text
POST /api/hello HTTP/1.1
```

- Method: POST
- URI: /api/hello
- Version: HTTP/1.1

**② Header**

요청에 대한 부가 정보를 전달한다.

| 헤더 | 의미 |
|---|---|
| Host | 요청 대상 호스트 |
| Content-Type | 본문의 데이터 형식 |
| Content-Length | 본문의 바이트 길이 |
| Connection | 연결 관리 관련 설정 |
| Transfer-Encoding | 메시지 본문의 전송 인코딩 |

**③ Body**

POST 등에서 전달할 데이터를 담는다. GET 요청에도 프로토콜 차원에서 본문이 절대 금지된 것은 아니지만, 일반적인 GET 요청에서는 본문을 사용하지 않는다.

### 2.3. HTTP 응답 구조

```http
HTTP/1.1 200 OK
Content-Type: text/html
Content-Length: 14
Connection: close

<h1>Hello</h1>
```

응답 역시 세 부분으로 구성된다.

1. Status Line: HTTP 버전, 상태 코드, 사유 구문
2. Header: Content-Type, Content-Length 등
3. Body: 실제 HTML 또는 데이터

#### 주요 HTTP 상태 코드

| 코드 | 의미 |
|---|---|
| 200 | 요청 성공 |
| 400 | 잘못된 요청 |
| 403 | 접근 금지 |
| 404 | 리소스를 찾을 수 없음 |
| 405 | 지원하지 않는 메서드 |
| 413 | 요청 본문 크기 제한 초과 |
| 431 | 요청 헤더 크기 제한 초과 |
| 500 | 서버 내부 오류 |
| 502 | 프록시의 잘못된 백엔드 응답 등 |
| 504 | 백엔드 응답 타임아웃 |

### 2.4. HTTP 요청 파싱

C 웹서버는 `recv()`로 받은 데이터를 직접 분석해야 한다.

HTTP/1.1에서 헤더와 본문은 다음과 같이 구분한다.

```text
요청 라인\r\n
헤더 1\r\n
헤더 2\r\n
\r\n
본문
```

`\r\n\r\n`은 헤더가 끝나는 지점이다.

TCP는 바이트 스트림이기 때문에 한 번의 `recv()`에 헤더 전체가 들어온다는 보장은 없다. 따라서 다음처럼 처리한다.

```text
데이터 수신
   ↓
버퍼에 누적
   ↓
헤더 종료 지점 탐색
   ↓
요청 라인 및 헤더 파싱
   ↓
본문 길이 확인
   ↓
본문 수신 완료까지 대기
   ↓
요청 처리
```

본문 경계는 `Content-Length` 또는 `Transfer-Encoding` 등의 HTTP 메시지 프레이밍 규칙에 따라 판단한다.

### 2.5. Content-Length

`Content-Length`는 본문 길이를 바이트 단위로 나타낸다.

```http
Content-Length: 5

hello
```

본문이 5바이트이므로 정확히 5바이트를 읽어야 한다.

잘못된 Content-Length, 중복·충돌 헤더, 지나치게 큰 본문 등의 검증도 필요하다.

### 2.6. Keep-Alive

**Keep-Alive는 하나의 TCP 연결에서 여러 HTTP 요청과 응답을 처리할 수 있도록 연결을 재사용하는 개념이다.** HTTP/1.1에서는 지속 연결이 기본 동작이다.

`Connection: close`가 지정되면 해당 응답 이후 연결을 종료한다.

Keep-Alive를 지원하려면 서버는 다음 요청이 어디서 시작하는지 정확하게 알아야 하므로 메시지 길이 파싱이 중요하다.

초기 구현에서는 `Connection: close`로 단순화하고 추후 Keep-Alive를 추가할 수 있다.

### 2.7. Chunked Transfer Encoding

본문 전체 크기를 미리 알지 못할 때 청크 단위로 데이터를 전송하는 방식이다.

```http
Transfer-Encoding: chunked

5
hello
0

```

실제 전송에서는 각 청크 줄과 데이터 뒤에 CRLF가 들어간다.

- `5`: 다음 청크의 길이(16진수)
- `hello`: 청크 데이터
- `0`: 마지막 청크

완전한 HTTP/1.1 메시지 처리를 위해서는 chunked 프레이밍도 알아야 한다.

특히 프록시에서는 Content-Length와 Transfer-Encoding의 해석이 서로 달라지면 HTTP Request Smuggling 같은 보안 문제가 발생할 수 있다.

---

## 3. 동시성 처리 (Concurrency)

### 3.1. 동시성이란?

**동시성은 여러 작업이 시간적으로 겹쳐 진행되도록 관리하는 개념이다.** 웹서버에서는 여러 클라이언트의 HTTP 요청을 처리하기 위해 필요하다.

단일 스레드에서 Blocking I/O만 사용한다면 한 클라이언트의 요청을 처리하는 동안 다른 요청의 처리가 지연될 수 있다. 동시성 모델을 적용하면 여러 클라이언트의 요청을 겹쳐 처리할 수 있다.

### 3.2. 대표적인 동시성 모델

| 모델 | 원리 | 특징 |
|---|---|---|
| fork() | 여러 프로세스 생성 | 격리성이 좋지만 프로세스 비용 발생 |
| pthread | 여러 스레드 사용 | 자원 공유가 가능하지만 동기화 필요 |
| I/O Multiplexing | 소켓의 I/O 준비 상태 감시 | 많은 연결 관리에 유리 |

#### 멀티프로세스

```text
서버 프로세스
  ├─ 자식 프로세스 A
  ├─ 자식 프로세스 B
  └─ 자식 프로세스 C
```

`fork()`로 자식 프로세스를 생성해 각각 요청을 처리한다.

#### 멀티스레드

```text
서버 프로세스
  ├─ Main Thread
  ├─ Worker Thread 1
  ├─ Worker Thread 2
  └─ Worker Thread 3
```

하나의 프로세스 내 여러 스레드가 요청을 처리한다. 힙과 전역 변수 등의 메모리를 공유하므로 동기화가 필요하다.

### 3.3. Thread Pool

**Thread Pool은 워커 스레드를 미리 일정 개수 생성하고 재사용하는 방식이다.**

```text
클라이언트 요청
       ↓
Main Thread
    accept()
       ↓
  공유 작업 큐
       ↓
 ┌─────┼─────┐
 ↓     ↓     ↓
Worker Worker Worker
  1      2      3
```

#### 동작 과정

1. 서버 시작 시 워커 스레드 생성
2. 메인 스레드가 accept()로 연결 수락
3. client_fd를 공유 작업 큐에 삽입
4. 워커가 큐에서 작업 획득
5. HTTP 요청 수신 및 처리
6. 응답 전송 및 연결 정리
7. 워커가 다음 작업 대기

#### 장점

- 스레드 생성 비용 감소
- 스레드 개수 제한 가능
- 구조가 비교적 직관적
- macOS와 Linux에서 구현 가능

#### 단점

- 스레드 간 공유 자원 동기화 필요
- 워커가 Blocking I/O로 오래 대기할 수 있음
- 동시 연결이 매우 많으면 자원 사용량 증가
- 작업 큐가 가득 찰 때의 처리 정책 필요

### 3.4. I/O Multiplexing

여러 소켓 중 I/O를 처리할 준비가 된 소켓을 감시하는 방식이다.

| API | 운영체제 | 특징 |
|---|---|---|
| select() | POSIX 계열 | 기본적인 다중 소켓 감시 |
| poll() | POSIX 계열 | 파일 디스크립터 배열 기반 |
| epoll() | Linux | 이벤트 기반의 효율적인 다중 연결 감시 |
| kqueue() | macOS / BSD | 이벤트 기반 감시 |

#### epoll의 개념

```text
Socket A ── 대기
Socket B ── 읽기 준비
Socket C ── 대기
                 ↓
             epoll_wait()
                 ↓
           Socket B 처리
```

epoll이 직접 HTTP 요청을 파싱하거나 데이터를 보내는 것은 아니다. **어떤 소켓이 I/O 준비 상태인지 알려주는 역할**을 한다. 실제 데이터 송수신은 `recv()`와 `send()` 등으로 수행한다.

#### C10K 문제

C10K는 약 10,000개의 동시 연결을 효율적으로 처리하는 문제를 의미한다. 연결마다 스레드나 프로세스를 생성하는 방식은 자원 사용량이 커질 수 있다. 따라서 많은 연결을 처리할 때는 이벤트 기반 모델, 논블로킹 I/O, 제한된 버퍼, 연결 타임아웃 등을 고려한다.

#### 이번 과제의 선택

1차 구현은 **pthread 기반 Thread Pool + Blocking I/O**를 사용한다. 필요하면 2차 개선에서 `poll()`이나 OS별 이벤트 API를 적용해 비교한다.

---

## 4. 리소스 동기화 (Synchronization)

### 4.1. 동기화란?

여러 스레드가 공유 자원에 동시에 접근할 때 데이터의 일관성을 유지하도록 제어하는 것이다.

### 4.2. Race Condition (경쟁 상태)

여러 스레드의 실행 순서에 따라 결과가 달라지는 문제다.

```c
int request_count = 0;

request_count++;
```

두 스레드가 동시에 증가 연산을 수행할 경우:

```text
초기값 = 0

Thread A: 0 읽기
Thread B: 0 읽기
Thread A: 1 저장
Thread B: 1 저장

최종값 = 1
기대값 = 2
```

증가 연산이 원자적으로 처리되지 않아 잘못된 결과가 발생할 수 있다.

### 4.3. Mutex

Mutex는 여러 스레드가 공유 자원에 동시에 접근하지 못하도록 상호 배제를 제공하는 동기화 도구다.

```c
pthread_mutex_lock(&mutex);

request_count++;

pthread_mutex_unlock(&mutex);
```

#### Critical Section

Mutex로 보호해야 하는 공유 자원 접근 구간을 임계 영역(Critical Section)이라고 한다.

원칙:

- 필요한 공유 자원만 보호한다.
- 임계 영역을 짧게 유지한다.
- Mutex를 잡은 채 Blocking I/O를 수행하는 상황을 가급적 피한다.

### 4.4. Condition Variable

Condition Variable은 특정 조건이 만족될 때까지 스레드가 효율적으로 대기할 수 있게 하는 도구다.

작업 큐가 비어 있으면 워커는 새 작업을 기다려야 한다.

```c
pthread_mutex_lock(&queue_mutex);

while (queue_empty() && !stopping) {
    pthread_cond_wait(&queue_cond, &queue_mutex);
}

pthread_mutex_unlock(&queue_mutex);
```

`pthread_cond_wait()`는 대기하면서 Mutex를 해제하고, 깨어날 때 다시 획득한다. 반드시 `while`로 조건을 다시 확인해야 한다.

작업을 추가할 때는 다음과 같이 신호를 보낸다.

```c
pthread_cond_signal(&queue_cond);
```

#### Mutex와 Condition Variable의 차이

| 구분 | 역할 |
|---|---|
| Mutex | 공유 데이터에 대한 동시 접근 제어 |
| Condition Variable | 조건 변화까지 대기하고 통지받기 |

Thread Pool에서는 두 가지를 함께 사용한다.

### 4.5. Deadlock (교착 상태)

서로 다른 스레드들이 상대방이 보유한 자원을 기다리면서 진행하지 못하는 상태다.

```text
Thread A              Thread B
   │                     │
Mutex 1 획득         Mutex 2 획득
   │                     │
Mutex 2 대기         Mutex 1 대기
   │                     │
       서로 대기
```

#### 예방 방법

- Mutex 획득 순서를 통일한다.
- 중첩 Lock을 최소화한다.
- 임계 영역을 짧게 유지한다.
- 오류 발생 시에도 Mutex를 해제한다.

### 4.6. 웹서버에서 동기화가 필요한 자원

| 자원 | 처리 방법 |
|---|---|
| 공유 작업 큐 | Mutex + Condition Variable |
| 전체 요청 수 | Mutex 또는 Atomic |
| Round-Robin 인덱스 | Mutex 또는 Atomic |
| 공유 종료 상태 | Mutex 또는 Atomic 등 |
| 클라이언트별 요청 버퍼 | 워커 단독 소유 권장 |
| 연결 소켓 fd | 소유권을 명확히 관리 |

**가장 중요한 원칙은 공유하지 않아도 되는 데이터는 공유하지 않는 것이다.**

---

## 5. Reverse Proxy

### 5.1. 프록시란?

프록시는 클라이언트와 서버 사이에서 통신을 중계하는 서버다.

#### Forward Proxy와 Reverse Proxy

| 구분 | Forward Proxy | Reverse Proxy |
|---|---|---|
| 대신하는 대상 | 클라이언트 | 백엔드 서버 |
| 주요 기능 | 외부 접근 중계, 접근 제어 | 라우팅, 로드 밸런싱, 보안 |
| 이번 과제 | 미선택 | 선택 |

### 5.2. Reverse Proxy 동작 과정

```text
브라우저
   │
   │ GET /api/users
   ▼
C Reverse Proxy (:8080)
   │
   ├─ HTTP 요청 파싱
   ├─ URI 확인
   ├─ 백엔드 선택
   │
   ▼
Backend (:8001)
   │
   │ HTTP 응답
   ▼
C Reverse Proxy
   │
   ▼
브라우저
```

브라우저는 프록시의 주소만 알고 있어도 된다. 실제 요청을 처리하는 내부 백엔드 서버를 직접 알 필요가 없다.

### 5.3. accept()와 connect()

Reverse Proxy에서는 서로 다른 두 TCP 연결이 필요하다.

```text
클라이언트
    │
    │ accept()
    ▼
Reverse Proxy
    │
    │ connect()
    ▼
백엔드 서버
```

- `accept()`: 클라이언트 연결 수락
- `connect()`: 프록시가 백엔드 서버에 연결

프록시는 클라이언트 입장에서는 서버이고, 백엔드 입장에서는 클라이언트 역할을 한다.

### 5.4. 라우팅

URI 경로에 따라 요청 처리 위치를 결정할 수 있다.

| 요청 경로 | 처리 대상 |
|---|---|
| `/static/*` | C 서버에서 직접 파일 제공 |
| `/api/*` | Backend 8001 |
| `/health` | C 서버에서 직접 상태 응답 |

#### 요청 전달 시 주의사항

- HTTP 요청 라인과 본문을 정확히 전달한다.
- 부분 송수신을 처리한다.
- Connection 관련 Hop-by-Hop 헤더를 규칙에 따라 처리한다.
- 메시지 길이를 정확히 판단한다.
- 백엔드 연결 및 응답 타임아웃을 설정한다.
- 클라이언트 및 백엔드 소켓을 모두 정리한다.

### 5.5. Round-Robin 로드 밸런싱

여러 백엔드에 요청을 순서대로 배분하는 방식이다.

```text
요청 1 → Backend 1
요청 2 → Backend 2
요청 3 → Backend 1
요청 4 → Backend 2
```

```c
int index = next_backend % backend_count;
next_backend++;
```

스레드 여러 개가 공유하는 `next_backend`는 Mutex 또는 Atomic 연산으로 보호해야 한다.

### 5.6. Reverse Proxy 보안

| 위험 | 대응 방법 |
|---|---|
| SSRF | 등록된 백엔드만 연결 허용 |
| Request Smuggling | 모호한 HTTP 메시지 경계 거부 |
| 큰 HTTP 요청 | 헤더·본문 크기 제한 |
| Slow Client | 읽기 및 연결 타임아웃 |
| 내부 정보 노출 | 불필요한 오류 정보 제거 |
| 백엔드 장애 | 502 / 504 응답 |

특히 클라이언트가 지정한 임의의 호스트로 연결하지 않고, 설정된 백엔드 주소만 사용하도록 구성한다.

---

## 6. 메모리 관리 및 시그널 종료 처리

### 6.1. C 언어의 메모리 관리

C에서는 동적 할당 메모리의 수명을 직접 관리해야 한다.

```c
char *buffer = malloc(4096);

if (buffer == NULL) {
    // 할당 실패 처리
}

// buffer 사용

free(buffer);
```

#### 대표적인 메모리 오류

| 오류 | 설명 |
|---|---|
| Memory Leak | 할당한 메모리를 해제하지 않음 |
| Use After Free | 해제된 메모리에 접근 |
| Double Free | 같은 할당을 두 번 해제 |
| Buffer Overflow | 버퍼 범위를 넘어서 접근 |
| Invalid Free | 잘못된 포인터를 해제 |

### 6.2. 소켓과 메모리는 별도로 정리

- `free()`: 동적으로 할당한 메모리 해제
- `close()`: 소켓 또는 파일 디스크립터 닫기
- `fclose()`: FILE 스트림 닫기

예를 들어 HTTP 요청 처리 중 클라이언트가 연결을 끊더라도 해당 요청에서 사용한 자원을 정리해야 한다.

#### recv() 반환값

| 반환값 | 의미 |
|---|---|
| 양수 | 읽은 바이트 수 |
| 0 | 상대방의 송신 종료에 따른 EOF |
| -1 | 오류 또는 재시도·대기할 상태 |

실패 시 `errno`를 확인하고 `EINTR`, `EAGAIN` 등은 상황에 맞게 처리한다.

### 6.3. Ownership (자원 소유권)

자원마다 누가 정리할지 명확히 결정해야 한다.

```text
Main Thread
    │
    ├─ accept()
    │
    └─ client_fd 작업 큐 전달
                  │
                  ▼
             Worker Thread
                  │
                  ├─ HTTP 요청 처리
                  ├─ 메모리 free()
                  └─ client_fd close()
```

소켓 소유권을 Worker에게 넘겼다면 Main Thread는 같은 소켓을 중복으로 닫지 않아야 한다.

**하나의 자원은 정해진 책임자가 적절한 시점에 한 번 정리한다.**

### 6.4. Signal이란?

시그널은 운영체제 등이 프로세스 또는 스레드에 특정 사건을 알리는 메커니즘이다.

#### 필수 시그널

| 시그널 | 의미 |
|---|---|
| SIGINT | Ctrl+C 등 인터럽트 |
| SIGTERM | 종료 요청 |
| SIGPIPE | 끊긴 소켓 등에 쓰기를 시도할 때 발생 가능 |

#### SIGPIPE 처리

클라이언트가 연결을 끊은 상태에서 서버가 응답을 전송하면 SIGPIPE가 발생할 수 있다. 서버 전체가 종료되는 것을 방지하기 위해 SIGPIPE를 무시하고 `send()` 오류를 검사하는 방식을 사용할 수 있다.

```c
signal(SIGPIPE, SIG_IGN);
```

실제 구현에서는 `sigaction()`이나 플랫폼별 소켓 옵션을 고려할 수 있다.

### 6.5. Graceful Shutdown

**Graceful Shutdown은 서버 종료 요청을 받았을 때 진행 중인 작업과 자원을 안전하게 정리한 뒤 종료하는 방식이다.**

```text
SIGINT / SIGTERM 수신
          ↓
새로운 연결 수락 중단
          ↓
워커 스레드에 종료 알림
          ↓
진행 중인 요청 정리
          ↓
소켓 / 메모리 해제
          ↓
프로세스 종료
```

Thread Pool에서 종료할 때는 Condition Variable의 `broadcast`로 대기 중인 워커를 깨울 수 있다. 단, 시그널 핸들러 안에서 Mutex 조작이나 `pthread_cond_broadcast()`를 직접 호출하지 않아야 한다.

Thread Pool 기반 서버에서는 `pthread_sigmask()`와 `sigwait()`를 활용하는 전용 종료 처리 스레드 방식도 고려할 수 있다.

---

## 7. 실제 구현 순서

사전 학습을 마쳤다면 다음 순서로 구현한다.

| 단계 | 구현 내용 | 확인할 사항 |
|---|---|---|
| 1 | TCP Echo Server | socket, bind, listen, accept |
| 2 | 기본 HTTP 서버 | GET 요청에 200 응답 |
| 3 | HTTP 파서 | 요청 라인, 헤더, 본문 처리 |
| 4 | 정적 파일 서버 | HTML/CSS/JS/이미지 및 오류 응답 |
| 5 | 동적 요청 | POST, /api/echo 등 |
| 6 | Reverse Proxy | connect, 요청·응답 중계 |
| 7 | Thread Pool | 작업 큐, Mutex, Condition Variable |
| 8 | 안정화 | 타임아웃, 종료 처리, 메모리 검사 |
| 9 | 고도화 | Keep-Alive, chunked, 로드 밸런싱 |
| 10 | 성능 테스트 | 처리량, 지연시간, 동시 연결 평가 |

### 개발 시 사용할 테스트 도구

| 도구 | 용도 |
|---|---|
| curl | HTTP 요청과 응답 확인 |
| nc | TCP 연결 및 수동 요청 테스트 |
| GDB / LLDB | C 코드 디버깅 |
| AddressSanitizer | 잘못된 메모리 접근 탐지 |
| Valgrind | Linux 환경 메모리 분석 |
| wrk / hey / ab | HTTP 부하 테스트 |

---

## 8. 최종 핵심 개념 한 줄 정리

| 개념 | 핵심 |
|---|---|
| Socket | 프로그램의 네트워크 통신 인터페이스 |
| TCP | 신뢰성 있는 바이트 스트림 전송 프로토콜 |
| File Descriptor | 프로세스가 열린 자원을 참조하는 정수 |
| accept() | TCP 연결을 수락하고 새로운 소켓 반환 |
| connect() | 다른 서버에 연결 |
| HTTP/1.1 | 요청 및 응답의 구조와 의미를 정의 |
| Content-Length | HTTP 본문 길이 |
| Keep-Alive | TCP 연결 재사용 |
| Concurrency | 여러 작업의 진행을 겹쳐 관리 |
| Thread Pool | 워커 스레드를 미리 생성해 재사용 |
| epoll | Linux의 I/O 이벤트 감시 API |
| Mutex | 공유 자원에 대한 상호 배제 |
| Condition Variable | 특정 조건을 기다리거나 통지 |
| Race Condition | 실행 순서에 따라 결과가 달라지는 문제 |
| Deadlock | 서로 자원을 기다려 진행이 멈추는 상태 |
| Reverse Proxy | 백엔드 서버를 대신해 요청을 받아 전달 |
| Load Balancing | 여러 백엔드에 요청 분산 |
| Ownership | 자원 정리 책임을 명확히 하는 것 |
| SIGPIPE | 끊긴 연결 등에 쓰기 시도 시 발생할 수 있는 시그널 |
| Graceful Shutdown | 진행 중인 작업과 자원을 안전하게 정리하며 종료 |

---

## 최종 결론

이번 과제는 단순히 HTTP 요청에 응답하는 C 프로그램이 아니라 다음을 직접 설계하고 구현하는 프로젝트다.

**TCP 소켓 연결 → HTTP/1.1 요청 파싱 → 라우팅 → 정적 파일 처리 또는 Reverse Proxy → HTTP 응답 → 자원 정리**

여기에 **Thread Pool 기반 동시성, Mutex/Condition Variable 동기화, 안전한 메모리 관리 및 Graceful Shutdown**을 추가한다.

최종 목표는 모든 기능이 동작하는 것뿐 아니라, 각 기술을 왜 선택했으며 어떤 장단점과 한계가 있는지 설명할 수 있는 것이다.
