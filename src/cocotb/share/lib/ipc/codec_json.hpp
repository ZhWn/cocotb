// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// JSON codec for the cocotb IPC layer.
//
// The wire format is byte-compatible with the historical newline-delimited
// JSON protocol: `IpcValue::bytes` values are encoded as the
// `{"__bytes__": "<base64>"}` object marker, and such markers are decoded
// back into `IpcValue::bytes` on the receiving side. Codecs never pass
// objects around, so the dispatcher and the rest of the stack see native
// bytes regardless of which codec a message was carried by.

#ifndef COCOTB_IPC_CODEC_JSON_HPP_
#define COCOTB_IPC_CODEC_JSON_HPP_

#include "./ipc_value.hpp"

#include <string>

namespace cocotb {
namespace ipc {
namespace codec_json {

// Encodes a value tree into one JSON document (no framing; the caller adds
// the length prefix / newline).
std::string encode(const IpcValue &value);

// Parses one JSON document from [begin, end) and stores the decoded value
// tree in `out`. Marker objects (`{"__bytes__": ...}`) are translated to
// `IpcValue::bytes`. Returns true on success.
bool decode(const char *begin, const char *end, IpcValue &out);

}  // namespace codec_json
}  // namespace ipc
}  // namespace cocotb

#endif /* COCOTB_IPC_CODEC_JSON_HPP_ */