# keel

keel is an HTTP/1.1 server for static files. It is written in C for Linux. It uses nonblocking I/O, keeps each connection in a fixed amount of memory, and serves files only from inside a chosen document root.

## Features

- `GET`, `HEAD`, and `OPTIONS`, with `405` for anything else
- HTTP/1.0 and HTTP/1.1, including persistent connections
- Directory index files, optional directory listings, and custom error pages
- Single byte ranges, `ETag`, and `Last-Modified`
- Precompressed `.gz` siblings when the client accepts gzip
- One worker per CPU, sharing a single listening socket
- Combined access log, leveled error log, and `SIGHUP` to reopen both
- `/_keel/health` and `/_keel/metrics`

TLS belongs on a reverse proxy in front of keel. keel speaks cleartext HTTP/1.1 only.

## Build

```bash
make
make test
```

The binary is `./keel`. `make asan` rebuilds with AddressSanitizer and UndefinedBehaviorSanitizer and runs the tests. `make clean` removes the binary and the `build/` tree.

Compilers: GCC or Clang, C11, pthreads. The system is Linux (`epoll`, `sendfile`, `openat2`).

Install into `/usr/local` (override with `PREFIX` and `DESTDIR`):

```bash
make install
```

## Run

```bash
./keel --root examples/site
```

Then open `http://127.0.0.1:8080/`. Useful flags:

```text
--config FILE
--bind ADDR
--port PORT
--root DIR
--workers N
--access-log PATH
--error-log PATH
-h, --help
-v, --version
```

Flags override the same keys in the config file. `port 0` asks the kernel for an ephemeral port. The chosen address is printed on startup.

`SIGINT` and `SIGTERM` stop new connections, drop idle ones, and finish in-flight responses for up to five seconds. `SIGHUP` reopens the log files. Changing the bind address, port, or document root requires a restart.

## Configuration

The file is `key value` lines. Blank lines and lines starting with `#` are ignored. Quote a value that contains spaces. See [keel.conf.example](keel.conf.example).

| Key | Default | Meaning |
| --- | --- | --- |
| `bind` | `0.0.0.0` | Listen address. `::` is accepted. |
| `port` | `8080` | Listen port. |
| `root` | `.` | Document root. Must be a directory. |
| `workers` | CPU count | Event-loop threads. `0` selects the CPU count. |
| `max_connections` | `1024` | Connections per worker. |
| `header_timeout_ms` | `5000` | Time allowed to finish request headers. |
| `keepalive_timeout_ms` | `10000` | Idle time between requests. |
| `write_timeout_ms` | `30000` | Time allowed with no write progress. |
| `max_header_bytes` | `8192` | Maximum request header block. |
| `max_requests_per_conn` | `1000` | Requests served before the connection closes. |
| `index` | `index.html` | File served for a directory URL. |
| `directory_listing` | `off` | List a directory when the index file is absent. |
| `server_name` | empty | If set, `Host` must match this name. |
| `server_header` | `keel` | Value of the `Server` header. Empty omits it. |
| `access_log` | stdout | Combined access log path. |
| `error_log` | stderr | Error log path. |
| `log_level` | `info` | `error`, `warn`, `info`, or `debug`. |

## HTTP behavior

A well formed `GET` or `HEAD` for a file returns `200` with `Content-Type`, `Content-Length`, `ETag`, `Last-Modified`, and `Accept-Ranges: bytes`. `HEAD` sends the same headers and no body. A directory URL without a trailing slash is redirected to one. `/` and other directory URLs serve the index file. If that file is missing, the response is `403` unless directory listing is on.

One `Range: bytes=...` interval returns `206`. An unsatisfiable range returns `416`. Several intervals in one header are ignored and the full file is returned. `If-None-Match` and `If-Modified-Since` return `304` when the file has not changed.

`OPTIONS *` and `OPTIONS` on any path return `200` with `Allow: GET, HEAD, OPTIONS`. HTTP/1.1 requests without `Host` return `400`. Request bodies are refused with `501` and the connection is closed. Header blocks that exceed the cap return `431`. A header that does not finish in time returns `408`.

If `errors/<status>.html` exists under the document root, that file is the body of that status. Otherwise keel uses a short built-in page.

When the client sends `Accept-Encoding: gzip` and `path.gz` exists beside `path`, keel sends the compressed file with `Content-Encoding: gzip` and the original media type.

`/_keel/health` returns `ok`. `/_keel/metrics` returns counters for uptime, active connections, requests, status classes, and bytes sent.

## Security

keel opens files through the document-root directory and rejects a path that would leave it, including encoded dot segments, encoded slashes, and symlinks that point outside. `%00` is rejected. Directory listing is off unless configured. Each connection has a fixed header buffer, timeouts, and a request cap. A full connection table is refused.

keel does not terminate TLS. Put it behind a proxy when clients are not on a trusted network. It does not implement HTTP/2, proxying, CGI, or WebSockets.

## Development

```bash
make test
make asan
```

Unit tests live in `tests/test_*.c`. `tests/integration.c` starts the binary and checks the protocol. See [CONTRIBUTING.md](CONTRIBUTING.md) and [SECURITY.md](SECURITY.md).

## License

MIT. See [LICENSE](LICENSE).

Credits: [Jeffrey Yu](https://github.com/JeffreytheCoder) and [Karan Kumar](https://github.com/puang59)
