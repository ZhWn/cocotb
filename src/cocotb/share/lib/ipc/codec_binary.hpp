// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Binary codec for the cocotb IPC layer.
//
// Wire format (little-endian, mirrored by cocotb/ipc/_protocol.py):
//
//   frame  : u32 len, payload
//   payload: one message (msgtype u8 + fields) or a naked value
//   msgtype: 0=request 1=response 2=callback 3=callback_ack 4=log
//   value  : u8 tag + payload        (tags below)
//   string : tag 0x04 + u32 n + utf8 bytes  (used for all text fields)
//
//   tags: 0x00 null, 0x01 bool(u8), 0x02 int(i64), 0x03 real(f64),
//         0x04 string, 0x05 bytes(u32 n + raw), 0x06 array(u32 count +
//         values), 0x07 object(u32 count + (u32 keylen + utf8 key + value)*)

#ifndef COCOTB_IPC_CODEC_BINARY_HPP_
#define COCOTB_IPC_CODEC_BINARY_HPP_

#include "./ipc_value.hpp"

#include <string>

namespace cocotb {
namespace ipc {
namespace codec_binary {

// Encodes a message (or generic value) into a payload. No framing; the caller
// adds the length prefix.
std::string encode(const IpcValue &value);

// Decodes one payload into `value`; returns false on malformed input.
bool decode(const char *begin, const char *end, IpcValue &value);

}  // namespace codec_binary
}  // namespace ipc
}  // namespace cocotb

#endif /* COCOTB_IPC_CODEC_BINARY_HPP_ */