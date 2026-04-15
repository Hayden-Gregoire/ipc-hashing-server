# CSCE 311 — Project 2

Unix domain sockets, binary request framing, pthread workers, and pooled SHA / file-reader resources (`proj2lib`).

## Build

Requires **Linux** (or a Linux devcontainer / Codespace) so `lib/proj2lib.a` links.

```bash
cd proj2
make
```

Produces `bin/proj2-server`.

## Run the server

Arguments: **datagram socket path**, **number of reader slots**, **number of solver slots**.

```bash
./bin/proj2-server /tmp/proj2.sock 16 32
```

- Use **enough solvers** so `max_rows` in any request is covered (often **≥** the largest row count in your `dat/` files; nested solver use inside the library may need **roughly 2×** that in practice).
- Use **enough readers** so the **file count** in a single request never exceeds the pool (stress scripts may send many files at once).

The server uses **abstract** Unix addresses for both the datagram endpoint and the stream reply path, matching the provided `proj2-client`.

Stop with **Ctrl+C** (SIGINT) or **SIGTERM**; the process waits for in-flight worker threads to finish before exiting.

## Run the client (separate terminal)

From the same `proj2` directory:

```bash
./bin/proj2-client /tmp/my_reply /tmp/proj2.sock dat/low1.dat
```

Order: **reply socket name**, **server datagram socket**, then **one or more** data files under `dat/`.

## Optional scripts

- `bash lib/monitor_server.bash <socket> <readers> <solvers>` — run the server with parsed steady-state output.
- `bash lib/send_requests.bash` — load generator (see script for arguments).

## Clean

```bash
make clean
```
