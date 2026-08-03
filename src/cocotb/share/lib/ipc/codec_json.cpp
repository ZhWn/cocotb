// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

#include "./codec_json.hpp"

#include <cerrno>  // errno, ERANGE
#include <cstdio>  // snprintf
#include <cstdlib>  // strtod, strtoll
#include <cstring>
#include <string>
#include <utility>  // move
#include <vector>

namespace cocotb {
namespace ipc {
namespace codec_json {

namespace {

/*******************************************************************************
 * base64 (the wire encoding of IpcValue::bytes inside JSON documents)
 *******************************************************************************/

const char b64chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const char *data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned a = static_cast<unsigned char>(data[i]);
        unsigned b = i + 1 < len ? static_cast<unsigned char>(data[i + 1]) : 0;
        unsigned c = i + 2 < len ? static_cast<unsigned char>(data[i + 2]) : 0;
        out.push_back(b64chars[a >> 2]);
        out.push_back(b64chars[((a & 3) << 4) | (b >> 4)]);
        out.push_back(i + 1 < len ? b64chars[((b & 15) << 2) | (c >> 6)] : '=');
        out.push_back(i + 2 < len ? b64chars[c & 63] : '=');
    }
    return out;
}

int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

std::string base64_decode(const std::string &in) {
    std::string out;
    out.reserve((in.size() / 4) * 3);
    unsigned buf = 0;
    int bits = 0;
    for (char ch : in) {
        if (ch == '=' || ch == '\n' || ch == '\r') {
            continue;
        }
        int v = b64_value(ch);
        if (v < 0) {
            return out;
        }
        buf = (buf << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

/*******************************************************************************
 * JSON encoder
 *******************************************************************************/

inline void append_escaped(std::string &out, const char *begin,
                           const char *end) {
    out.push_back('"');
    for (const char *c = begin; c != end; ++c) {
        unsigned char ch = static_cast<unsigned char>(*c);
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (ch < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", ch);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    out.push_back('"');
}

inline void append_number(std::string &out, double value) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%.17g", value);
    out += buf;
}

void encode_value(const IpcValue &value, std::string &out) {
    switch (value.type()) {
        case IpcType::Null:
            out += "null";
            break;
        case IpcType::Bool:
            out += value.get_bool() ? "true" : "false";
            break;
        case IpcType::Int:
            out += std::to_string(value.get_int());
            break;
        case IpcType::Float:
            append_number(out, value.get_float());
            break;
        case IpcType::String: {
            const std::string &str = value.get_string();
            append_escaped(out, str.data(), str.data() + str.size());
            break;
        }
        case IpcType::Bytes: {
            // Wire marker, byte-compatible with the legacy protocol.
            const std::string &raw = value.get_bytes();
            out += "{\"__bytes__\":\"";
            out += base64_encode(raw.data(), raw.size());
            out += "\"}";
            break;
        }
        case IpcType::Array: {
            out.push_back('[');
            bool first = true;
            for (const auto &item : value.array_ref()) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                encode_value(item, out);
            }
            out.push_back(']');
            break;
        }
        case IpcType::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto &entry : value.object_ref()) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                append_escaped(out, entry.first.data(),
                               entry.first.data() + entry.first.size());
                out.push_back(':');
                encode_value(entry.second, out);
            }
            out.push_back('}');
            break;
        }
    }
}

/*******************************************************************************
 * JSON parser (ported from the legacy json.hpp, semantics unchanged)
 *******************************************************************************/

inline bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool parse_value(const char *&c, const char *end, IpcValue &out) {
    if (c == end) {
        return false;
    }
    switch (*c) {
        case 'n':
            if (end - c >= 4 && c[1] == 'u' && c[2] == 'l' && c[3] == 'l') {
                c += 4;
                out = IpcValue::null();
                return true;
            }
            return false;
        case 't':
            if (end - c >= 4 && c[1] == 'r' && c[2] == 'u' && c[3] == 'e') {
                c += 4;
                out = IpcValue::boolean(true);
                return true;
            }
            return false;
        case 'f':
            if (end - c >= 5 && c[1] == 'a' && c[2] == 'l' && c[3] == 's' &&
                c[4] == 'e') {
                c += 5;
                out = IpcValue::boolean(false);
                return true;
            }
            return false;
        case '"': {
            std::string str;
            ++c;  // skip opening quote
            while (c != end) {
                unsigned char ch = static_cast<unsigned char>(*c);
                if (ch == '"') {
                    ++c;
                    out = IpcValue::string(std::move(str));
                    return true;
                }
                if (ch == '\\') {
                    ++c;
                    if (c == end) {
                        return false;
                    }
                    char esc = *c;
                    switch (esc) {
                        case '"':
                        case '\\':
                        case '/':
                            str.push_back(esc);
                            break;
                        case 'b':
                            str.push_back('\b');
                            break;
                        case 'f':
                            str.push_back('\f');
                            break;
                        case 'n':
                            str.push_back('\n');
                            break;
                        case 'r':
                            str.push_back('\r');
                            break;
                        case 't':
                            str.push_back('\t');
                            break;
                        case 'u': {
                            if (end - c < 5) {
                                return false;
                            }
                            unsigned code = 0;
                            for (int i = 1; i <= 4; ++i) {
                                char h = c[i];
                                unsigned digit;
                                if (h >= '0' && h <= '9') {
                                    digit = static_cast<unsigned>(h - '0');
                                } else if (h >= 'a' && h <= 'f') {
                                    digit = static_cast<unsigned>(h - 'a' + 10);
                                } else if (h >= 'A' && h <= 'F') {
                                    digit = static_cast<unsigned>(h - 'A' + 10);
                                } else {
                                    return false;
                                }
                                code = code * 16 + digit;
                            }
                            c += 4;
                            // We only support the BMP; surrogate pairs are not
                            // needed for the IPC messages we exchange.
                            if (code < 0x80) {
                                str.push_back(static_cast<char>(code));
                            } else if (code < 0x800) {
                                str.push_back(static_cast<char>(0xC0 | (code >> 6)));
                                str.push_back(
                                    static_cast<char>(0x80 | (code & 0x3F)));
                            } else {
                                str.push_back(
                                    static_cast<char>(0xE0 | (code >> 12)));
                                str.push_back(static_cast<char>(
                                    0x80 | ((code >> 6) & 0x3F)));
                                str.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                            }
                            break;
                        }
                        default:
                            return false;
                    }
                    ++c;
                } else if (ch < 0x20) {
                    return false;
                } else {
                    str.push_back(static_cast<char>(ch));
                    ++c;
                }
            }
            return false;
        }
        case '[': {
            IpcValue arr = IpcValue::array();
            ++c;  // skip '['
            while (true) {
                while (c != end && is_ws(*c)) {
                    ++c;
                }
                if (c == end) {
                    return false;
                }
                if (*c == ']') {
                    ++c;
                    out = std::move(arr);
                    return true;
                }
                IpcValue item;
                if (!parse_value(c, end, item)) {
                    return false;
                }
                arr.push_back(std::move(item));
                while (c != end && is_ws(*c)) {
                    ++c;
                }
                if (c == end) {
                    return false;
                }
                if (*c == ',') {
                    ++c;
                } else if (*c == ']') {
                    ++c;
                    out = std::move(arr);
                    return true;
                } else {
                    return false;
                }
            }
        }
        case '{': {
            IpcValue obj = IpcValue::object();
            ++c;  // skip '{'
            while (true) {
                while (c != end && is_ws(*c)) {
                    ++c;
                }
                if (c == end) {
                    return false;
                }
                if (*c == '}') {
                    ++c;
                    out = std::move(obj);
                    return true;
                }
                if (*c != '"') {
                    return false;
                }
                IpcValue key_value;
                if (!parse_value(c, end, key_value) || !key_value.is_string()) {
                    return false;
                }
                const std::string key = key_value.get_string();
                while (c != end && is_ws(*c)) {
                    ++c;
                }
                if (c == end || *c != ':') {
                    return false;
                }
                ++c;
                IpcValue member;
                if (!parse_value(c, end, member)) {
                    return false;
                }
                obj.set(key, std::move(member));
                while (c != end && is_ws(*c)) {
                    ++c;
                }
                if (c == end) {
                    return false;
                }
                if (*c == ',') {
                    ++c;
                } else if (*c == '}') {
                    ++c;
                    out = std::move(obj);
                    return true;
                } else {
                    return false;
                }
            }
        }
        default: {
            // Number: [minus] int [frac] [exp]
            if (*c == '-' || (*c >= '0' && *c <= '9')) {
                bool is_float = false;
                std::string num;
                num.reserve(static_cast<size_t>(end - c));
                while (c != end) {
                    char ch = *c;
                    if ((ch >= '0' && ch <= '9') || ch == '-' || ch == '+' ||
                        ch == '.' || ch == 'e' || ch == 'E') {
                        if (ch == '.' || ch == 'e' || ch == 'E') {
                            is_float = true;
                        }
                        num.push_back(ch);
                        ++c;
                    } else {
                        break;
                    }
                }
                if (num.empty() || (num.size() == 1 && num[0] == '-')) {
                    return false;
                }
                if (is_float) {
                    out = IpcValue::floating(std::strtod(num.c_str(), nullptr));
                } else {
                    errno = 0;
                    long long ll = std::strtoll(num.c_str(), nullptr, 10);
                    if (errno == ERANGE || ll > INT64_MAX || ll < INT64_MIN) {
                        // Too large for int64; fall back to double.
                        out = IpcValue::floating(std::strtod(num.c_str(), nullptr));
                    } else {
                        out = IpcValue::integer(ll);
                    }
                }
                return true;
            }
            return false;
        }
    }
}

// Recursively replaces `{"__bytes__": "<base64>"}` object markers with
// IpcValue::bytes values, matching the historical wire marker semantics.
IpcValue demark(const IpcValue &value) {
    if (value.is_object() && value.object_ref().size() == 1) {
        const IpcValue *marker = value.get("__bytes__");
        if (marker && marker->is_string()) {
            return IpcValue::bytes(base64_decode(marker->get_string()));
        }
    }
    if (value.is_object()) {
        IpcValue obj = IpcValue::object();
        for (const auto &entry : value.object_ref()) {
            obj.set(entry.first, demark(entry.second));
        }
        return obj;
    }
    if (value.is_array()) {
        IpcValue arr = IpcValue::array();
        for (const auto &item : value.array_ref()) {
            arr.push_back(demark(item));
        }
        return arr;
    }
    return value;
}

}  // namespace

std::string encode(const IpcValue &value) {
    std::string out;
    encode_value(value, out);
    return out;
}

bool decode(const char *begin, const char *end, IpcValue &out) {
    const char *c = begin;

    // Skip leading whitespace.
    while (c != end && is_ws(*c)) {
        ++c;
    }

    if (c == end) {
        return false;
    }

    IpcValue parsed;
    if (!parse_value(c, end, parsed)) {
        return false;
    }

    // Skip trailing whitespace; nothing else may follow the value.
    while (c != end && is_ws(*c)) {
        ++c;
    }
    if (c != end) {
        return false;
    }

    out = demark(parsed);
    return true;
}

}  // namespace codec_json
}  // namespace ipc
}  // namespace cocotb