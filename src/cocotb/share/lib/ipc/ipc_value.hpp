// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Protocol-neutral value model for the cocotb IPC layer.
//
// Every codec (JSON, binary, ...) and the request dispatcher operate on
// `IpcValue` trees. The model deliberately mirrors the JSON wire model of the
// original implementation so that the JSON codec remains byte-compatible with
// the old newline-delimited protocol, but adds a first-class `Bytes` type so
// the binary codec and the dispatcher never need the `{"__bytes__": ...}`
// dictionary hack.

#ifndef COCOTB_IPC_VALUE_HPP_
#define COCOTB_IPC_VALUE_HPP_

#include <cstdint>  // int64_t, uint64_t
#include <string>
#include <utility>  // move, pair
#include <vector>

namespace cocotb {
namespace ipc {

enum class IpcType { Null, Bool, Int, Float, String, Bytes, Array, Object };

class IpcValue {
  public:
    IpcValue() : type_(IpcType::Null), bool_(false), int_(0), float_(0.0) {}

    static IpcValue null() { return IpcValue(); }

    static IpcValue boolean(bool value) {
        IpcValue v;
        v.type_ = IpcType::Bool;
        v.bool_ = value;
        return v;
    }

    static IpcValue integer(int64_t value) {
        IpcValue v;
        v.type_ = IpcType::Int;
        v.int_ = value;
        return v;
    }

    static IpcValue floating(double value) {
        IpcValue v;
        v.type_ = IpcType::Float;
        v.float_ = value;
        return v;
    }

    static IpcValue string(std::string value) {
        IpcValue v;
        v.type_ = IpcType::String;
        v.str_ = std::move(value);
        return v;
    }

    static IpcValue bytes(std::string value) {
        IpcValue v;
        v.type_ = IpcType::Bytes;
        v.str_ = std::move(value);
        return v;
    }

    static IpcValue array() {
        IpcValue v;
        v.type_ = IpcType::Array;
        return v;
    }

    static IpcValue object() {
        IpcValue v;
        v.type_ = IpcType::Object;
        return v;
    }

    IpcType type() const { return type_; }

    bool is_null() const { return type_ == IpcType::Null; }
    bool is_bool() const { return type_ == IpcType::Bool; }
    bool is_int() const { return type_ == IpcType::Int; }
    bool is_float() const { return type_ == IpcType::Float; }
    bool is_string() const { return type_ == IpcType::String; }
    bool is_bytes() const { return type_ == IpcType::Bytes; }
    bool is_array() const { return type_ == IpcType::Array; }
    bool is_object() const { return type_ == IpcType::Object; }

    bool get_bool() const { return bool_; }
    int64_t get_int() const { return int_; }
    double get_float() const {
        return type_ == IpcType::Float ? float_
                                       : static_cast<double>(int_);
    }
    const std::string &get_string() const { return str_; }
    const std::string &get_bytes() const { return str_; }

    std::vector<IpcValue> &array_ref() { return array_; }
    const std::vector<IpcValue> &array_ref() const { return array_; }

    const std::vector<std::pair<std::string, IpcValue>> &object_ref() const {
        return object_;
    }

    // Object access. `get` returns nullptr when the key is absent.
    IpcValue *get(const char *key) {
        for (auto &entry : object_) {
            if (entry.first == key) {
                return &entry.second;
            }
        }
        return nullptr;
    }

    const IpcValue *get(const char *key) const {
        for (const auto &entry : object_) {
            if (entry.first == key) {
                return &entry.second;
            }
        }
        return nullptr;
    }

    void set(const std::string &key, IpcValue value) {
        for (auto &entry : object_) {
            if (entry.first == key) {
                entry.second = std::move(value);
                return;
            }
        }
        object_.push_back(std::make_pair(key, std::move(value)));
    }

    void push_back(IpcValue value) { array_.push_back(std::move(value)); }

  private:
    IpcType type_;
    bool bool_;
    int64_t int_;
    double float_;
    std::string str_;
    std::vector<IpcValue> array_;
    std::vector<std::pair<std::string, IpcValue>> object_;
};

// Serializes a response payload into an IpcValue; non-finite doubles are
// turned into `null` (matching the historical JSON behavior).
inline IpcValue make_result(bool ok, IpcValue result, const std::string &error,
                            uint64_t id) {
    IpcValue msg = IpcValue::object();
    msg.set("type", IpcValue::string("response"));
    msg.set("id", IpcValue::integer(static_cast<int64_t>(id)));
    msg.set("ok", IpcValue::boolean(ok));
    if (ok) {
        msg.set("result", std::move(result));
    } else {
        msg.set("error", IpcValue::string(error));
    }
    return msg;
}

// Extracts the request id from a message; 0 when absent.
inline uint64_t request_id(const IpcValue &msg) {
    const IpcValue *id = msg.get("id");
    if (id && id->is_int()) {
        return static_cast<uint64_t>(id->get_int());
    }
    return 0;
}

}  // namespace ipc
}  // namespace cocotb

#endif /* COCOTB_IPC_VALUE_HPP_ */