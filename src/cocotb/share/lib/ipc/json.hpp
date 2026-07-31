// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Minimal JSON value type, parser and serializer for the cocotb IPC layer.
//
// This is intentionally self-contained (no external dependencies) and covers
// just the subset of JSON needed by the IPC protocol: null, booleans,
// integers, doubles, strings, arrays and objects.

#ifndef COCOTB_IPC_JSON_HPP_
#define COCOTB_IPC_JSON_HPP_

#include <cerrno>    // errno, ERANGE
#include <cmath>     // strtod
#include <cstdint>   // int64_t, uint64_t
#include <cstdio>    // snprintf
#include <cstdlib>   // strtod, strtoll
#include <string>
#include <utility>  // move
#include <vector>

namespace cocotb {
namespace ipc {

enum class JsonType {
    Null,
    Bool,
    Int,
    Float,
    String,
    Array,
    Object,
};

class JsonValue {
  public:
    JsonValue() : type_(JsonType::Null), bool_(false), int_(0), float_(0.0) {}

    static JsonValue null() { return JsonValue(); }

    static JsonValue boolean(bool value) {
        JsonValue v;
        v.type_ = JsonType::Bool;
        v.bool_ = value;
        return v;
    }

    static JsonValue integer(int64_t value) {
        JsonValue v;
        v.type_ = JsonType::Int;
        v.int_ = value;
        return v;
    }

    static JsonValue floating(double value) {
        JsonValue v;
        v.type_ = JsonType::Float;
        v.float_ = value;
        return v;
    }

    static JsonValue string(std::string value) {
        JsonValue v;
        v.type_ = JsonType::String;
        v.str_ = std::move(value);
        return v;
    }

    static JsonValue array() {
        JsonValue v;
        v.type_ = JsonType::Array;
        return v;
    }

    static JsonValue object() {
        JsonValue v;
        v.type_ = JsonType::Object;
        return v;
    }

    JsonType type() const { return type_; }

    bool is_null() const { return type_ == JsonType::Null; }
    bool is_bool() const { return type_ == JsonType::Bool; }
    bool is_int() const { return type_ == JsonType::Int; }
    bool is_float() const { return type_ == JsonType::Float; }
    bool is_string() const { return type_ == JsonType::String; }
    bool is_array() const { return type_ == JsonType::Array; }
    bool is_object() const { return type_ == JsonType::Object; }

    bool get_bool() const { return bool_; }
    int64_t get_int() const { return int_; }
    double get_float() const { return type_ == JsonType::Float ? float_ : static_cast<double>(int_); }
    const std::string &get_string() const { return str_; }

    std::vector<JsonValue> &array_ref() { return array_; }
    const std::vector<JsonValue> &array_ref() const { return array_; }

    const std::vector<std::pair<std::string, JsonValue>> &object_ref() const {
        return object_;
    }

    // Object access. `get` returns nullptr when the key is absent.
    JsonValue *get(const char *key) {        for (auto &entry : object_) {
            if (entry.first == key) {
                return &entry.second;
            }
        }
        return nullptr;
    }

    const JsonValue *get(const char *key) const {
        for (const auto &entry : object_) {
            if (entry.first == key) {
                return &entry.second;
            }
        }
        return nullptr;
    }

    void set(const std::string &key, JsonValue value) {
        for (auto &entry : object_) {
            if (entry.first == key) {
                entry.second = std::move(value);
                return;
            }
        }
        object_.push_back(std::make_pair(key, std::move(value)));
    }

    void push_back(JsonValue value) { array_.push_back(std::move(value)); }

  private:
    JsonType type_;
    bool bool_;
    int64_t int_;
    double float_;
    std::string str_;
    std::vector<JsonValue> array_;
    std::vector<std::pair<std::string, JsonValue>> object_;
};

namespace detail {

inline void append_escaped(std::string &out, const char *begin, const char *end) {
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

}  // namespace detail

inline std::string serialize(const JsonValue &value) {
    std::string out;
    switch (value.type()) {
        case JsonType::Null:
            out += "null";
            break;
        case JsonType::Bool:
            out += value.get_bool() ? "true" : "false";
            break;
        case JsonType::Int:
            out += std::to_string(value.get_int());
            break;
        case JsonType::Float:
            detail::append_number(out, value.get_float());
            break;
        case JsonType::String: {
            const std::string &str = value.get_string();
            detail::append_escaped(out, str.data(), str.data() + str.size());
            break;
        }
        case JsonType::Array: {
            out.push_back('[');
            bool first = true;
            for (const auto &item : value.array_ref()) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                out += serialize(item);
            }
            out.push_back(']');
            break;
        }
        case JsonType::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto &entry : value.object_ref()) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                detail::append_escaped(out, entry.first.data(),
                                       entry.first.data() + entry.first.size());
                out.push_back(':');
                out += serialize(entry.second);
            }
            out.push_back('}');
            break;
        }
    }
    return out;
}

// Parses a single JSON value starting at *c (which must point at the first
// non-whitespace character of the value) and advances *c past the value.
// Returns true on success.
inline bool parse_value(const char *&c, const char *end, JsonValue &out) {
    switch (*c) {
        case 'n':
            if (end - c >= 4 && c[1] == 'u' && c[2] == 'l' && c[3] == 'l') {
                c += 4;
                out = JsonValue::null();
                return true;
            }
            return false;
        case 't':
            if (end - c >= 4 && c[1] == 'r' && c[2] == 'u' && c[3] == 'e') {
                c += 4;
                out = JsonValue::boolean(true);
                return true;
            }
            return false;
        case 'f':
            if (end - c >= 5 && c[1] == 'a' && c[2] == 'l' && c[3] == 's' &&
                c[4] == 'e') {
                c += 5;
                out = JsonValue::boolean(false);
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
                    out = JsonValue::string(std::move(str));
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
                                str.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                            } else {
                                str.push_back(static_cast<char>(0xE0 | (code >> 12)));
                                str.push_back(
                                    static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
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
            JsonValue arr = JsonValue::array();
            ++c;  // skip '['
            while (true) {
                while (c != end &&
                       (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r')) {
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
                JsonValue item;
                if (!parse_value(c, end, item)) {
                    return false;
                }
                arr.push_back(std::move(item));
                while (c != end &&
                       (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r')) {
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
            JsonValue obj = JsonValue::object();
            ++c;  // skip '{'
            while (true) {
                while (c != end &&
                       (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r')) {
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
                JsonValue key_value;
                if (!parse_value(c, end, key_value)) {
                    return false;
                }
                while (c != end &&
                       (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r')) {
                    ++c;
                }
                if (c == end || *c != ':') {
                    return false;
                }
                ++c;
                JsonValue member;
                if (!parse_value(c, end, member)) {
                    return false;
                }
                obj.set(key_value.get_string(), std::move(member));
                while (c != end &&
                       (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r')) {
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
                    out = JsonValue::floating(std::strtod(num.c_str(), nullptr));
                } else {
                    errno = 0;
                    long long ll = std::strtoll(num.c_str(), nullptr, 10);
                    if (errno == ERANGE || ll > INT64_MAX || ll < INT64_MIN) {
                        // Too large for int64; fall back to double.
                        out = JsonValue::floating(std::strtod(num.c_str(), nullptr));
                    } else {
                        out = JsonValue::integer(ll);
                    }
                }
                return true;
            }
            return false;
        }
    }
}

// Parses one JSON value from [begin, end). Returns true and stores the result
// in `out` on success; `out` is left untouched otherwise. When `consumed_end`
// is non-null it is set to the position just past the parsed value (useful for
// parsing the members of arrays and objects); otherwise only whitespace is
// allowed past the value.
inline bool parse_json(const char *begin, const char *end, JsonValue &out,
                       const char **consumed_end = nullptr) {
    const char *c = begin;

    // Skip leading whitespace.
    while (c != end && (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r')) {
        ++c;
    }

    if (c == end) {
        return false;
    }

    if (!parse_value(c, end, out)) {
        return false;
    }

    if (consumed_end != nullptr) {
        *consumed_end = c;
        return true;
    }

    // Skip trailing whitespace; nothing else may follow the value.
    while (c != end && (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r')) {
        ++c;
    }
    return c == end;
}

// Serializes a response payload into a JsonValue; non-finite doubles are
// turned into `null`.
inline JsonValue make_result(bool ok, JsonValue result, const std::string &error,
                             uint64_t id) {
    JsonValue msg = JsonValue::object();
    msg.set("type", JsonValue::string("response"));
    msg.set("id", JsonValue::integer(static_cast<int64_t>(id)));
    msg.set("ok", JsonValue::boolean(ok));
    if (ok) {
        msg.set("result", std::move(result));
    } else {
        msg.set("error", JsonValue::string(error));
    }
    return msg;
}

}  // namespace ipc
}  // namespace cocotb

#endif /* COCOTB_IPC_JSON_HPP_ */
