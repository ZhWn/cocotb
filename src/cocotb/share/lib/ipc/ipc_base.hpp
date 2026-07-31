// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Abstract transport for the cocotb IPC layer.
//
// The transport carries newline-delimited JSON messages between the
// simulator process (server) and the Python child process (client).
// Additional transport backends can be added by implementing this interface
// and selecting them in the factory.

#ifndef COCOTB_IPC_BASE_HPP_
#define COCOTB_IPC_BASE_HPP_

#include <cstdint>  // uint16_t
#include <string>

namespace cocotb {
namespace ipc {

class IpcTransport {
  public:
    virtual ~IpcTransport() = default;

    // Bind a listening endpoint. Returns true on success.
    virtual bool open() = 0;

    // The port (or equivalent identifier) the client should connect to.
    virtual uint16_t get_port() const = 0;

    // Wait for the client to connect. Returns true once connected.
    // A negative timeout waits indefinitely.
    virtual bool wait_for_client(long timeout_ms) = 0;

    // Send a raw chunk of bytes. Returns true on success.
    virtual bool send(const char *data, size_t len) = 0;

    // Receive one newline-terminated line (without the trailing newline).
    // Returns true on success, false when the connection is closed or an
    // error occurs.
    virtual bool recv_line(std::string &out) = 0;

    virtual void close() = 0;

    // Factory: creates the default transport for this platform.
    static IpcTransport *create();
};

// Sends a complete JSON message (appends the trailing newline).
bool send_json(IpcTransport &transport, const std::string &message);

// Receives one JSON message (a single newline-delimited line).
bool recv_json(IpcTransport &transport, std::string &message);

}  // namespace ipc
}  // namespace cocotb

#endif /* COCOTB_IPC_BASE_HPP_ */
