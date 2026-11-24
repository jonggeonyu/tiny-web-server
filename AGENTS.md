# Tiny Web Server

- Build with `make`. Run `make analyze` for Clang static analysis.
- Run `make test` for TCP integration checks. Run `make test-sanitize CC=clang` for the same checks under AddressSanitizer and UndefinedBehaviorSanitizer. The tests require Python 3 and permission to bind a local loopback port.
- Keep ownership of each socket and allocation explicit. For changes to TCP I/O, inspect short reads and writes, retryable errors such as `EINTR`, peer EOF, and cleanup on failure.
- Test byte-for-byte echo and connection lifecycle through loopback. Use a check that observes a specific syscall outcome when the change depends on that outcome. Add HTTP request-limit checks when the project introduces an HTTP parser.
