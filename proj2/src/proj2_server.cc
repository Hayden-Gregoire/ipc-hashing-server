// proj2_server.cc
// CSCE 311 Project 2 - Spring 2026
#include "proj2/lib/sha_solver.h"
#include "proj2/lib/file_reader.h"
#include "proj2/lib/domain_socket.h"

#include <sys/socket.h>
#include <signal.h>
#include <pthread.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <atomic>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Termination flag — set only inside signal handler
// ---------------------------------------------------------------------------
static volatile sig_atomic_t g_terminate = 0;

static std::atomic<int> g_active_workers{0};

struct ActiveWorker {
    ActiveWorker() { ++g_active_workers; }
    ~ActiveWorker() { --g_active_workers; }
};

// Pool sizes from argv (Init); Checkout must not exceed these or it blocks forever.
static uint32_t g_num_solvers = 0;
static uint32_t g_num_readers = 0;

static void signal_handler(int /*sig*/) { g_terminate = 1; }

// ---------------------------------------------------------------------------
// Per-request data passed to worker thread
// ---------------------------------------------------------------------------
struct Request {
    std::string              reply_endpoint;
    std::vector<std::string> file_paths;
    std::vector<uint32_t>    row_counts;
};

// ---------------------------------------------------------------------------
// Binary protocol parsing
// ---------------------------------------------------------------------------
static bool read_u32(const char *buf, size_t buf_len, size_t &off,
                     uint32_t &out) {
    if (off + 4 > buf_len) return false;
    std::memcpy(&out, buf + off, 4);
    off += 4;
    return true;
}

static bool read_str(const char *buf, size_t buf_len, size_t &off,
                     uint32_t len, std::string &out) {
    if (off + len > buf_len) return false;
    out.assign(buf + off, len);
    off += len;
    return true;
}

static bool parse_datagram(const char *buf, size_t buf_len, Request &req) {
    size_t   off = 0;
    uint32_t slen = 0;

    if (!read_u32(buf, buf_len, off, slen))                        return false;
    if (!read_str(buf, buf_len, off, slen, req.reply_endpoint))    return false;

    uint32_t nfiles = 0;
    if (!read_u32(buf, buf_len, off, nfiles))                      return false;

    req.file_paths.resize(nfiles);
    req.row_counts.resize(nfiles);

    for (uint32_t i = 0; i < nfiles; ++i) {
        if (!read_u32(buf, buf_len, off, slen))                        return false;
        if (!read_str(buf, buf_len, off, slen, req.file_paths[i]))     return false;
        if (!read_u32(buf, buf_len, off, req.row_counts[i]))           return false;
    }
    return true;
}

// Best-effort: open reply stream so client can observe EOF instead of hanging forever.
static void reject_reply_open_only(const std::string &reply_endpoint) {
    proj2::UnixDomainStreamClient reply_sock(reply_endpoint);
    reply_sock.Init();
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------
static void *handle_request(void *arg) {
    ActiveWorker  active;
    Request      *req = static_cast<Request *>(arg);

    uint32_t num_files  = static_cast<uint32_t>(req->file_paths.size());
    uint32_t max_rows   = 0;
    uint32_t total_rows = 0;
    for (uint32_t rc : req->row_counts) {
        if (rc > max_rows) max_rows = rc;
        total_rows += rc;
    }

    if (max_rows > g_num_solvers) {
        reject_reply_open_only(req->reply_endpoint);
        delete req;
        return nullptr;
    }
    if (num_files > g_num_readers) {
        reject_reply_open_only(req->reply_endpoint);
        delete req;
        return nullptr;
    }

    // Deadlock prevention: solvers first, then readers.
    proj2::SolverHandle solver_handle =
        proj2::ShaSolvers::Checkout(max_rows);

    proj2::ReaderHandle reader_handle =
        proj2::FileReaders::Checkout(num_files, &solver_handle);

    std::vector<std::vector<proj2::ReaderHandle::HashType>> file_hashes;
    file_hashes.resize(num_files);
    reader_handle.Process(req->file_paths, req->row_counts, &file_hashes);

    proj2::FileReaders::Checkin(std::move(reader_handle));
    proj2::ShaSolvers::Checkin(std::move(solver_handle));

    // Flatten hashes (file order, row order within file)
    std::string result;
    result.reserve(static_cast<size_t>(total_rows) * 64);
    for (auto &file_vec : file_hashes)
        for (auto &hash : file_vec)
            result.append(hash.data(), 64);

    proj2::UnixDomainStreamClient reply_sock(req->reply_endpoint);
    reply_sock.Init();

    const char *ptr = result.data();
    size_t      remaining = result.size();
    while (remaining > 0) {
        std::size_t n = reply_sock.Write(ptr, remaining);
        if (n == 0) {
            perror("write to client reply socket");
            break;
        }
        ptr += n;
        remaining -= n;
    }

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
    int         num_readers   = std::atoi(argv[2]);
    int         num_solvers   = std::atoi(argv[3]);

    if (num_readers <= 0 || num_solvers <= 0) {
        std::cerr << "Reader and solver counts must be positive integers.\n";
        return 1;
    }

    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    g_num_solvers = static_cast<uint32_t>(num_solvers);
    g_num_readers = static_cast<uint32_t>(num_readers);

    proj2::ShaSolvers::Init(g_num_solvers);
    proj2::FileReaders::Init(g_num_readers);

    ::unlink(socket_path);

    const std::string          socket_path_str(socket_path);
    proj2::UnixDomainDatagramEndpoint dgram(socket_path_str);
    dgram.Init();

    const size_t BUF_SIZE = 65536;
    char         buf[BUF_SIZE];

    while (!g_terminate) {
        struct pollfd pfd{};
        pfd.fd     = dgram.socket_fd();
        pfd.events = POLLIN;

        int pr = poll(&pfd, 1, 250);
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (g_terminate) break;
        if (pr == 0) continue;

        if (pfd.revents & (POLLERR | POLLNVAL)) break;
        if (!(pfd.revents & POLLIN)) continue;

        ssize_t bytes = ::recv(dgram.socket_fd(), buf, BUF_SIZE, 0);
        if (bytes < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            continue;
        }
        if (bytes == 0) continue;

        Request *req = new Request();
        if (!parse_datagram(buf, static_cast<size_t>(bytes), *req)) {
            delete req;
            continue;
        }

        pthread_t      tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        if (pthread_create(&tid, &attr, handle_request, req) != 0) {
            perror("pthread_create");
            delete req;
        }
        pthread_attr_destroy(&attr);
    }

    while (g_active_workers.load(std::memory_order_acquire) > 0) {
        struct timespec ts;
        ts.tv_sec  = 0;
        ts.tv_nsec = 10L * 1000 * 1000;
        nanosleep(&ts, nullptr);
    }

    ::unlink(socket_path);
    return 0;
}
