// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// TCP loopback transport for the cocotb IPC layer.
//
// The simulator process listens on an ephemeral TCP port on the loopback
// interface and the Python child process connects to it.

#include "./ipc_base.hpp"

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

    uint16_t get_port() const override { return port_; }

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
        return client_sock_ != kInvalidSocket;
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

    bool recv_line(std::string &out) override {
        while (true) {
            auto newline = recv_buf_.find('\n');
            if (newline != std::string::npos) {
                out.assign(recv_buf_, 0, newline);
                recv_buf_.erase(0, newline + 1);
                return true;
            }
            if (client_sock_ == kInvalidSocket) {
                return false;
            }
            char chunk[4096];
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
                // Refuse to buffer more than 16 MiB without a newline.
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

IpcTransport *IpcTransport::create() { return new TcpTransport(); }

bool send_json(IpcTransport &transport, const std::string &message) {
    return transport.send(message.data(), message.size()) &&
           transport.send("\n", 1);
}

bool recv_json(IpcTransport &transport, std::string &message) {
    return transport.recv_line(message);
}

}  // namespace ipc
}  // namespace cocotb
