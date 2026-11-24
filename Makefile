CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic
CLANG ?= clang
PYTHON ?= python3

SANITIZER_FLAGS = -g -O1 -Wshadow -fsanitize=address,undefined -fno-sanitize-recover=all

SOURCES = src/main.c src/tcp_server.c src/http_request.c src/http_response.c src/static_file.c
HEADERS = src/tcp_server.h src/http_request.h src/http_response.h src/static_file.h

tiny-web-server: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

.PHONY: analyze test test-sanitize clean
analyze:
	$(CLANG) --analyze $(CPPFLAGS) $(CFLAGS) -Wshadow -Xanalyzer -analyzer-output=text $(SOURCES)

test: tiny-web-server
	$(PYTHON) tests/test_tcp_echo.py ./tiny-web-server
	$(PYTHON) tests/test_http.py ./tiny-web-server

test-sanitize:
	@set -eu; \
	build_dir=$$(mktemp -d); \
	trap 'rm -rf "$$build_dir"' EXIT; \
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZER_FLAGS) $(LDFLAGS) -o "$$build_dir/tiny-web-server" $(SOURCES) $(LDLIBS); \
	$(PYTHON) tests/test_tcp_echo.py "$$build_dir/tiny-web-server"; \
	$(PYTHON) tests/test_http.py "$$build_dir/tiny-web-server"

clean:
	rm -f tiny-web-server
