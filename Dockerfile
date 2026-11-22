# 빌드·테스트용 Linux 환경. 코드는 이미지에 넣지 않고 실행할 때 폴더를 마운트한다.
#   docker build -t tiny-web-server .
#   docker run --rm -it -v "${PWD}:/app" -p 8080:8080 tiny-web-server
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential gdb valgrind \
        curl netcat-openbsd apache2-utils wrk \
        procps lsof iproute2 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
CMD ["bash"]
