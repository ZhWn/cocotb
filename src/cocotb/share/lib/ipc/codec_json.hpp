// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// JSON codec for the cocotb IPC layer.
//
// Messages are encoded as JSON objects ({"type": "request", ...}), which
// makes protocol debugging easy: run with COCOTB_IPC_PROTOCOL=json and dump
// the frames as text. Bytes values are encoded as
// {"__bytes__": "<base64>"}; NaN and infinity are encoded as the (Python
// compatible) bare tokens NaN, Infinity and -Infinity.

#ifndef COCOTB_IPC_CODEC_JSON_HPP_
#define COCOTB_IPC_CODEC_JSON_HPP_

#include "./ipc_value.hpp"

#include <string>

namespace cocotb {
namespace ipc {
namespace codec_json {

// Encodes a message into a JSON document. Returns an empty string when
// `value` is not a message (an object with a string "type" member).
std::string encode(const IpcValue &value);

// Decodes one JSON document into a message; returns false on malformed
// input or when the document is not an object.
bool decode(const char *begin, const char *end, IpcValue &value);

}  // namespace codec_json
}  // namespace ipc
}  // namespace cocotb

#endif /* COCOTB_IPC_CODEC_JSON_HPP_ */
