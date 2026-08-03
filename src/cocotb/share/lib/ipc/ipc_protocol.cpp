// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

#include "./ipc_protocol.hpp"

#include "./codec_binary.hpp"
#include "./codec_json.hpp"

#include <cstdint>
#include <cstdlib>  // getenv
#include <cstring>
#include <string>

namespace cocotb {
namespace ipc {

IpcProtocol g_ipc_protocol = IpcProtocol::Json;

namespace {

}  // namespace

bool frame_send(IpcTransport &transport, const char *data, size_t len,
                size_t max_frame) {
    if (len > max_frame || len > kMaxFrameSize) {
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

bool send_value(IpcTransport &transport, IpcProtocol protocol,
                const IpcValue &value, size_t max_frame) {
    if (max_frame == 0) {
        max_frame = kMaxFrameSize;
    }
    switch (protocol) {
        case IpcProtocol::Json: {
            const std::string payload = codec_json::encode(value);
            return frame_send(transport, payload.data(), payload.size(),
                              max_frame);
        }
        case IpcProtocol::Binary: {
            const std::string payload = codec_binary::encode(value);
            return frame_send(transport, payload.data(), payload.size(),
                              max_frame);
        }
    }
    return false;
}

bool recv_value(IpcTransport &transport, IpcProtocol protocol,
                IpcValue &value, size_t max_frame) {
    if (max_frame == 0) {
        max_frame = kMaxFrameSize;
    }
    std::string payload;
    if (!transport.recv_frame(payload)) {
        return false;
    }
    if (payload.size() > max_frame) {
        return false;
    }
    switch (protocol) {
        case IpcProtocol::Json:
            return codec_json::decode(payload.data(),
                                      payload.data() + payload.size(), value);
        case IpcProtocol::Binary:
            return codec_binary::decode(payload.data(),
                                        payload.data() + payload.size(),
                                        value);
    }
    return false;
}

bool resolve_protocol(const char *env) {
    const char *value = getenv(env);
    if (value && value[0] != '\0' &&
        std::string(value) == "binary") {
        g_ipc_protocol = IpcProtocol::Binary;
    } else {
        g_ipc_protocol = IpcProtocol::Json;
    }
    return true;
}

}  // namespace ipc
}  // namespace cocotb