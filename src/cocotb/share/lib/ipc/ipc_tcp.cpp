// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// TCP loopback transport for the cocotb IPC layer.
//
// The simulator process listens on an ephemeral TCP port on the loopback
// interface and the Python child process connects to it. Messages are
// length-prefixed frames `[u32 LE length][payload]`; TCP_NODELAY is enabled
// on both sockets so small control messages are not stalled by Nagle's
// algorithm.

#include "./ipc_base.hpp"
#include "./ipc_shm.hpp"

#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cocotb {
namespace ipc {

namespace {

class TcpTransport : public IpcTransport {
  public:
    TcpTransport() = default;
    ~TcpTransport() override { close(); }

    bool open() override {
#ifdef _WIN32
        WSADATA wsa_data;
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            return false;
        }
#endif
        listen_sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_sock_ == kInvalidSocket) {
            return false;
        }

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
        FD_SET(static_cast<unsigned>(listen_sock_), &readfds);

        timeval tv;
        timeval *tv_ptr = nullptr;
        if (timeout_ms >= 0) {
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            tv_ptr = &tv;
        }

        int ret = select(static_cast<int>(listen_sock_) + 1, &readfds, nullptr,
                         nullptr, tv_ptr);
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

    void close() override {
#ifdef _WIN32
        if (client_sock_ != kInvalidSocket) {
            closesocket(client_sock_);
        }
        if (listen_sock_ != kInvalidSocket) {
            closesocket(listen_sock_);
        }
        WSACleanup();
#else
        if (client_sock_ != kInvalidSocket) {
            ::close(client_sock_);
        }
        if (listen_sock_ != kInvalidSocket) {
            ::close(listen_sock_);
        }
#endif
        client_sock_ = kInvalidSocket;
        listen_sock_ = kInvalidSocket;
    }

  private:
    static constexpr size_t kMaxPayload = 1u << 30;

    bool enable_nodelay() {
        int enabled = 1;
        return setsockopt(client_sock_, IPPROTO_TCP, TCP_NODELAY,
                          reinterpret_cast<const char *>(&enabled),
                          sizeof(enabled)) == 0;
    }

#ifdef _WIN32
    static constexpr SOCKET kInvalidSocket = INVALID_SOCKET;
    SOCKET listen_sock_ = kInvalidSocket;
    SOCKET client_sock_ = kInvalidSocket;
#else
    static constexpr int kInvalidSocket = -1;
    int listen_sock_ = kInvalidSocket;
    int client_sock_ = kInvalidSocket;
#endif
    uint16_t port_ = 0;
    std::string recv_buf_;
};

}  // namespace

IpcTransport *IpcTransport::create() {
    // COCOTB_IPC_TRANSPORT selects the transport backend: "tcp" (default)
    // or "shm" (shared memory). Unknown values fall back to TCP.
    const char *env = std::getenv("COCOTB_IPC_TRANSPORT");
    if (env && std::strcmp(env, "shm") == 0) {
        IpcTransport *shm = create_shm_transport();
        if (shm) {
            return shm;
        }
    }
    return new TcpTransport();
}

}  // namespace ipc
}  // namespace cocotb