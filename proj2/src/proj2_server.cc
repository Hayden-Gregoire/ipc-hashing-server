// proj2_server.cc
// CSCE 311 Project 2 - Spring 2026

#include "proj2/lib/sha_solver.h"
#include "proj2/lib/file_reader.h"
#include "proj2/lib/domain_socket.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>

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

    std::cerr << "[worker] reply_endpoint=" << req->reply_endpoint
              << " num_files=" << num_files
              << " max_rows=" << max_rows << "\n";

    // Deadlock prevention: solvers first, then readers
    proj2::SolverHandle solver_handle =
        proj2::ShaSolvers::Checkout(max_rows);

    proj2::ReaderHandle reader_handle =
        proj2::FileReaders::Checkout(num_files, &solver_handle);

    std::vector<std::vector<proj2::ReaderHandle::HashType>> file_hashes;
    reader_handle.Process(req->file_paths, req->row_counts, &file_hashes);

    proj2::FileReaders::Checkin(std::move(reader_handle));
    proj2::ShaSolvers::Checkin(std::move(solver_handle));

    // Flatten hashes
    std::string result;
    result.reserve(static_cast<size_t>(total_rows) * 64);
    for (auto &file_vec : file_hashes)
        for (auto &hash : file_vec)
            result.append(hash.data(), 64);

    std::cerr << "[worker] sending " << result.size() << " bytes to "
              << req->reply_endpoint << "\n";

    // Connect to client reply socket
    int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); delete req; return nullptr; }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, req->reply_endpoint.c_str(),
                 sizeof(addr.sun_path) - 1);

    if (::connect(sock,
                  reinterpret_cast<struct sockaddr *>(&addr),
                  sizeof(addr)) < 0) {
        perror("connect to client reply socket");
        ::close(sock);
        delete req;
        return nullptr;
    }

    const char *ptr = result.data();
    size_t remaining = result.size();
    while (remaining > 0) {
        ssize_t n = ::write(sock, ptr, remaining);
        if (n <= 0) { perror("write"); break; }
        ptr       += n;
        remaining -= static_cast<size_t>(n);
    }

    ::close(sock);
    std::cerr << "[worker] done\n";
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

    // Signal handlers
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Initialize resource pools
    proj2::ShaSolvers::Init(static_cast<uint32_t>(num_solvers));
    proj2::FileReaders::Init(static_cast<uint32_t>(num_readers));

    // Remove stale socket
    ::unlink(socket_path);

    // Create datagram socket
    int server_fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un server_addr{};
    server_addr.sun_family = AF_UNIX;
    std::strncpy(server_addr.sun_path, socket_path,
                 sizeof(server_addr.sun_path) - 1);

    if (::bind(server_fd,
               reinterpret_cast<struct sockaddr *>(&server_addr),
               sizeof(server_addr)) < 0) {
        perror("bind");
        ::close(server_fd);
        return 1;
    }

    // Verify socket type after bind
    struct stat st;
    if (::stat(socket_path, &st) == 0) {
        std::cerr << "[server] bound to " << socket_path
                  << " (is socket: " << S_ISSOCK(st.st_mode) << ")\n";
    }

    std::cerr << "[server] listening for datagrams on " << socket_path << "\n";

    const size_t BUF_SIZE = 65536;
    char buf[BUF_SIZE];

    while (!g_terminate) {
        std::cerr << "[server] waiting for datagram...\n";
        ssize_t bytes = ::recv(server_fd, buf, BUF_SIZE, 0);

        if (bytes < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            continue;
        }
        if (bytes == 0) continue;

        std::cerr << "[server] received datagram of " << bytes << " bytes\n";

        Request *req = new Request();
        if (!parse_datagram(buf, static_cast<size_t>(bytes), *req)) {
            std::cerr << "Failed to parse datagram (" << bytes << " bytes)\n";
            delete req;
            continue;
        }

        std::cerr << "[server] parsed request: reply=" << req->reply_endpoint
                  << " files=" << req->file_paths.size() << "\n";

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

    ::close(server_fd);
    ::unlink(socket_path);
    return 0;
}