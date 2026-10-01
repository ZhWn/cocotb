// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

#include "./ipc_protocol.hpp"

#include "./codec_binary.hpp"
#include "./codec_json.hpp"
#include "./ipc_priv.hpp"

#include <cstdint>
#include <cstdlib>  // getenv
#include <cstring>  // strcmp
#include <string>

namespace cocotb {
namespace ipc {

IpcProtocol g_ipc_protocol = IpcProtocol::Binary;

namespace {

bool frame_send(IpcTransport &transport, const char *data, size_t len) {
    if (len > kMaxFrameSize) {
        return false;
    }
    char header[4];
    header[0] = static_cast<char>(len & 0xFF);
    header[1] = static_cast<char>((len >> 8) & 0xFF);
    header[2] = static_cast<char>((len >> 16) & 0xFF);
    header[3] = static_cast<char>((len >> 24) & 0xFF);
    return transport.send(header, sizeof(header)) &&
           transport.send(data, len);
}

}  // namespace

bool send_value(IpcTransport &transport, IpcProtocol protocol,
                const IpcValue &value) {
    std::string payload;
    switch (protocol) {
        case IpcProtocol::Binary:
            payload = codec_binary::encode(value);
            break;
        case IpcProtocol::Json:
            payload = codec_json::encode(value);
            break;
    }
    if (payload.empty()) {
        return false;
    }
    return frame_send(transport, payload.data(), payload.size());
}

bool recv_value(IpcTransport &transport, IpcProtocol protocol, IpcValue &value) {
    std::string payload;
    if (!transport.recv_frame(payload)) {
        return false;
    }
    switch (protocol) {
        case IpcProtocol::Binary:
            return codec_binary::decode(payload.data(),
                                        payload.data() + payload.size(),
                                        value);
        case IpcProtocol::Json:
            return codec_json::decode(payload.data(),
                                      payload.data() + payload.size(), value);
    }
    return false;
}

void resolve_protocol() {
    const char *value = getenv("COCOTB_IPC_PROTOCOL");
    if (value == nullptr || value[0] == '\0' || strcmp(value, "binary") == 0) {
        g_ipc_protocol = IpcProtocol::Binary;
    } else if (strcmp(value, "json") == 0) {
        g_ipc_protocol = IpcProtocol::Json;
    } else {
        IPC_LOG_ERROR(
            "Unknown COCOTB_IPC_PROTOCOL %s, falling back to \"binary\"", value);
        g_ipc_protocol = IpcProtocol::Binary;
    }
}

}  // namespace ipc
}  // namespace cocotb
