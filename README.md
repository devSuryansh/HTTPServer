# keel

keel is an HTTP/1.1 server for static files. It is written in C for Linux.

## Build

```bash
make
```

The binary is `./keel`. `make clean` removes it and the object tree under `build/`.

## Run

```bash
./keel
```

keel listens on `0.0.0.0:8080`. A well formed request receives an empty `200 OK`. A malformed request receives the matching status, and the connection closes.

## Test

```bash
make test
```
