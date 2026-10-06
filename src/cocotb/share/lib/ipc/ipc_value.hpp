// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Dynamic value used to represent IPC messages and their payloads.
//
// Messages are trees of IpcValue (objects containing values, values
// containing arrays/objects, ...). Codecs turn the tree into bytes and back.

#ifndef COCOTB_IPC_VALUE_HPP_
#define COCOTB_IPC_VALUE_HPP_

#include <cstdint>  // int64_t, uint64_t
#include <map>      // std::map
#include <string>   // std::string
#include <utility>  // std::move
#include <vector>   // std::vector

namespace cocotb {
namespace ipc {

enum class IpcType { Null, Bool, Int, Float, String, Bytes, Array, Object };

class IpcValue {
  public:
    IpcValue() : type_(IpcType::Null), b_(false), i_(0), d_(0.0) {}

    IpcValue(const IpcValue &) = default;
    IpcValue(IpcValue &&) = default;
    IpcValue &operator=(const IpcValue &) = default;
    IpcValue &operator=(IpcValue &&) = default;
    ~IpcValue() = default;

    static IpcValue null() { return IpcValue(); }

    static IpcValue boolean(bool v) {
        IpcValue x;
        x.type_ = IpcType::Bool;
        x.b_ = v;
        return x;
    }

    static IpcValue integer(int64_t v) {
        IpcValue x;
        x.type_ = IpcType::Int;
        x.i_ = v;
        return x;
    }

    static IpcValue floating(double v) {
        IpcValue x;
        x.type_ = IpcType::Float;
        x.d_ = v;
        return x;
    }

    static IpcValue string(std::string v) {
        IpcValue x;
        x.type_ = IpcType::String;
        x.s_ = std::move(v);
        return x;
    }

    static IpcValue bytes(std::string v) {
        IpcValue x;
        x.type_ = IpcType::Bytes;
        x.s_ = std::move(v);
        return x;
    }

    static IpcValue array() {
        IpcValue x;
        x.type_ = IpcType::Array;
        return x;
    }

    static IpcValue object() {
        IpcValue x;
        x.type_ = IpcType::Object;
        return x;
    }

    IpcType type() const { return type_; }

    bool is_null() const { return type_ == IpcType::Null; }
    bool is_bool() const { return type_ == IpcType::Bool; }
    bool is_int() const { return type_ == IpcType::Int; }
    bool is_float() const { return type_ == IpcType::Float; }
    bool is_numeric() const {
        // Bool counts as numeric: Python's bool is an int subclass and the
        // legacy FFI accepted `dut.x.value = False` by converting it with
        // PyLong_AsLong. The binary codec keeps bools distinct because
        // Clock.start passes one where the C side wants a real bool, so the
        // widening has to happen here.
        return type_ == IpcType::Int || type_ == IpcType::Float ||
               type_ == IpcType::Bool;
    }
    bool is_string() const { return type_ == IpcType::String; }
    bool is_bytes() const { return type_ == IpcType::Bytes; }
    bool is_array() const { return type_ == IpcType::Array; }
    bool is_object() const { return type_ == IpcType::Object; }

    bool get_bool() const { return b_; }

    // Ints are returned as-is; floats are truncated toward zero so that a
    // JSON-encoded integer arriving as a float still works; bools widen to
    // 0/1 for the same reason.
    int64_t get_int() const {
        if (type_ == IpcType::Int) {
            return i_;
        }
        if (type_ == IpcType::Float) {
            return static_cast<int64_t>(d_);
        }
        if (type_ == IpcType::Bool) {
            return b_ ? 1 : 0;
        }
        return 0;
    }

    double get_float() const {
        if (type_ == IpcType::Float) {
            return d_;
        }
        if (type_ == IpcType::Int) {
            return static_cast<double>(i_);
        }
        if (type_ == IpcType::Bool) {
            return b_ ? 1.0 : 0.0;
        }
        return 0.0;
    }

    const std::string &get_string() const { return s_; }
    const std::string &get_bytes() const { return s_; }

    const std::vector<IpcValue> &array_ref() const { return a_; }
    std::vector<IpcValue> &array_ref() { return a_; }

    const std::map<std::string, IpcValue> &object_ref() const { return o_; }
    std::map<std::string, IpcValue> &object_ref() { return o_; }

    size_t size() const {
        if (type_ == IpcType::Array) {
            return a_.size();
        }
        if (type_ == IpcType::Object) {
            return o_.size();
        }
        return 0;
    }

    // Returns nullptr when the key is missing or this is not an object.
    const IpcValue *get(const std::string &key) const {
        if (type_ != IpcType::Object) {
            return nullptr;
        }
        auto it = o_.find(key);
        if (it == o_.end()) {
            return nullptr;
        }
        return &it->second;
    }

    void set(const std::string &key, IpcValue value) {
        if (type_ != IpcType::Object) {
            type_ = IpcType::Object;
            o_.clear();
        }
        o_[key] = std::move(value);
    }

    void push_back(IpcValue value) {
        if (type_ != IpcType::Array) {
            type_ = IpcType::Array;
            a_.clear();
        }
        a_.push_back(std::move(value));
    }

  private:
    IpcType type_;
    bool b_;
    int64_t i_;
    double d_;
    std::string s_;
    std::vector<IpcValue> a_;
    std::map<std::string, IpcValue> o_;
};

}  // namespace ipc
}  // namespace cocotb

#endif /* COCOTB_IPC_VALUE_HPP_ */
