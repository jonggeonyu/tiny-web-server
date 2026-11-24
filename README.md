# Tiny Web Server

정글 SW-AI 13주차 C 웹 서버 과제다. Linux `epoll`로 여러 TCP 연결을 처리한다. HTTP 모드는 문서 루트의 파일을 제공하고 `POST /api/echo`에서 요청 본문을 돌려준다.

HTTP/1.1 연결은 정상 응답 뒤에 재사용한다. `Connection: close`가 있는 요청과 오류 응답은 송신 후 연결을 닫는다. 정상 응답 후 새 요청이 5초 안에 시작되지 않아도 연결을 닫는다.

## 빌드와 실행

Linux 또는 Docker의 Linux 컨테이너에서 실행한다.

```sh
make
root=$(mktemp -d)
printf '<h1>Hello</h1>\n' > "$root/index.html"
./tiny-web-server 8080 "$root"
```

서버와 같은 Linux 환경의 다른 터미널에서 요청을 보낸다. Docker를 사용한다면 같은 컨테이너에서 명령을 실행한다.

```sh
curl -i http://127.0.0.1:8080/
curl -i --data-binary 'hello' http://127.0.0.1:8080/api/echo
```

포트만 넘기면 TCP Echo 모드로 실행된다. 서버를 종료하려면 서버 터미널에서 `Ctrl+C`를 누른다.

## 검증

Linux에서 `make test`로 Echo와 HTTP를 실제 TCP 연결로 검증한다. `make test-sanitize`는 AddressSanitizer와 UndefinedBehaviorSanitizer를 켠 빌드에서 같은 검증을 실행한다.

Keep-Alive 테스트는 같은 연결에서 GET과 POST를 여러 번 보내 순서대로 응답을 읽는다. 미리 도착한 요청과 유휴 종료도 확인한다.

[이벤트 루프와 HTTP 설계](dev-notes/http-epoll-design.md)에서 연결별 상태, 크기 한도, 오류 처리와 검증 기준을 확인할 수 있다.

실행 파일을 지우려면 `make clean`을 실행한다.
