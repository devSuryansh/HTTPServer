# Security

## What keel protects

keel serves files only from the configured document root. Paths are normalized and opened relative to that directory. A request that would leave the root is rejected, including `..`, encoded dot segments, encoded slashes, and a symlink whose target is outside the root. A NUL in a percent-encoded path is rejected.

Each connection has a fixed header buffer, a header timeout, an idle timeout, a write timeout, and a cap on requests. When a worker's connection table is full, further connections are refused. Directory listings are off unless `directory_listing` is turned on. Request bodies are not read.

## What keel does not do

keel does not terminate TLS, authenticate clients, or isolate sites from each other beyond the single document root and the optional `server_name` check. Do not expose it directly on an untrusted network. Terminate TLS on a reverse proxy.

## Reporting

Please report a vulnerability to the maintainer privately, out of band, before opening a public issue. Non-sensitive defects can be filed as a normal bug report.
