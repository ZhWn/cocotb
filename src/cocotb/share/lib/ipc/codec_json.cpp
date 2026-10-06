// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

#include "./codec_json.hpp"

#include <cmath>     // isnan, isinf
#include <cstdint>
#include <cstdlib>   // strtod
#include <cstdio>    // snprintf
#include <cstring>   // strcmp, memcmp
#include <string>
#include <utility>   // move

namespace cocotb {
namespace ipc {
namespace codec_json {

namespace {

/*******************************************************************************
 * Base64 (for bytes values)
 *******************************************************************************/

const char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::string &in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= in.size()) {
        uint32_t v =
            (static_cast<uint32_t>(static_cast<uint8_t>(in[i])) << 16) |
            (static_cast<uint32_t>(static_cast<uint8_t>(in[i + 1])) << 8) |
            static_cast<uint32_t>(static_cast<uint8_t>(in[i + 2]));
        out.push_back(kBase64Alphabet[(v >> 18) & 63]);
        out.push_back(kBase64Alphabet[(v >> 12) & 63]);
        out.push_back(kBase64Alphabet[(v >> 6) & 63]);
        out.push_back(kBase64Alphabet[v & 63]);
        i += 3;
    }
    size_t rem = in.size() - i;
    if (rem == 1) {
        uint32_t v = static_cast<uint32_t>(static_cast<uint8_t>(in[i])) << 16;
        out.push_back(kBase64Alphabet[(v >> 18) & 63]);
        out.push_back(kBase64Alphabet[(v >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        uint32_t v =
            (static_cast<uint32_t>(static_cast<uint8_t>(in[i])) << 16) |
            (static_cast<uint32_t>(static_cast<uint8_t>(in[i + 1])) << 8);
        out.push_back(kBase64Alphabet[(v >> 18) & 63]);
        out.push_back(kBase64Alphabet[(v >> 12) & 63]);
        out.push_back(kBase64Alphabet[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

int base64_value(char c) {
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

bool base64_decode(const std::string &in, std::string &out) {
    if (in.size() % 4 != 0) {
        return false;
    }
    out.clear();
    out.reserve((in.size() / 4) * 3);
    for (size_t i = 0; i < in.size(); i += 4) {
        int pad = 0;
        uint32_t v = 0;
        for (size_t j = 0; j < 4; ++j) {
            char c = in[i + j];
            if (c == '=') {
                // Padding only allowed in the last quantum.
                if (i + 4 != in.size() || j < 2) {
                    return false;
                }
                pad++;
                v <<= 6;
                continue;
            }
            if (pad > 0) {
                return false;  // data after padding
            }
            int val = base64_value(c);
            if (val < 0) {
                return false;
            }
            v = (v << 6) | static_cast<uint32_t>(val);
        }
        out.push_back(static_cast<char>((v >> 16) & 0xFF));
        if (pad < 2) {
            out.push_back(static_cast<char>((v >> 8) & 0xFF));
        }
        if (pad < 1) {
            out.push_back(static_cast<char>(v & 0xFF));
        }
    }
    return true;
}

/*******************************************************************************
 * Writer
 *******************************************************************************/

void put_escaped(std::string &out, const std::string &s) {
    out.push_back('"');
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
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
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    // Pass through bytes >= 0x20 verbatim; the payload is
                    // UTF-8 (or raw bytes inside a string of a debug codec),
                    // which is valid JSON as long as control chars are
                    // escaped.
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void put_double(std::string &out, double v) {
    if (std::isnan(v)) {
        out += "NaN";
        return;
    }
    if (std::isinf(v)) {
        out += v < 0 ? "-Infinity" : "Infinity";
        return;
    }
    // Shortest representation that round-trips exactly.
    char buf[40];
    for (int precision = 15; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
        if (std::strtod(buf, nullptr) == v) {
            break;
        }
    }
    out += buf;
}

void put_value(const IpcValue &value, std::string &out) {
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
            put_double(out, value.get_float());
            break;
        case IpcType::String:
            put_escaped(out, value.get_string());
            break;
        case IpcType::Bytes:
            out += "{\"__bytes__\":";
            put_escaped(out, base64_encode(value.get_bytes()));
            out += "}";
            break;
        case IpcType::Array: {
            out.push_back('[');
            bool first = true;
            for (const auto &item : value.array_ref()) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                put_value(item, out);
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
                put_escaped(out, entry.first);
                out.push_back(':');
                put_value(entry.second, out);
            }
            out.push_back('}');
            break;
        }
    }
}

/*******************************************************************************
 * Parser
 *******************************************************************************/

void append_utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class Parser {
  public:
    Parser(const char *begin, const char *end) : p_(begin), end_(end) {}

    bool parse_document(IpcValue &out) {
        skip_ws();
        if (!parse_value(out)) {
            return false;
        }
        skip_ws();
        return p_ == end_;
    }

  private:
    void skip_ws() {
        while (p_ < end_ &&
               (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) {
            ++p_;
        }
    }

    bool match(const char *literal) {
        size_t n = std::strlen(literal);
        if (static_cast<size_t>(end_ - p_) < n ||
            std::memcmp(p_, literal, n) != 0) {
            return false;
        }
        p_ += n;
        return true;
    }

    bool parse_value(IpcValue &out) {
        if (p_ >= end_) {
            return false;
        }
        switch (*p_) {
            case 'n':
                if (!match("null")) {
                    return false;
                }
                out = IpcValue::null();
                return true;
            case 't':
                if (!match("true")) {
                    return false;
                }
                out = IpcValue::boolean(true);
                return true;
            case 'f':
                if (!match("false")) {
                    return false;
                }
                out = IpcValue::boolean(false);
                return true;
            case '"':
                return parse_string(out);
            case '[':
                return parse_array(out);
            case '{':
                return parse_object(out);
            case 'N':
                if (!match("NaN")) {
                    return false;
                }
                out = IpcValue::floating(
                    std::strtod("nan", nullptr));
                return true;
            case 'I':
                if (!match("Infinity")) {
                    return false;
                }
                out = IpcValue::floating(
                    std::strtod("inf", nullptr));
                return true;
            case '-': {
                // Could be -Infinity or a number.
                if (static_cast<size_t>(end_ - p_) > 1 && p_[1] == 'I') {
                    if (!match("-Infinity")) {
                        return false;
                    }
                    out = IpcValue::floating(
                        -std::strtod("inf", nullptr));
                    return true;
                }
                return parse_number(out);
            }
            default:
                return parse_number(out);
        }
    }

    bool parse_number(IpcValue &out) {
        const char *start = p_;
        if (p_ < end_ && *p_ == '-') {
            ++p_;
        }
        bool has_digit = false;
        while (p_ < end_ && *p_ >= '0' && *p_ <= '9') {
            ++p_;
            has_digit = true;
        }
        if (p_ < end_ && *p_ == '.') {
            ++p_;
            while (p_ < end_ && *p_ >= '0' && *p_ <= '9') {
                ++p_;
                has_digit = true;
            }
        }
        if (!has_digit) {
            return false;
        }
        if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
            ++p_;
            if (p_ < end_ && (*p_ == '+' || *p_ == '-')) {
                ++p_;
            }
            bool exp_digit = false;
            while (p_ < end_ && *p_ >= '0' && *p_ <= '9') {
                ++p_;
                exp_digit = true;
            }
            if (!exp_digit) {
                return false;
            }
        }
        std::string tmp(start, p_);
        double d = std::strtod(tmp.c_str(), nullptr);
        // Tokens without a fraction/exponent are integers: keep them as
        // ints so schema checks (`id` must be an int, etc.) behave the same
        // as with the binary codec. Bound the length to avoid int64
        // overflow; such values are vanishingly rare and parse as floats.
        if (tmp.find_first_of(".eE") == std::string::npos &&
            tmp.size() <= 19) {
            long long ll = std::strtoll(tmp.c_str(), nullptr, 10);
            out = IpcValue::integer(static_cast<int64_t>(ll));
            return true;
        }
        out = IpcValue::floating(d);
        return true;
    }

    bool parse_string(IpcValue &out) {
        if (p_ >= end_ || *p_ != '"') {
            return false;
        }
        ++p_;
        std::string result;
        while (true) {
            if (p_ >= end_) {
                return false;
            }
            char c = *p_++;
            if (c == '"') {
                break;
            }
            if (c != '\\') {
                result.push_back(c);
                continue;
            }
            if (p_ >= end_) {
                return false;
            }
            char esc = *p_++;
            switch (esc) {
                case '"':
                    result.push_back('"');
                    break;
                case '\\':
                    result.push_back('\\');
                    break;
                case '/':
                    result.push_back('/');
                    break;
                case 'b':
                    result.push_back('\b');
                    break;
                case 'f':
                    result.push_back('\f');
                    break;
                case 'n':
                    result.push_back('\n');
                    break;
                case 'r':
                    result.push_back('\r');
                    break;
                case 't':
                    result.push_back('\t');
                    break;
                case 'u': {
                    uint32_t cp;
                    if (!parse_hex4(cp)) {
                        return false;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // Surrogate pair: expect \uXXXX low surrogate.
                        if (static_cast<size_t>(end_ - p_) < 6 ||
                            p_[0] != '\\' || p_[1] != 'u') {
                            return false;
                        }
                        p_ += 2;
                        uint32_t lo;
                        if (!parse_hex4(lo)) {
                            return false;
                        }
                        if (lo < 0xDC00 || lo > 0xDFFF) {
                            return false;
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return false;  // lone low surrogate
                    }
                    append_utf8(result, cp);
                    break;
                }
                default:
                    return false;
            }
        }
        // Recognize the bytes marker: exactly {"__bytes__": "<base64>"}.
        // (Only relevant when called from parse_object below, where the
        // decision is made; here we just produce the string.)
        out = IpcValue::string(std::move(result));
        return true;
    }

    bool parse_hex4(uint32_t &out) {
        if (static_cast<size_t>(end_ - p_) < 4) {
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p_++;
            out <<= 4;
            if (c >= '0' && c <= '9') {
                out |= static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                out |= static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                out |= static_cast<uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    bool parse_array(IpcValue &out) {
        ++p_;  // '['
        IpcValue arr = IpcValue::array();
        skip_ws();
        if (p_ < end_ && *p_ == ']') {
            ++p_;
            out = std::move(arr);
            return true;
        }
        while (true) {
            skip_ws();
            IpcValue item;
            if (!parse_value(item)) {
                return false;
            }
            arr.push_back(std::move(item));
            skip_ws();
            if (p_ < end_ && *p_ == ',') {
                ++p_;
                continue;
            }
            if (p_ < end_ && *p_ == ']') {
                ++p_;
                out = std::move(arr);
                return true;
            }
            return false;
        }
    }

    bool parse_object(IpcValue &out) {
        ++p_;  // '{'
        IpcValue obj = IpcValue::object();
        skip_ws();
        if (p_ < end_ && *p_ == '}') {
            ++p_;
            out = std::move(obj);
            return true;
        }
        while (true) {
            skip_ws();
            IpcValue key;
            if (!parse_string(key)) {
                return false;
            }
            skip_ws();
            if (p_ >= end_ || *p_ != ':') {
                return false;
            }
            ++p_;
            skip_ws();
            IpcValue value;
            if (!parse_value(value)) {
                return false;
            }
            const std::string &key_str = key.get_string();
            if (key_str == "__bytes__" && value.is_string()) {
                std::string decoded;
                if (base64_decode(value.get_string(), decoded)) {
                    value = IpcValue::bytes(std::move(decoded));
                }
                // Not valid base64: keep as a plain string member.
            }
            obj.set(key_str, std::move(value));
            skip_ws();
            if (p_ < end_ && *p_ == ',') {
                ++p_;
                continue;
            }
            if (p_ < end_ && *p_ == '}') {
                ++p_;
                out = std::move(obj);
                return true;
            }
            return false;
        }
    }

    const char *p_;
    const char *end_;
};

}  // namespace

std::string encode(const IpcValue &value) {
    const IpcValue *type = value.get("type");
    if (!value.is_object() || type == nullptr || !type->is_string()) {
        return std::string();
    }
    std::string out;
    put_value(value, out);
    return out;
}

bool decode(const char *begin, const char *end, IpcValue &value) {
    Parser parser(begin, end);
    IpcValue result;
    if (!parser.parse_document(result) || !result.is_object()) {
        return false;
    }
    const IpcValue *type = result.get("type");
    if (type == nullptr || !type->is_string()) {
        return false;
    }
    value = std::move(result);
    return true;
}

}  // namespace codec_json
}  // namespace ipc
}  // namespace cocotb
