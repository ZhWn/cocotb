// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Abstract transport for the cocotb IPC layer.
//
// The transport carries framed messages of the form `[u32 LE length][payload]`
// between the simulator process (server) and the Python child process
// (client). The payload is encoded by a codec (JSON or binary); the length
// framing lives in the transport. Additional transport backends can be added
// by implementing this interface and selecting them in the factory.

#ifndef COCOTB_IPC_BASE_HPP_
#define COCOTB_IPC_BASE_HPP_

#include <cstddef>  // size_t
#include <string>

namespace cocotb {
namespace ipc {

class IpcTransport {
  public:
    virtual ~IpcTransport() = default;

    // Bind a listening endpoint. Returns true on success.
    virtual bool open() = 0;

    // The endpoint identifier the client should connect to. For TCP this is
    // the decimal port number; for shared memory it is a unique resource
    // token. The value is handed to the Python child on the command line.
    virtual std::string get_endpoint() const = 0;

    // Wait for the client to connect. Returns true once connected.
    // A negative timeout waits indefinitely.
    virtual bool wait_for_client(long timeout_ms) = 0;

    // Send a raw chunk of bytes. Returns true on success.
    virtual bool send(const char *data, size_t len) = 0;

    // Receive one complete framed message `[u32 LE length][payload]` and
    // return the payload (without the 4-byte length prefix). Blocks until a
    // full frame is available. Returns true on success, false when the
    // connection is closed or an error occurs.
    virtual bool recv_frame(std::string &out) = 0;

    virtual void close() = 0;

    // Factory: creates the transport selected by the COCOTB_IPC_TRANSPORT
    // environment variable ("tcp" by default, "shm" for shared memory).
    static IpcTransport *create();
};

}  // namespace ipc
}  // namespace cocotb

#endif /* COCOTB_IPC_BASE_HPP_ */