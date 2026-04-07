// proj2_server.cc
// CSCE 311 Project 2 - Spring 2026

#include "proj2/lib/sha_solver.h"
#include "proj2/lib/file_reader.h"
#include "proj2/lib/domain_socket.h"
#include "proj2/lib/thread_log.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>

#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

// ---------------------------------------------------------------------------
// Termination flag
// ---------------------------------------------------------------------------
static volatile sig_atomic_t g_terminate = 0;

static void signal_handler(int /*sig*/) {
    g_terminate = 1;
}

// ---------------------------------------------------------------------------
// Per-request data passed to worker thread
// ---------------------------------------------------------------------------
struct Request {
    std::string              reply_endpoint;
    std::vector<std::string> file_paths;
    std::vector<uint32_t>    row_counts;
};

// ---------------------------------------------------------------------------
// Binary protocol parsing helpers
// ---------------------------------------------------------------------------
static bool read_u32(const char *buf, size_t buf_len, size_t &offset,
                     uint32_t &out) {
    if (offset + 4 > buf_len) return false;
    std::memcpy(&out, buf + offset, 4);
    offset += 4;
    return true;
}

static bool read_string(const char *buf, size_t buf_len, size_t &offset,
                        uint32_t len, std::string &out) {
    if (offset + len > buf_len) return false;
    out.assign(buf + offset, len);
    offset += len;
    return true;
}

static bool parse_datagram(const char *buf, size_t buf_len, Request &req) {
    size_t   offset = 0;
    uint32_t tmp_len = 0;

    if (!read_u32(buf, buf_len, offset, tmp_len)) return false;
    if (!read_string(buf, buf_len, offset, tmp_len, req.reply_endpoint))
        return false;

    uint32_t file_count = 0;
    if (!read_u32(buf, buf_len, offset, file_count)) return false;

    req.file_paths.resize(file_count);
    req.row_counts.resize(file_count);

    for (uint32_t i = 0; i < file_count; ++i) {
        if (!read_u32(buf, buf_len, offset, tmp_len)) return false;
        if (!read_string(buf, buf_len, offset, tmp_len, req.file_paths[i]))
            return false;
        if (!read_u32(buf, buf_len, offset, req.row_counts[i])) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------
static void *handle_request(void *arg) {
    Request *req = static_cast<Request *>(arg);

    uint32_t num_files  = static_cast<uint32_t>(req->file_paths.size());
    uint32_t max_rows   = 0;
    uint32_t total_rows = 0;
    for (uint32_t rc : req->row_counts) {
        if (rc > max_rows) max_rows = rc;
        total_rows += rc;
    }

    // ------------------------------------------------------------------
    // Deadlock prevention:
    //   Always acquire solvers FIRST (max row count across all files),
    //   then acquire readers. This strict ordering prevents circular waits.
    // ------------------------------------------------------------------

    // 1. Checkout solvers (blocks until max_rows solvers are free)
    proj2::SolverHandle solver_handle = proj2::ShaSolvers::Checkout(max_rows);

    // 2. Checkout readers (blocks until num_files readers are free)
    //    Pass a pointer to the already-held solver handle.
    proj2::ReaderHandle reader_handle =
        proj2::FileReaders::Checkout(num_files, &solver_handle);

    // 3. Process all files — result is a 2D vector: [file][row] = 64-char hash
    std::vector<std::vector<proj2::ReaderHandle::HashType>> file_hashes;
    reader_handle.Process(req->file_paths, req->row_counts, &file_hashes);

    // 4. Release readers then solvers (std::move rescinds ownership)
    proj2::FileReaders::Checkin(std::move(reader_handle));
    proj2::ShaSolvers::Checkin(std::move(solver_handle));

    // 5. Flatten into one contiguous byte string, ordered by file
    std::string result;
    result.reserve(total_rows * 64);
    for (auto &file_vec : file_hashes) {
        for (auto &hash : file_vec) {
            result.append(hash.data(), 64);
        }
    }

    // 6. Connect back to client's reply stream socket and send all hashes
    proj2::UnixDomainStreamClient client(req->reply_endpoint);
    client.Init();  // connect
    client.Write(result.data(), result.size());

    delete req;
    return nullptr;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <socket_path> <num_readers> <num_solvers>\n";
        return 1;
    }

    const char *socket_path = argv[1];
    int num_readers = std::atoi(argv[2]);
    int num_solvers = std::atoi(argv[3]);

    if (num_readers <= 0 || num_solvers <= 0) {
        std::cerr << "Reader and solver counts must be positive integers.\n";
        return 1;
    }

    // Install signal handlers — no unsafe ops inside the handler
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Initialize resource pools exactly once at startup
    proj2::ShaSolvers::Init(static_cast<uint32_t>(num_solvers));
    proj2::FileReaders::Init(static_cast<uint32_t>(num_readers));

    // Bind the server datagram socket
    proj2::UnixDomainDatagramEndpoint server(socket_path);
    server.Init();

    const size_t BUF_SIZE = 65536;

    while (!g_terminate) {
        std::string peer_path;
        std::string datagram = server.RecvFrom(&peer_path, BUF_SIZE);

        if (datagram.empty()) {
            if (g_terminate) break;
            continue;
        }

        Request *req = new Request();
        if (!parse_datagram(datagram.data(), datagram.size(), *req)) {
            std::cerr << "Failed to parse datagram\n";
            delete req;
            continue;
        }

        // Spawn a detached thread per request
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        if (pthread_create(&tid, &attr, handle_request, req) != 0) {
            perror("pthread_create");
            delete req;
        }
        pthread_attr_destroy(&attr);
    }

    return 0;
}