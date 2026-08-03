// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

#include "./codec_binary.hpp"

#include <cstdint>
#include <cstring>  // memcpy
#include <string>
#include <utility>  // move
#include <vector>

namespace cocotb {
namespace ipc {
namespace codec_binary {

namespace {

enum MsgType : uint8_t {
    kRequest = 0,
    kResponse = 1,
    kCallback = 2,
    kCallbackAck = 3,
    kLog = 4,
};

enum ValueTag : uint8_t {
    kNull = 0x00,
    kBool = 0x01,
    kInt = 0x02,
    kReal = 0x03,
    kString = 0x04,
    kBytes = 0x05,
    kArray = 0x06,
    kObject = 0x07,
};

/*******************************************************************************
 * Writer
 *******************************************************************************/

void put_u8(std::string &out, uint8_t v) {
    out.push_back(static_cast<char>(v));
}

void put_u32(std::string &out, uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

void put_u64(std::string &out, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

void put_string(std::string &out, const std::string &s) {
    put_u8(out, kString);
    put_u32(out, static_cast<uint32_t>(s.size()));
    out.append(s.data(), s.size());
}

void put_value(const IpcValue &value, std::string &out);

void put_value(const IpcValue &value, std::string &out) {
    switch (value.type()) {
        case IpcType::Null:
            put_u8(out, kNull);
            break;
        case IpcType::Bool:
            put_u8(out, kBool);
            put_u8(out, value.get_bool() ? 1 : 0);
            break;
        case IpcType::Int:
            put_u8(out, kInt);
            put_u64(out, static_cast<uint64_t>(value.get_int()));
            break;
        case IpcType::Float: {
            put_u8(out, kReal);
            uint64_t bits = 0;
            double v = value.get_float();
            std::memcpy(&bits, &v, sizeof(bits));
            put_u64(out, bits);
            break;
        }
        case IpcType::String:
            put_string(out, value.get_string());
            break;
        case IpcType::Bytes:
            put_u8(out, kBytes);
            put_u32(out, static_cast<uint32_t>(value.get_bytes().size()));
            out.append(value.get_bytes().data(), value.get_bytes().size());
            break;
        case IpcType::Array: {
            put_u8(out, kArray);
            const std::vector<IpcValue> &items = value.array_ref();
            put_u32(out, static_cast<uint32_t>(items.size()));
            for (const auto &item : items) {
                put_value(item, out);
            }
            break;
        }
        case IpcType::Object: {
            put_u8(out, kObject);
            const auto &members = value.object_ref();
            put_u32(out, static_cast<uint32_t>(members.size()));
            for (const auto &entry : members) {
                put_u32(out, static_cast<uint32_t>(entry.first.size()));
                out.append(entry.first.data(), entry.first.size());
                put_value(entry.second, out);
            }
            break;
        }
    }
}

inline void put_message(const IpcValue &msg, std::string &out) {
    const std::string type = msg.get("type")->get_string();
    if (type == "request") {
        put_u8(out, kRequest);
        put_u64(out, static_cast<uint64_t>(msg.get("id")->get_int()));
        put_string(out, msg.get("method")->get_string());
        const IpcValue *args = msg.get("args");
        put_u32(out, static_cast<uint32_t>(args->array_ref().size()));
        for (const auto &arg : args->array_ref()) {
            put_value(arg, out);
        }
    } else if (type == "response") {
        put_u8(out, kResponse);
        put_u64(out, static_cast<uint64_t>(msg.get("id")->get_int()));
        const IpcValue *ok = msg.get("ok");
        if (ok && ok->is_bool() && ok->get_bool()) {
            put_u8(out, 1);
            put_value(*msg.get("result"), out);
        } else {
            put_u8(out, 0);
            put_string(out, msg.get("error")->get_string());
        }
    } else if (type == "callback") {
        put_u8(out, kCallback);
        put_u64(out, static_cast<uint64_t>(msg.get("id")->get_int()));
        put_string(out, msg.get("func")->get_string());
        const IpcValue *cb_id = msg.get("cb_id");
        put_u64(out, static_cast<uint64_t>(cb_id ? cb_id->get_int() : 0));
    } else if (type == "callback_ack") {
        put_u8(out, kCallbackAck);
        put_u64(out, static_cast<uint64_t>(msg.get("id")->get_int()));
        put_u64(out, static_cast<uint64_t>(msg.get("result")->get_int()));
    } else if (type == "log") {
        put_u8(out, kLog);
        put_u64(out, static_cast<uint64_t>(msg.get("level")->get_int()));
        put_string(out, msg.get("logger")->get_string());
        put_string(out, msg.get("filename")->get_string());
        put_u64(out, static_cast<uint64_t>(msg.get("lineno")->get_int()));
        put_string(out, msg.get("msg")->get_string());
        put_string(out, msg.get("function")->get_string());
    }
}

/*******************************************************************************
 * Decoder
 *******************************************************************************/

class Reader {
  public:
    Reader(const char *begin, const char *end)
        : p_(begin), end_(end), ok_(true) {}

    bool u8(uint8_t &v) {
        if (!ok_ || end_ - p_ < 1) {
            return fail();
        }
        v = static_cast<uint8_t>(*p_++);
        return true;
    }

    bool u32(uint32_t &v) {
        if (!ok_ || end_ - p_ < 4) {
            return fail();
        }
        v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<uint32_t>(static_cast<uint8_t>(p_[i])) << (8 * i);
        }
        p_ += 4;
        return true;
    }

    bool u64(uint64_t &v) {
        if (!ok_ || end_ - p_ < 8) {
            return fail();
        }
        v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<uint64_t>(static_cast<uint8_t>(p_[i]))
                 << (8 * i);
        }
        p_ += 8;
        return true;
    }

    bool take(size_t n, const char *&out) {
        if (!ok_ || static_cast<size_t>(end_ - p_) < n) {
            return fail();
        }
        out = p_;
        p_ += n;
        return true;
    }

    bool ok() const { return ok_; }

  private:
    bool fail() {
        ok_ = false;
        return false;
    }

    const char *p_;
    const char *end_;
    bool ok_;
};

bool read_string(Reader &r, std::string &out) {
    uint8_t tag;
    uint32_t len;
    if (!r.u8(tag) || tag != kString || !r.u32(len)) {
        return false;
    }
    const char *data;
    if (!r.take(len, data)) {
        return false;
    }
    out.assign(data, len);
    return true;
}

bool read_value(Reader &r, IpcValue &out) {
    uint8_t tag;
    if (!r.u8(tag)) {
        return false;
    }
    switch (tag) {
        case kNull:
            out = IpcValue::null();
            return true;
        case kBool: {
            uint8_t v;
            if (!r.u8(v)) {
                return false;
            }
            out = IpcValue::boolean(v != 0);
            return true;
        }
        case kInt: {
            uint64_t v;
            if (!r.u64(v)) {
                return false;
            }
            out = IpcValue::integer(static_cast<int64_t>(v));
            return true;
        }
        case kReal: {
            uint64_t bits;
            if (!r.u64(bits)) {
                return false;
            }
            double v = 0.0;
            std::memcpy(&v, &bits, sizeof(v));
            out = IpcValue::floating(v);
            return true;
        }
        case kString: {
            uint32_t len;
            const char *data;
            if (!r.u32(len) || !r.take(len, data)) {
                return false;
            }
            out = IpcValue::string(std::string(data, len));
            return true;
        }
        case kBytes: {
            uint32_t len;
            const char *data;
            if (!r.u32(len) || !r.take(len, data)) {
                return false;
            }
            out = IpcValue::bytes(std::string(data, len));
            return true;
        }
        case kArray: {
            uint32_t count;
            if (!r.u32(count)) {
                return false;
            }
            IpcValue arr = IpcValue::array();
            for (uint32_t i = 0; i < count; ++i) {
                IpcValue item;
                if (!read_value(r, item)) {
                    return false;
                }
                arr.push_back(std::move(item));
            }
            out = std::move(arr);
            return true;
        }
        case kObject: {
            uint32_t count;
            if (!r.u32(count)) {
                return false;
            }
            IpcValue obj = IpcValue::object();
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t keylen;
                const char *key_data;
                if (!r.u32(keylen) || !r.take(keylen, key_data)) {
                    return false;
                }
                IpcValue member;
                if (!read_value(r, member)) {
                    return false;
                }
                obj.set(std::string(key_data, keylen), std::move(member));
            }
            out = std::move(obj);
            return true;
        }
        default:
            return false;
    }
}

bool read_message(Reader &r, IpcValue &out) {
    uint8_t type;
    if (!r.u8(type)) {
        return false;
    }
    switch (type) {
        case kRequest: {
            uint64_t id;
            std::string method;
            uint32_t nargs;
            if (!r.u64(id) || !read_string(r, method) || !r.u32(nargs)) {
                return false;
            }
            IpcValue args = IpcValue::array();
            for (uint32_t i = 0; i < nargs; ++i) {
                IpcValue arg;
                if (!read_value(r, arg)) {
                    return false;
                }
                args.push_back(std::move(arg));
            }
            IpcValue msg = IpcValue::object();
            msg.set("type", IpcValue::string("request"));
            msg.set("id", IpcValue::integer(static_cast<int64_t>(id)));
            msg.set("method", IpcValue::string(std::move(method)));
            msg.set("args", std::move(args));
            out = std::move(msg);
            return true;
        }
        case kResponse: {
            uint64_t id;
            uint8_t ok;
            if (!r.u64(id) || !r.u8(ok)) {
                return false;
            }
            IpcValue msg = IpcValue::object();
            msg.set("type", IpcValue::string("response"));
            msg.set("id", IpcValue::integer(static_cast<int64_t>(id)));
            msg.set("ok", IpcValue::boolean(ok != 0));
            if (ok) {
                IpcValue result;
                if (!read_value(r, result)) {
                    return false;
                }
                msg.set("result", std::move(result));
            } else {
                std::string error;
                if (!read_string(r, error)) {
                    return false;
                }
                msg.set("error", IpcValue::string(std::move(error)));
            }
            out = std::move(msg);
            return true;
        }
        case kCallback: {
            uint64_t id;
            std::string func;
            uint64_t cb_id;
            if (!r.u64(id) || !read_string(r, func) || !r.u64(cb_id)) {
                return false;
            }
            IpcValue msg = IpcValue::object();
            msg.set("type", IpcValue::string("callback"));
            msg.set("id", IpcValue::integer(static_cast<int64_t>(id)));
            msg.set("func", IpcValue::string(std::move(func)));
            msg.set("cb_id", IpcValue::integer(static_cast<int64_t>(cb_id)));
            out = std::move(msg);
            return true;
        }
        case kCallbackAck: {
            uint64_t id;
            uint64_t result;
            if (!r.u64(id) || !r.u64(result)) {
                return false;
            }
            IpcValue msg = IpcValue::object();
            msg.set("type", IpcValue::string("callback_ack"));
            msg.set("id", IpcValue::integer(static_cast<int64_t>(id)));
            msg.set("result", IpcValue::integer(static_cast<int64_t>(result)));
            out = std::move(msg);
            return true;
        }
        case kLog: {
            uint64_t level;
            std::string logger;
            std::string filename;
            uint64_t lineno;
            std::string msg;
            std::string function;
            if (!r.u64(level) || !read_string(r, logger) ||
                !read_string(r, filename) || !r.u64(lineno) ||
                !read_string(r, msg) || !read_string(r, function)) {
                return false;
            }
            IpcValue log = IpcValue::object();
            log.set("type", IpcValue::string("log"));
            log.set("level", IpcValue::integer(static_cast<int64_t>(level)));
            log.set("logger", IpcValue::string(std::move(logger)));
            log.set("filename", IpcValue::string(std::move(filename)));
            log.set("lineno", IpcValue::integer(static_cast<int64_t>(lineno)));
            log.set("msg", IpcValue::string(std::move(msg)));
            log.set("function", IpcValue::string(std::move(function)));
            out = std::move(log);
            return true;
        }
        default:
            return false;
    }
}

}  // namespace

std::string encode(const IpcValue &value) {
    std::string out;
    if (value.is_object() && value.get("type") &&
        value.get("type")->is_string()) {
        put_message(value, out);
    } else {
        put_value(value, out);
    }
    return out;
}

bool decode(const char *begin, const char *end, IpcValue &value) {
    Reader r(begin, end);
    return read_message(r, value) && r.ok();
}

}  // namespace codec_binary
}  // namespace ipc
}  // namespace cocotb