#!/usr/bin/env python3
"""
fake_backend.py: 프록시 e2e 테스트용 "고장 난 백엔드".

  python3 -I tests/fake_backend.py 모드 포트 기록파일

  hang  : 요청을 받고 아무 답도 하지 않는다 (프록시 응답 시간 초과 → 504 확인용)
  close : 요청 머리를 다 받은 뒤 답 없이 연결을 끊는다
          (보낸 뒤 끊김 → GET 은 다음 백엔드로 재시도, POST 는 502 확인용)

받은 요청마다 기록파일에 요청 라인 한 줄을 남긴다 → "POST 가 두 번 오지 않았다"를 셀 수 있다.
"""
import socket
import sys
import threading


def read_head(conn):
    data = b""
    while b"\r\n\r\n" not in data and len(data) < 65536:
        chunk = conn.recv(4096)
        if not chunk:
            break
        data += chunk
    return data


def handle(conn, mode, log_path):
    try:
        head = read_head(conn)
        line = head.split(b"\r\n", 1)[0].decode("latin-1")
        with open(log_path, "a") as f:
            f.write(line + "\n")
        if mode == "hang":
            threading.Event().wait(60)  # 답하지 않고 붙잡고 있기
    finally:
        conn.close()


def main():
    mode, port, log_path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(16)
    open(log_path, "w").close()
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=handle, args=(conn, mode, log_path), daemon=True).start()


if __name__ == "__main__":
    main()
