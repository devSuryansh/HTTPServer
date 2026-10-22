# keel

keel is an HTTP/1.1 server for static files. It is written in C for Linux.

## Build

```bash
make
```

The binary is `./keel`. `make clean` removes it and the object tree under `build/`.

## Run

```bash
./keel --root ./public --port 8080
```

keel serves files from `--root` (the current directory by default) on `--port` (8080 by default). `GET` and `HEAD` return the file. `/` serves `index.html`. A path that leaves the document root is rejected. Other methods receive `405`.

## Test

```bash
make test
```
