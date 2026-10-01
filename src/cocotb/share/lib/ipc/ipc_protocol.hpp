// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Protocol selection and framed message I/O for the cocotb IPC layer.
//
// The transport layer carries frames of the form `[u32 LE length][payload]`;
// the codec turns an `IpcValue` message into that payload and back. This
// module picks the codec (binary by default, JSON when COCOTB_IPC_PROTOCOL
// says so) and performs the framed send/receive on top of any transport.
//
// Message schema (both codecs):
//   hello         {type, version, pid}          Python -> simulator (first frame)
//   ready         {type, version, ok}           simulator -> Python
//   request       {type, id, method, args}      Python -> simulator
//   response      {type, id, ok, result|error}  simulator -> Python
//   callback      {type, id, func, cb_id, time} simulator -> Python
//   callback_ack  {type, id, result}            Python -> simulator
//   log           {type, level, logger, ...}    simulator -> Python

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
enum class IpcProtocol { Binary, Json };

// Maximum frame payload accepted on any transport.
constexpr size_t kMaxFrameSize = 1u << 30;

extern IpcProtocol g_ipc_protocol;

// Encodes `value` with the selected codec and sends it as one frame.
// `value` must be a message (an object with a string "type" member).
bool send_value(IpcTransport &transport, IpcProtocol protocol,
                const IpcValue &value);

// Receives one frame and decodes it into `value`.
bool recv_value(IpcTransport &transport, IpcProtocol protocol, IpcValue &value);

// Resolves the codec from the environment (COCOTB_IPC_PROTOCOL). The default
// is "binary"; "json" selects the JSON codec for debugging. Unknown values
// are reported and fall back to the default.
void resolve_protocol();

}  // namespace ipc
}  // namespace cocotb

#endif /* COCOTB_IPC_PROTOCOL_HPP_ */
