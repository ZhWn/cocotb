// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Binary codec for the cocotb IPC layer.
//
// Wire format (little-endian, mirrored by cocotb/_ipc/_protocol.py):
//
//   frame  : u32 len, payload          (framing lives in the transport)
//   payload: msgtype u8 + fields
//   msgtype: 0=request 1=response 2=callback 3=callback_ack 4=log
//            5=hello 6=ready
//   value  : u8 tag + payload          (tags below)
//
//   request      : u64 id, string method, u32 nargs, value*n
//   response     : u64 id, u8 ok, value | string error
//   callback     : u64 id, string func, u64 cb_id, u32 time_high,
//                  u32 time_low
//   callback_ack : u64 id, i64 result
//   log          : i64 level, string logger, string filename, u64 lineno,
//                  string msg, string function
//   hello        : u32 version, u64 pid
//   ready        : u32 version, u8 ok
//
//   tags: 0x00 null, 0x01 bool(u8), 0x02 int(i64), 0x03 real(f64),
//         0x04 string(u32 n + utf8), 0x05 bytes(u32 n + raw),
//         0x06 array(u32 count + values),
//         0x07 object(u32 count + (u32 keylen + utf8 key + value)*)

#ifndef COCOTB_IPC_CODEC_BINARY_HPP_
#define COCOTB_IPC_CODEC_BINARY_HPP_

#include "./ipc_value.hpp"

#include <string>

namespace cocotb {
namespace ipc {
namespace codec_binary {

// Encodes a message into a payload. Returns an empty string when `value` is
// not a message (an object with a string "type" member). No framing; the
// caller adds the length prefix.
std::string encode(const IpcValue &value);

// Decodes one payload into a message; returns false on malformed input.
bool decode(const char *begin, const char *end, IpcValue &value);

}  // namespace codec_binary
}  // namespace ipc
}  // namespace cocotb

#endif /* COCOTB_IPC_CODEC_BINARY_HPP_ */
