# Concurrent IPC Hashing Server

A multithreaded client-server system built in C++ using Unix domain sockets for
inter-process communication (IPC). The server processes file-based requests and
returns SHA-256 hashes using a concurrent worker model.

## Overview

- Handles requests via Unix domain **datagram sockets**
- Sends responses using **stream sockets**
- Uses **pthread-based worker threads** for concurrent processing
- Implements a **custom binary protocol** for request parsing
- Uses pooled **reader and solver resources** for efficiency
- Enforces ordering of resource acquisition to **prevent deadlock**
- Supports **graceful shutdown** with active worker tracking

## My Contributions

- Implemented the multithreaded server logic within a provided systems framework
- Built request parsing and handling for a custom binary IPC protocol
- Designed concurrent request processing using pthread-based workers
- Coordinated pooled resources (file readers and SHA solvers)
- Enforced deadlock-safe execution through ordered resource acquisition
- Implemented signal handling and graceful shutdown logic

## Build

Requires Linux (or a Linux devcontainer):

```bash
make
```

## Run the Server

```bash
./bin/proj2-server /tmp/proj2.sock <num_readers> <num_solvers>
```

## Run the Client

```bash
./bin/proj2-client /tmp/reply.sock /tmp/proj2.sock dat/low1.dat
```
