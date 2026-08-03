// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Protocol selection and framed message I/O for the cocotb IPC layer.
//
// The transport layer carries frames of the form `[u32 LE length][payload]`;
// the codec turns an `IpcValue` message into that payload and back. This
// module picks the codec (JSON by default, binary when COCOTB_IPC_PROTOCOL
// says so) and performs the framed send/receive on top of any transport.

#ifndef COCOTB_IPC_PROTOCOL_HPP_
#define COCOTB_IPC_PROTOCOL_HPP_

#include "./ipc_base.hpp"
#include "./ipc_value.hpp"

#include <cstddef>  // size_t
#include <string>

namespace cocotb {
namespace ipc {

// The codec used for all messages in this simulation run. Both the server
// (simulator) and the spawned Python child read the same environment
// variable, so the two sides always agree.
enum class IpcProtocol { Json, Binary };

// Maximum frame payload accepted on any transport.
constexpr size_t kMaxFrameSize = 1u << 30;

extern IpcProtocol g_ipc_protocol;

// Writes one framed message ([u32 LE length][payload]) to the transport.
bool frame_send(IpcTransport &transport, const char *data, size_t len,
                size_t max_frame);

// Encodes `value` with the selected codec and sends it as one frame.
bool send_value(IpcTransport &transport, IpcProtocol protocol,
                const IpcValue &value, size_t max_frame);

// Receives one frame and decodes it into `value`.
bool recv_value(IpcTransport &transport, IpcProtocol protocol,
                IpcValue &value, size_t max_frame);

// Resolves the protocol from the environment; unknown values fall back to
// Json. Returns false when the requested protocol is not compiled in.
bool resolve_protocol(const char *env = "COCOTB_IPC_PROTOCOL");

}  // namespace ipc
}  // namespace cocotb

#endif /* COCOTB_IPC_PROTOCOL_HPP_ */