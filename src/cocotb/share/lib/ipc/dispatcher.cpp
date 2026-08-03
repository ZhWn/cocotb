// Copyright cocotb contributors
// Copyright (c) 2013, 2018 Potential Ventures Ltd
// Copyright (c) 2013 SolarFlare Communications Inc
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// IPC request dispatcher: implements the `cocotb.simulator` API on the server
// side by translating requests from the Python process into GPI calls.
//
// The method names and argument order mirror the API of the legacy
// `cocotb.simulator` Python extension, as implemented by the pure-Python
// `cocotb.simulator` module on the client side.

#include "./ipc_priv.hpp"

#include <gpi.h>

#include <cerrno>  // EBUSY, EINVAL, EAGAIN
#include <cstdint>  // int64_t, uint64_t
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>  // move
#include <vector>

namespace cocotb {
namespace ipc {

namespace {

using HandleKind = cocotb::ipc::HandleKind;

std::unordered_map<uint64_t, HandleEntry> &handle_table() {
    static std::unordered_map<uint64_t, HandleEntry> table;
    return table;
}

uint64_t next_handle_id = 1;

template <typename T>
T get_handle(HandleKind kind, uint64_t id) {
    auto &table = handle_table();
    auto it = table.find(id);
    if (it == table.end() || it->second.kind != kind) {
        return nullptr;
    }
    return static_cast<T>(it->second.ptr);
}

/*******************************************************************************
 * Argument parsing helpers
 *******************************************************************************/

class Args {
  public:
    Args(const std::vector<IpcValue> &args, std::string &error)
        : args_(args), error_(error) {}

    bool size(size_t n) {
        if (args_.size() != n) {
            error_ = "Wrong number of arguments";
            return false;
        }
        return true;
    }

    bool get_string(size_t idx, std::string &out) {
        if (idx >= args_.size() || !args_[idx].is_string()) {
            error_ = "Expected a string argument";
            return false;
        }
        out = args_[idx].get_string();
        return true;
    }

    // Byte arrays are first-class IpcValue::bytes values; the codec is
    // responsible for translating them to/from the wire format, so the
    // dispatcher never sees the legacy {"__bytes__": ...} marker.
    bool get_bytes(size_t idx, std::string &out) {
        if (idx >= args_.size() || !args_[idx].is_bytes()) {
            error_ = "Expected a bytes argument";
            return false;
        }
        out = args_[idx].get_bytes();
        return true;
    }

    bool get_int(size_t idx, int64_t &out) {
        if (idx >= args_.size() ||
            (!args_[idx].is_int() && !args_[idx].is_float())) {
            error_ = "Expected an integer argument";
            return false;
        }
        out = args_[idx].get_int();
        return true;
    }

    bool get_uint64(size_t idx, uint64_t &out) {
        int64_t v;
        if (!get_int(idx, v) || v < 0) {
            error_ = "Expected a non-negative integer argument";
            return false;
        }
        out = static_cast<uint64_t>(v);
        return true;
    }

    bool get_double(size_t idx, double &out) {
        if (idx >= args_.size() ||
            (!args_[idx].is_float() && !args_[idx].is_int())) {
            error_ = "Expected a numeric argument";
            return false;
        }
        out = args_[idx].get_float();
        return true;
    }

    bool get_bool(size_t idx, bool &out) {
        if (idx >= args_.size() || !args_[idx].is_bool()) {
            error_ = "Expected a boolean argument";
            return false;
        }
        out = args_[idx].get_bool();
        return true;
    }

  private:
    const std::vector<IpcValue> &args_;
    std::string &error_;
};

/*******************************************************************************
 * Clock implementation, ported from the legacy Python extension
 *******************************************************************************/

class GpiClock {
  public:
    GpiClock(GpiObjHdl *clk_sig) : clk_signal(clk_sig) {}

    ~GpiClock() { stop(); }

    int start(uint64_t period_steps, uint64_t high_steps, bool start_high,
              gpi_set_action set_action);

    int stop();

  private:
    GpiObjHdl *clk_signal = nullptr;
    GpiCbHdl *clk_toggle_cb_hdl = nullptr;

    uint64_t period = 0;
    uint64_t t_high = 0;
    gpi_set_action m_set_action;

    int clk_val = 0;

    int toggle(bool initialSet);
    static int toggle_cb(void *gpi_clk);
};

int GpiClock::start(uint64_t period_steps, uint64_t high_steps, bool start_high,
                    gpi_set_action set_action) {
    if (clk_toggle_cb_hdl) {
        return EBUSY;
    }
    if ((period_steps < 2) || (high_steps < 1) ||
        (high_steps >= period_steps)) {
        return EINVAL;
    }

    period = period_steps;
    t_high = high_steps;
    m_set_action = set_action;

    clk_val = start_high;
    return toggle(true);
}

int GpiClock::stop() {
    if (!clk_toggle_cb_hdl) {
        return -1;
    }
    gpi_remove_cb(clk_toggle_cb_hdl);
    clk_toggle_cb_hdl = nullptr;
    return 0;
}

int GpiClock::toggle(bool initialSet) {
    if (!initialSet) {
        clk_val = !clk_val;
    }
    gpi_set_signal_value_int(clk_signal, clk_val, m_set_action);

    uint64_t to_next_edge = clk_val ? t_high : (period - t_high);

    clk_toggle_cb_hdl =
        gpi_register_timed_callback(&GpiClock::toggle_cb, this, to_next_edge);
    if (!clk_toggle_cb_hdl) {
        return EAGAIN;
    }

    return 0;
}

int GpiClock::toggle_cb(void *gpi_clk) {
    IPC_LOG_TRACE("GPI => [ IPC (GpiClock) ]");
    GpiClock *clk_obj = static_cast<GpiClock *>(gpi_clk);
    int result = clk_obj->toggle(false);
    IPC_LOG_TRACE("[ IPC (GpiClock) ] => GPI");
    return result;
}

}  // namespace

/*******************************************************************************
 * Handle table
 *******************************************************************************/

// The same raw GPI handle may be returned by multiple requests (e.g. repeated
// lookups of the same object). Map it back to a stable id so that Python-side
// equality of handles matches pointer identity, as in the legacy bindings.
//
// The map is keyed by kind as well as pointer: one-shot simulator callbacks
// are deleted after firing (see e.g. VpiCbHdl::run), so a freed callback
// object may be reallocated as an object handle (or vice versa). Keying on the
// pointer alone would alias such reused addresses to stale ids of the wrong
// kind.
std::unordered_map<HandleKind, std::unordered_map<void *, uint64_t>>
    &handle_by_ptr() {
    static std::unordered_map<HandleKind, std::unordered_map<void *, uint64_t>>
        map;
    return map;
}

uint64_t add_handle(HandleKind kind, void *ptr) {
    if (!ptr) {
        return 0;
    }
    auto &by_ptr = handle_by_ptr()[kind];
    auto it = by_ptr.find(ptr);
    if (it != by_ptr.end()) {
        return it->second;
    }
    uint64_t id = next_handle_id++;
    handle_table()[id] = HandleEntry{kind, ptr};
    by_ptr[ptr] = id;
    return id;
}

void *get_handle(HandleKind kind, uint64_t id) {
    auto &table = handle_table();
    auto it = table.find(id);
    if (it == table.end() || it->second.kind != kind) {
        return nullptr;
    }
    return it->second.ptr;
}

void remove_handle(uint64_t id) {
    auto &table = handle_table();
    auto it = table.find(id);
    if (it != table.end()) {
        handle_by_ptr()[it->second.kind].erase(it->second.ptr);
        table.erase(it);
    }
}

/*******************************************************************************
 * Request dispatch
 *******************************************************************************/

bool dispatch_request(const IpcValue &msg, IpcValue &result,
                      std::string &error) {
    const IpcValue *method = msg.get("method");
    const IpcValue *args = msg.get("args");
    if (!method || !method->is_string()) {
        error = "Malformed request: missing method";
        return false;
    }
    if (!args || !args->is_array()) {
        error = "Malformed request: missing args";
        return false;
    }

    const std::string &m = method->get_string();
    const std::vector<IpcValue> &a = args->array_ref();

    if (m == "get_sim_time") {
        uint32_t high, low;
        gpi_get_sim_time(&high, &low);
        IpcValue arr = IpcValue::array();
        arr.push_back(IpcValue::integer(static_cast<int64_t>(high)));
        arr.push_back(IpcValue::integer(static_cast<int64_t>(low)));
        result = std::move(arr);
        return true;
    }

    if (m == "get_precision") {
        int32_t precision;
        gpi_get_sim_precision(&precision);
        result = IpcValue::integer(precision);
        return true;
    }

    if (m == "get_simulator_product") {
        result = IpcValue::string(gpi_get_simulator_product());
        return true;
    }

    if (m == "get_simulator_version") {
        result = IpcValue::string(gpi_get_simulator_version());
        return true;
    }

    if (m == "get_simulator_args") {
        int argc;
        char const *const *argv;
        if (gpi_get_simulator_args(&argc, &argv) != 0) {
            error = "Failed to get simulator arguments";
            return false;
        }
        IpcValue arr = IpcValue::array();
        for (int i = 0; i < argc; i++) {
            arr.push_back(IpcValue::string(argv[i]));
        }
        result = std::move(arr);
        return true;
    }

    if (m == "get_root_handle") {
        Args arg(a, error);
        std::string name;
        if (!arg.get_string(0, name)) {
            return false;
        }
        gpi_sim_hdl hdl = gpi_get_root_handle(
            name.empty() ? nullptr : name.c_str());
        uint64_t id = add_handle(HandleKind::Obj, hdl);
        result = id ? IpcValue::integer(static_cast<int64_t>(id))
                    : IpcValue::null();
        return true;
    }

    if (m == "is_running") {
        result = IpcValue::boolean(gpi_has_registered_impl());
        return true;
    }

    if (m == "package_iterate") {
        gpi_iterator_hdl hdl = gpi_iterate(nullptr, GPI_PACKAGE_SCOPES);
        uint64_t id = add_handle(HandleKind::Iterator, hdl);
        result = id ? IpcValue::integer(static_cast<int64_t>(id))
                    : IpcValue::null();
        return true;
    }

    if (m == "stop_simulator") {
        gpi_finish();
        result = IpcValue::null();
        return true;
    }

    if (m == "set_gpi_log_level") {
        Args arg(a, error);
        int64_t level;
        if (!arg.get_int(0, level)) {
            return false;
        }
        ipc_logging_set_level(static_cast<enum gpi_log_level>(level));
        result = IpcValue::null();
        return true;
    }

    if (m == "initialize_logger") {
        // The Python side registers its logging callbacks; from here on the
        // log handler forwards GPI log messages to the Python process.
        ipc_logging_configure();
        result = IpcValue::null();
        return true;
    }

    if (m == "set_sim_event_callback") {
        // The Python side keeps track of its own callback; the C side simply
        // notifies it via the `end_of_sim_time` callback message.
        result = IpcValue::null();
        return true;
    }

    if (m == "register_readonly_callback" ||
        m == "register_rwsynch_callback" ||
        m == "register_nextstep_callback") {
        Args arg(a, error);
        uint64_t cb_id;
        if (!arg.get_uint64(0, cb_id)) {
            return false;
        }
        IpcCallbackData *data = new IpcCallbackData{cb_id};
        gpi_cb_hdl hdl;
        if (m == "register_readonly_callback") {
            hdl = gpi_register_readonly_callback(ipc_cb_handler, data);
        } else if (m == "register_rwsynch_callback") {
            hdl = gpi_register_readwrite_callback(ipc_cb_handler, data);
        } else {
            hdl = gpi_register_nexttime_callback(ipc_cb_handler, data);
        }
        uint64_t id = add_handle(HandleKind::Callback, hdl);
        result = id ? IpcValue::integer(static_cast<int64_t>(id))
                    : IpcValue::null();
        return true;
    }

    if (m == "register_timed_callback") {
        Args arg(a, error);
        uint64_t time;
        uint64_t cb_id;
        if (!arg.get_uint64(0, time) || !arg.get_uint64(1, cb_id)) {
            return false;
        }
        IpcCallbackData *data = new IpcCallbackData{cb_id};
        gpi_cb_hdl hdl =
            gpi_register_timed_callback(ipc_cb_handler, data, time);
        uint64_t id = add_handle(HandleKind::Callback, hdl);
        result = id ? IpcValue::integer(static_cast<int64_t>(id))
                    : IpcValue::null();
        return true;
    }

    if (m == "register_value_change_callback") {
        Args arg(a, error);
        uint64_t sig_id;
        uint64_t cb_id;
        int64_t edge;
        if (!arg.get_uint64(0, sig_id) || !arg.get_uint64(1, cb_id) ||
            !arg.get_int(2, edge)) {
            return false;
        }
        gpi_sim_hdl sig_hdl = get_handle<gpi_sim_hdl>(HandleKind::Obj, sig_id);
        if (!sig_hdl) {
            error = "Invalid signal handle";
            return false;
        }
        IpcCallbackData *data = new IpcCallbackData{cb_id};
        gpi_cb_hdl hdl = gpi_register_value_change_callback(
            ipc_cb_handler, data, sig_hdl, static_cast<gpi_edge>(edge));
        uint64_t id = add_handle(HandleKind::Callback, hdl);
        result = id ? IpcValue::integer(static_cast<int64_t>(id))
                    : IpcValue::null();
        return true;
    }

    if (m == "deregister_callback") {
        Args arg(a, error);
        uint64_t cb_id;
        if (!arg.get_uint64(0, cb_id)) {
            return false;
        }
        gpi_cb_hdl cb_hdl =
            get_handle<gpi_cb_hdl>(HandleKind::Callback, cb_id);
        if (!cb_hdl) {
            error = "Invalid callback handle";
            return false;
        }
        // Clean up the uncalled callback data, then deregister.
        void *cb_data;
        gpi_get_cb_info(cb_hdl, nullptr, &cb_data);
        delete static_cast<IpcCallbackData *>(cb_data);
        gpi_remove_cb(cb_hdl);
        remove_handle(cb_id);
        result = IpcValue::null();
        return true;
    }

    if (m == "iterate") {
        Args arg(a, error);
        uint64_t hdl_id;
        int64_t type;
        if (!arg.get_uint64(0, hdl_id) || !arg.get_int(1, type)) {
            return false;
        }
        gpi_sim_hdl obj_hdl = get_handle<gpi_sim_hdl>(HandleKind::Obj, hdl_id);
        if (!obj_hdl) {
            error = "Invalid object handle";
            return false;
        }
        gpi_iterator_hdl hdl =
            gpi_iterate(obj_hdl, static_cast<gpi_iterator_sel>(type));
        uint64_t id = add_handle(HandleKind::Iterator, hdl);
        result = id ? IpcValue::integer(static_cast<int64_t>(id))
                    : IpcValue::null();
        return true;
    }

    if (m == "iterator_next") {
        Args arg(a, error);
        uint64_t iter_id;
        if (!arg.get_uint64(0, iter_id)) {
            return false;
        }
        gpi_iterator_hdl iter_hdl =
            get_handle<gpi_iterator_hdl>(HandleKind::Iterator, iter_id);
        if (!iter_hdl) {
            error = "Invalid iterator handle";
            return false;
        }
        gpi_sim_hdl obj_hdl = gpi_next(iter_hdl);
        if (!obj_hdl) {
            // End of iteration.
            result = IpcValue::null();
            return true;
        }
        uint64_t id = add_handle(HandleKind::Obj, obj_hdl);
        result = id ? IpcValue::integer(static_cast<int64_t>(id))
                    : IpcValue::null();
        return true;
    }

    if (m == "get_signal_val_binstr" || m == "get_signal_val_str" ||
        m == "get_signal_val_real" || m == "get_signal_val_long") {
        Args arg(a, error);
        uint64_t hdl_id;
        if (!arg.get_uint64(0, hdl_id)) {
            return false;
        }
        gpi_sim_hdl obj_hdl = get_handle<gpi_sim_hdl>(HandleKind::Obj, hdl_id);
        if (!obj_hdl) {
            error = "Invalid object handle";
            return false;
        }
        if (m == "get_signal_val_binstr") {
            const char *value = gpi_get_signal_value_binstr(obj_hdl);
            if (!value) {
                error = "Simulator yielded a null pointer instead of binstr";
                return false;
            }
            result = IpcValue::string(value);
        } else if (m == "get_signal_val_str") {
            const char *value = gpi_get_signal_value_str(obj_hdl);
            if (!value) {
                error = "Simulator yielded a null pointer instead of string";
                return false;
            }
            result = IpcValue::bytes(std::string(value, std::strlen(value)));
        } else if (m == "get_signal_val_real") {
            result = IpcValue::floating(gpi_get_signal_value_real(obj_hdl));
        } else {
            result = IpcValue::integer(
                static_cast<int64_t>(gpi_get_signal_value_long(obj_hdl)));
        }
        return true;
    }

    if (m == "set_signal_val_binstr" || m == "set_signal_val_str" ||
        m == "set_signal_val_real" || m == "set_signal_val_int") {
        Args arg(a, error);
        uint64_t hdl_id;
        int64_t action;
        if (!arg.get_uint64(0, hdl_id) || !arg.get_int(1, action)) {
            return false;
        }
        gpi_sim_hdl obj_hdl = get_handle<gpi_sim_hdl>(HandleKind::Obj, hdl_id);
        if (!obj_hdl) {
            error = "Invalid object handle";
            return false;
        }
        gpi_set_action set_action = static_cast<gpi_set_action>(action);
        if (m == "set_signal_val_binstr") {
            std::string value;
            if (!arg.get_string(2, value)) {
                return false;
            }
            gpi_set_signal_value_binstr(obj_hdl, value.c_str(), set_action);
        } else if (m == "set_signal_val_str") {
            std::string value;
            if (!arg.get_bytes(2, value)) {
                return false;
            }
            gpi_set_signal_value_str(obj_hdl, value.c_str(), set_action);
        } else if (m == "set_signal_val_real") {
            double value;
            if (!arg.get_double(2, value)) {
                return false;
            }
            gpi_set_signal_value_real(obj_hdl, value, set_action);
        } else {
            int64_t value;
            if (!arg.get_int(2, value)) {
                return false;
            }
            gpi_set_signal_value_int(obj_hdl, static_cast<int32_t>(value),
                                     set_action);
        }
        result = IpcValue::null();
        return true;
    }

    if (m == "get_definition_name" || m == "get_definition_file") {
        Args arg(a, error);
        uint64_t hdl_id;
        if (!arg.get_uint64(0, hdl_id)) {
            return false;
        }
        gpi_sim_hdl obj_hdl = get_handle<gpi_sim_hdl>(HandleKind::Obj, hdl_id);
        if (!obj_hdl) {
            error = "Invalid object handle";
            return false;
        }
        const char *value = m == "get_definition_name"
                                ? gpi_get_definition_name(obj_hdl)
                                : gpi_get_definition_file(obj_hdl);
        if (!value) {
            error = "Simulator yielded a null pointer instead of a string";
            return false;
        }
        result = IpcValue::string(value);
        return true;
    }

    if (m == "get_handle_by_name") {
        Args arg(a, error);
        uint64_t hdl_id;
        std::string name;
        int64_t discovery = GPI_AUTO;
        if (!arg.get_uint64(0, hdl_id) || !arg.get_string(1, name)) {
            return false;
        }
        if (a.size() > 2) {
            if (!arg.get_int(2, discovery)) {
                return false;
            }
            if (discovery < GPI_AUTO || discovery > GPI_NATIVE) {
                error = "Enum value for discovery_method out of range";
                return false;
            }
        }
        gpi_sim_hdl obj_hdl = get_handle<gpi_sim_hdl>(HandleKind::Obj, hdl_id);
        if (!obj_hdl) {
            error = "Invalid object handle";
            return false;
        }
        gpi_sim_hdl child = gpi_get_handle_by_name(
            obj_hdl, name.c_str(), static_cast<gpi_discovery>(discovery));
        uint64_t id = add_handle(HandleKind::Obj, child);
        result = id ? IpcValue::integer(static_cast<int64_t>(id))
                    : IpcValue::null();
        return true;
    }

    if (m == "get_handle_by_index") {
        Args arg(a, error);
        uint64_t hdl_id;
        int64_t index;
        if (!arg.get_uint64(0, hdl_id) || !arg.get_int(1, index)) {
            return false;
        }
        gpi_sim_hdl obj_hdl = get_handle<gpi_sim_hdl>(HandleKind::Obj, hdl_id);
        if (!obj_hdl) {
            error = "Invalid object handle";
            return false;
        }
        gpi_sim_hdl child = gpi_get_handle_by_index(
            obj_hdl, static_cast<int32_t>(index));
        uint64_t id = add_handle(HandleKind::Obj, child);
        result = id ? IpcValue::integer(static_cast<int64_t>(id))
                    : IpcValue::null();
        return true;
    }

    if (m == "get_name_string" || m == "get_type_string") {
        Args arg(a, error);
        uint64_t hdl_id;
        if (!arg.get_uint64(0, hdl_id)) {
            return false;
        }
        gpi_sim_hdl obj_hdl = get_handle<gpi_sim_hdl>(HandleKind::Obj, hdl_id);
        if (!obj_hdl) {
            error = "Invalid object handle";
            return false;
        }
        const char *value = m == "get_name_string"
                                ? gpi_get_signal_name_str(obj_hdl)
                                : gpi_get_signal_type_str(obj_hdl);
        if (!value) {
            error = "Simulator yielded a null pointer instead of a string";
            return false;
        }
        result = IpcValue::string(value);
        return true;
    }

    if (m == "get_type" || m == "get_const" || m == "get_signed" ||
        m == "get_num_elems" || m == "get_indexable" || m == "get_range") {
        Args arg(a, error);
        uint64_t hdl_id;
        if (!arg.get_uint64(0, hdl_id)) {
            return false;
        }
        gpi_sim_hdl obj_hdl = get_handle<gpi_sim_hdl>(HandleKind::Obj, hdl_id);
        if (!obj_hdl) {
            error = "Invalid object handle";
            return false;
        }
        if (m == "get_type") {
            result = IpcValue::integer(gpi_get_object_type(obj_hdl));
        } else if (m == "get_const") {
            result = IpcValue::boolean(gpi_is_constant(obj_hdl) != 0);
        } else if (m == "get_signed") {
            result = IpcValue::integer(gpi_is_signed(obj_hdl));
        } else if (m == "get_num_elems") {
            result = IpcValue::integer(gpi_get_num_elems(obj_hdl));
        } else if (m == "get_indexable") {
            result = IpcValue::boolean(gpi_is_indexable(obj_hdl) != 0);
        } else {
            IpcValue arr = IpcValue::array();
            arr.push_back(IpcValue::integer(gpi_get_range_left(obj_hdl)));
            arr.push_back(IpcValue::integer(gpi_get_range_right(obj_hdl)));
            arr.push_back(IpcValue::integer(gpi_get_range_dir(obj_hdl)));
            result = std::move(arr);
        }
        return true;
    }

    if (m == "clock_create") {
        Args arg(a, error);
        uint64_t hdl_id;
        if (!arg.get_uint64(0, hdl_id)) {
            return false;
        }
        gpi_sim_hdl obj_hdl = get_handle<gpi_sim_hdl>(HandleKind::Obj, hdl_id);
        if (!obj_hdl) {
            error = "Invalid object handle";
            return false;
        }
        GpiClock *gpi_clk = new GpiClock(obj_hdl);
        uint64_t id = add_handle(HandleKind::Clock, gpi_clk);
        result = id ? IpcValue::integer(static_cast<int64_t>(id))
                    : IpcValue::null();
        return true;
    }

    if (m == "delete_clock") {
        Args arg(a, error);
        uint64_t clk_id;
        if (!arg.get_uint64(0, clk_id)) {
            return false;
        }
        GpiClock *gpi_clk =
            get_handle<GpiClock *>(HandleKind::Clock, clk_id);
        if (!gpi_clk) {
            error = "Invalid clock handle";
            return false;
        }
        delete gpi_clk;
        remove_handle(clk_id);
        result = IpcValue::null();
        return true;
    }

    if (m == "clock_start" || m == "clock_stop") {
        Args arg(a, error);
        uint64_t clk_id;
        if (!arg.get_uint64(0, clk_id)) {
            return false;
        }
        GpiClock *gpi_clk =
            get_handle<GpiClock *>(HandleKind::Clock, clk_id);
        if (!gpi_clk) {
            error = "Invalid clock handle";
            return false;
        }
        if (m == "clock_stop") {
            gpi_clk->stop();
            result = IpcValue::integer(0);
            return true;
        }
        uint64_t period, high;
        bool start_high;
        int64_t set_action;
        if (!arg.get_uint64(1, period) || !arg.get_uint64(2, high) ||
            !arg.get_bool(3, start_high) || !arg.get_int(4, set_action)) {
            return false;
        }
        // Return the errno-style error code; the Python side maps it to the
        // appropriate exception type.
        int ret = gpi_clk->start(
            period, high, start_high, static_cast<gpi_set_action>(set_action));
        result = IpcValue::integer(ret);
        return true;
    }

    error = "Unknown method: " + m;
    return false;
}

}  // namespace ipc
}  // namespace cocotb
