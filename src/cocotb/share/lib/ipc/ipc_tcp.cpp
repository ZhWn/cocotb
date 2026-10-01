// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// TCP loopback transport for the cocotb IPC layer.
//
// The simulator process listens on an ephemeral TCP port on the loopback
// interface and the Python child process connects to it. Messages are
// length-prefixed frames `[u32 LE length][payload]`; TCP_NODELAY is enabled
// on both sockets (the Python side too) so small control messages are not
// stalled by Nagle's algorithm.

#include "./ipc_base.hpp"

#include "../gpi/logging.hpp"  // LOG_WARN

#include <chrono>
#include <cstdint>
#include <cstdlib>  // getenv
#include <cstring>  // strcmp
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>  // TCP_NODELAY
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cocotb {
namespace ipc {

namespace {

#ifdef _WIN32
typedef SOCKET socket_t;
const socket_t kInvalidSocket = INVALID_SOCKET;
#else
typedef int socket_t;
const socket_t kInvalidSocket = -1;
#endif

// Budget for the spin phase in wait_readable(): a bounded poll before
// falling back to a blocking select(), trading a little CPU for lower
// per-message latency on loopback round-trips.
constexpr std::chrono::microseconds kSpinBudget{50};

bool wsa_started = false;

bool ensure_wsa() {
#ifdef _WIN32
    if (!wsa_started) {
        WSADATA wsa_data;
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            return false;
        }
        // Deliberately never WSACleanup(): the transport is a process-wide
        // singleton and cleaning up while sockets may still be closing
        // during process teardown is more trouble than it is worth.
        wsa_started = true;
    }
    return true;
#else
    (void)wsa_started;
    return true;
#endif
}

void close_socket(socket_t s) {
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

class TcpTransport : public IpcTransport {
  public:
    TcpTransport() = default;
    ~TcpTransport() override { close(); }

    bool open() override {
        if (!ensure_wsa()) {
            return false;
        }
        listen_sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_sock_ == kInvalidSocket) {
            return false;
        }

        int reuse = 1;
        setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char *>(&reuse), sizeof(reuse));

        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // let the OS pick a free port

        if (bind(listen_sock_, reinterpret_cast<sockaddr *>(&addr),
                 sizeof(addr)) != 0) {
            close();
            return false;
        }
        if (listen(listen_sock_, 1) != 0) {
            close();
            return false;
        }

        socklen_t addr_len = sizeof(addr);
        if (getsockname(listen_sock_, reinterpret_cast<sockaddr *>(&addr),
                        &addr_len) != 0) {
            close();
            return false;
        }
        port_ = ntohs(addr.sin_port);
        return true;
    }

    std::string get_endpoint() const override {
        return std::to_string(port_);
    }

    bool wait_for_client(long timeout_ms) override {
        if (listen_sock_ == kInvalidSocket) {
            return false;
        }
        if (client_sock_ != kInvalidSocket) {
            return true;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_sock_, &readfds);

        timeval tv;
        timeval *tv_ptr = nullptr;
        if (timeout_ms >= 0) {
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            tv_ptr = &tv;
        }

        int nfds = static_cast<int>(listen_sock_) + 1;
        int ret = select(nfds, &readfds, nullptr, nullptr, tv_ptr);
        if (ret <= 0) {
            return false;
        }

        client_sock_ = accept(listen_sock_, nullptr, nullptr);
        if (client_sock_ == kInvalidSocket) {
            return false;
        }
        return enable_nodelay();
    }

    bool send(const char *data, size_t len) override {
        if (client_sock_ == kInvalidSocket) {
            return false;
        }
        size_t sent = 0;
        while (sent < len) {
#ifdef _WIN32
            int n = ::send(client_sock_, data + sent,
                           static_cast<int>(len - sent), 0);
#else
            ssize_t n = ::send(client_sock_, data + sent, len - sent, 0);
#endif
            if (n <= 0) {
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    bool recv_frame(std::string &out) override {
        while (true) {
            if (recv_buf_.size() >= 4) {
                uint32_t len = 0;
                for (int i = 0; i < 4; ++i) {
                    len |= static_cast<uint32_t>(
                               static_cast<unsigned char>(recv_buf_[i]))
                           << (8 * i);
                }
                if (len > kMaxPayload) {
                    // Refuse to buffer an absurd frame.
                    close();
                    return false;
                }
                if (recv_buf_.size() >= static_cast<size_t>(4 + len)) {
                    out.assign(recv_buf_, 4, len);
                    recv_buf_.erase(0, 4 + len);
                    return true;
                }
            }
            if (client_sock_ == kInvalidSocket) {
                return false;
            }
            char chunk[65536];
#ifdef _WIN32
            int n = ::recv(client_sock_, chunk, sizeof(chunk), 0);
#else
            ssize_t n = ::recv(client_sock_, chunk, sizeof(chunk), 0);
#endif
            if (n <= 0) {
                // The peer closed the connection.
                close();
                return false;
            }
            recv_buf_.append(chunk, static_cast<size_t>(n));
            if (recv_buf_.size() > 16 * 1024 * 1024) {
                // Refuse to buffer more than 16 MiB without a frame header.
                close();
                return false;
            }
        }
    }

    bool wait_readable(long timeout_ms) override {
        if (client_sock_ == kInvalidSocket) {
            return false;
        }
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(client_sock_, &readfds);

        // If a complete frame is already buffered, report readable without
        // touching the socket.
        if (recv_buf_.size() >= 4) {
            uint32_t len = 0;
            for (int i = 0; i < 4; ++i) {
                len |= static_cast<uint32_t>(
                           static_cast<unsigned char>(recv_buf_[i]))
                       << (8 * i);
            }
            if (recv_buf_.size() >= static_cast<size_t>(4 + len)) {
                return true;
            }
        }

        int nfds = static_cast<int>(client_sock_) + 1;

        // Spin-then-block: a peer round-trip (the child dispatching its next
        // request or acknowledgement) typically completes within tens of
        // microseconds, so poll for a bounded budget before blocking in
        // select(). This trades a little CPU for a much lower per-message
        // latency; once the budget is exhausted the wait below sleeps as
        // before, so long idle waits stay cheap.
        if (timeout_ms != 0) {
            const auto deadline =
                std::chrono::steady_clock::now() + kSpinBudget;
            while (true) {
                fd_set spinfds;
                FD_ZERO(&spinfds);
                FD_SET(client_sock_, &spinfds);
                timeval zero_tv;
                zero_tv.tv_sec = 0;
                zero_tv.tv_usec = 0;
                int r = select(nfds, &spinfds, nullptr, nullptr, &zero_tv);
                if (r != 0) {
                    return r > 0;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    break;
                }
            }
        }

        timeval tv;
        timeval *tv_ptr = nullptr;
        if (timeout_ms >= 0) {
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            tv_ptr = &tv;
        }
        int ret = select(nfds, &readfds, nullptr, nullptr, tv_ptr);
        return ret > 0;
    }

    bool is_open() const override { return client_sock_ != kInvalidSocket; }

    void close() override {
        if (client_sock_ != kInvalidSocket) {
            close_socket(client_sock_);
            client_sock_ = kInvalidSocket;
        }
        if (listen_sock_ != kInvalidSocket) {
            close_socket(listen_sock_);
            listen_sock_ = kInvalidSocket;
        }
        recv_buf_.clear();
    }

  private:
    static constexpr size_t kMaxPayload = 1u << 30;

    bool enable_nodelay() {
        int enabled = 1;
        return setsockopt(client_sock_, IPPROTO_TCP, TCP_NODELAY,
                          reinterpret_cast<const char *>(&enabled),
                          sizeof(enabled)) == 0;
    }

    socket_t listen_sock_ = kInvalidSocket;
    socket_t client_sock_ = kInvalidSocket;
    uint16_t port_ = 0;
    std::string recv_buf_;
};

}  // namespace

IpcTransport *IpcTransport::create() {
    // COCOTB_IPC_TRANSPORT selects the transport backend: "tcp" (default) or
    // "shm" (shared memory). Unknown values fall back to TCP.
    const char *env = std::getenv("COCOTB_IPC_TRANSPORT");
    if (env && std::strcmp(env, "shm") == 0) {
        // The shared-memory transport is not implemented yet: fall back to
        // TCP. The endpoint handed to the child is a plain port number, so
        // the child connects over TCP regardless of this variable.
        LOG_WARN(
            "COCOTB_IPC_TRANSPORT=shm is not supported yet, using TCP");
        return new TcpTransport();
    }
    return new TcpTransport();
}

}  // namespace ipc
}  // namespace cocotb
