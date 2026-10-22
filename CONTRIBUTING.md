# Contributing

Thanks for looking at keel. Small, focused changes are the easiest to review.

## Build and test

```bash
make
make test
make asan
```

`make test` runs the unit tests and `tests/integration.c`, which starts `./keel`. `make asan` repeats that build with AddressSanitizer and UndefinedBehaviorSanitizer. Run both before opening a pull request.

GCC and Clang should both build cleanly with the flags in the Makefile.

## Commits

Use [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/):

```text
type(scope): short summary

Why the change exists, when that is not obvious from the summary.
```

Sign off with `git commit -s`. Keep each commit buildable.

## Scope

keel is a static file server. Changes that add TLS, HTTP/2, proxying, or CGI need a discussion first, because they move the program into a different job. Bug fixes, clearer errors, and tighter tests are always welcome.
