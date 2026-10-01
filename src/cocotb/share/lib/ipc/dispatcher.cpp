// Copyright cocotb contributors
// Copyright (c) 2013, 2018 Potential Ventures Ltd
// Copyright (c) 2013 SolarFlare Communications Inc
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// IPC request dispatcher: implements the `cocotb.simulator` API on the server
// side by translating requests from the Python child process into GPI calls.
//
// The method names and argument order mirror the API of the legacy
// `cocotb.simulator` Python extension module, as implemented by the
// pure-Python `cocotb.simulator` module on the client side.
//
// Handles are referenced over the wire by opaque integer ids. Ids are
// allocated by the server and are stable for repeated lookups of the same
// underlying pointer, so Python-side identity comparisons behave the same
// way they did with the C extension (which compared raw pointers).

#include "./ipc_priv.hpp"

#include <gpi.h>

#include <cerrno>   // EBUSY, EINVAL, EAGAIN
#include <cstdint>  // int64_t, uint64_t
#include <cstring>  // strcmp
#include <map>
#include <string>
#include <unordered_map>
#include <utility>  // move
#include <vector>

namespace cocotb {
namespace ipc {

namespace {

using cocotb::ipc::IpcCallbackData;

/*******************************************************************************
 * Handle table
 *******************************************************************************/

enum class HandleKind { Obj, Iterator, Callback, Clock };

struct HandleEntry {
    HandleKind kind;
    void *ptr;
};

std::unordered_map<uint64_t, HandleEntry> &handle_table() {
    static std::unordered_map<uint64_t, HandleEntry> table;
    return table;
}

// Reverse lookups keep ids stable for repeated lookups of the same pointer
// and guard against pointer reuse aliasing an id of a different kind.
std::unordered_map<void *, uint64_t> &ptr_map(HandleKind kind) {
    static std::unordered_map<void *, uint64_t> objs;
    static std::unordered_map<void *, uint64_t> iterators;
    static std::unordered_map<void *, uint64_t> callbacks;
    static std::unordered_map<void *, uint64_t> clocks;
    switch (kind) {
        case HandleKind::Obj:
            return objs;
        case HandleKind::Iterator:
            return iterators;
        case HandleKind::Callback:
            return callbacks;
        case HandleKind::Clock:
            return clocks;
    }
    return objs;  // unreachable
}

uint64_t next_handle_id = 1;

uint64_t add_handle(HandleKind kind, void *ptr) {
    if (ptr == nullptr) {
        return 0;
    }
    auto &by_ptr = ptr_map(kind);
    auto it = by_ptr.find(ptr);
    if (it != by_ptr.end()) {
        // Same underlying object (or a recycled address with the same kind):
        // keep the id stable, refreshing the entry.
        handle_table()[it->second] = HandleEntry{kind, ptr};
        return it->second;
    }
    uint64_t id = next_handle_id++;
    handle_table()[id] = HandleEntry{kind, ptr};
    by_ptr[ptr] = id;
    return id;
}

void remove_handle(uint64_t id) {
    auto it = handle_table().find(id);
    if (it == handle_table().end()) {
        return;
    }
    auto &by_ptr = ptr_map(it->second.kind);
    auto pit = by_ptr.find(it->second.ptr);
    if (pit != by_ptr.end() && pit->second == id) {
        by_ptr.erase(pit);
    }
    handle_table().erase(it);
}

template <typename T>
T lookup_handle(HandleKind kind, uint64_t id) {
    auto it = handle_table().find(id);
    if (it == handle_table().end() || it->second.kind != kind) {
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

    // Note: numeric getters accept a float too so that JSON (which cannot
    // distinguish ints from floats written without a decimal point, or values
    // that lost precision) round-trips like the binary codec does.

    bool get_int(size_t i, int64_t &out) {
        if (i >= args_.size() || !args_[i].is_numeric()) {
            error_ = "Expected an integer argument";
            return false;
        }
        out = args_[i].get_int();
        return true;
    }

    bool get_uint64(size_t i, uint64_t &out) {
        if (i >= args_.size() || !args_[i].is_numeric()) {
            error_ = "Expected an unsigned integer argument";
            return false;
        }
        out = static_cast<uint64_t>(args_[i].get_int());
        return true;
    }

    bool get_uint32(size_t i, uint32_t &out) {
        uint64_t v;
        if (!get_uint64(i, v)) {
            return false;
        }
        out = static_cast<uint32_t>(v);
        return true;
    }

    bool get_bool(size_t i, bool &out) {
        if (i >= args_.size() || !args_[i].is_bool()) {
            error_ = "Expected a boolean argument";
            return false;
        }
        out = args_[i].get_bool();
        return true;
    }

    bool get_double(size_t i, double &out) {
        if (i >= args_.size() || !args_[i].is_numeric()) {
            error_ = "Expected a float argument";
            return false;
        }
        out = args_[i].get_float();
        return true;
    }

    // String argument; also accepts null to mean "no name" (used by
    // get_root_handle, whose legacy signature accepted None).
    bool get_string(size_t i, std::string &out) {
        if (i >= args_.size() || !args_[i].is_string()) {
            error_ = "Expected a string argument";
            return false;
        }
        out = args_[i].get_string();
        return true;
    }

    bool get_string_or_null(size_t i, std::string &out, bool &is_null) {
        if (i >= args_.size()) {
            error_ = "Missing argument";
            return false;
        }
        if (args_[i].is_null()) {
            is_null = true;
            out.clear();
            return true;
        }
        if (!args_[i].is_string()) {
            error_ = "Expected a string or None";
            return false;
        }
        is_null = false;
        out = args_[i].get_string();
        return true;
    }

    bool get_handle(size_t i, HandleKind kind, void *&out) {
        uint64_t id;
        if (!get_uint64(i, id)) {
            return false;
        }
        // Qualified so lookup does not find this member function instead of
        // the free template of the same name.
        out = cocotb::ipc::lookup_handle<void *>(kind, id);
        if (out == nullptr) {
            error_ = "Invalid or stale handle";
            return false;
        }
        return true;
    }

  private:
    const std::vector<IpcValue> &args_;
    std::string &error_;
};

/*******************************************************************************
 * Server-side clock (moved from the legacy pygpi extension)
 *******************************************************************************/

class GpiClock {
  public:
    GpiClock(GpiObjHdl *clk_sig) : clk_signal(clk_sig) {}

    ~GpiClock() { stop(); }

    // Start the clock. Returns nonzero in case of failure:
    //  - EBUSY if the clock was already started (stop first)
    //  - EINVAL if the parameters are invalid
    //  - EAGAIN if registering the toggle callback failed
    int start(uint64_t period_steps, uint64_t high_steps, bool start_high,
              gpi_set_action set_action);

    int stop();

  private:
    GpiObjHdl *clk_signal = nullptr;
    GpiCbHdl *clk_toggle_cb_hdl = nullptr;

    uint64_t period = 0;
    uint64_t t_high = 0;
    gpi_set_action m_set_action = GPI_DEPOSIT;

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
        // LCOV_EXCL_START
        if (!initialSet) {
            // Failing when called from start() will be reported via
            // exception, but log in case of later failure that would
            // otherwise be silent.
            IPC_LOG_ERROR(
                "Clock will be stopped: failed to register toggle cb");
        }
        return EAGAIN;
        // LCOV_EXCL_STOP
    }

    return 0;
}

int GpiClock::toggle_cb(void *gpi_clk) {
    IPC_LOG_TRACE("GPI => [ IPC (GpiClock) ]");
    GpiClock *clk_obj = (GpiClock *)gpi_clk;
    int result = clk_obj->toggle(false);
    IPC_LOG_TRACE("[ IPC (GpiClock) ] => GPI");
    return result;
}

/*******************************************************************************
 * Callback trampoline
 *******************************************************************************/

}  // namespace

// Fires when the simulator invokes a callback registered for Python: sends a
// `callback` message and serves requests from Python reentrantly until the
// matching `callback_ack` arrives. Returns Python's result, or -1 when the
// connection is lost.
int ipc_cb_handler(void *user_data) {
    IpcCallbackData *data = static_cast<IpcCallbackData *>(user_data);

    // Piggyback the current simulation time on the event so the Python side
    // can answer get_sim_time() without a round trip while handling it.
    uint32_t high = 0;
    uint32_t low = 0;
    gpi_get_sim_time(&high, &low);
    IpcValue time = IpcValue::array();
    time.push_back(IpcValue::integer(high));
    time.push_back(IpcValue::integer(low));

    IpcValue msg = IpcValue::object();
    msg.set("type", IpcValue::string("callback"));
    msg.set("id", IpcValue::integer(0));  // assigned when sent
    msg.set("func", IpcValue::string("gpi"));
    msg.set("cb_id", IpcValue::integer(static_cast<int64_t>(data->cb_id)));
    msg.set("time", std::move(time));

    int result = cocotb::ipc::send_callback_and_wait(msg);

    // Callbacks are one-shot: GPI frees the callback object after this
    // handler returns, so free our user data here (legacy deleted the
    // PythonCallback in handle_gpi_callback the same way).
    delete data;
    return result;
}

namespace {

/*******************************************************************************
 * Request dispatch
 *******************************************************************************/

bool dispatch_one(const std::string &m, const std::vector<IpcValue> &a,
                  IpcValue &result, std::string &error) {
    // Batch of requests in a single round trip. The client sends one array
    // argument holding the entries, each entry an array of [method, *args];
    // the result is an array of [ok, value|error] pairs.
    if (m == "batch") {
        if (a.size() != 1 || !a[0].is_array()) {
            error = "Malformed batch payload";
            return false;
        }
        IpcValue out = IpcValue::array();
        for (const auto &entry : a[0].array_ref()) {
            IpcValue pair = IpcValue::array();
            if (!entry.is_array() || entry.size() == 0 ||
                !entry.array_ref()[0].is_string()) {
                pair.push_back(IpcValue::boolean(false));
                pair.push_back(IpcValue::string("Malformed batch entry"));
                out.push_back(std::move(pair));
                continue;
            }
            const std::vector<IpcValue> &items = entry.array_ref();
            std::string sub_method = items[0].get_string();
            std::vector<IpcValue> sub_args(items.begin() + 1, items.end());
            IpcValue sub_result;
            std::string sub_error;
            bool ok = dispatch_one(sub_method, sub_args, sub_result,
                                   sub_error);
            pair.push_back(IpcValue::boolean(ok));
            if (ok) {
                pair.push_back(std::move(sub_result));
            } else {
                pair.push_back(IpcValue::string(std::move(sub_error)));
            }
            out.push_back(std::move(pair));
        }
        result = std::move(out);
        return true;
    }

    // --- Simulation control and interrogation ---

    if (m == "get_sim_time") {
        uint32_t high = 0;
        uint32_t low = 0;
        gpi_get_sim_time(&high, &low);
        IpcValue out = IpcValue::array();
        out.push_back(IpcValue::integer(high));
        out.push_back(IpcValue::integer(low));
        result = std::move(out);
        return true;
    }

    if (m == "get_precision") {
        int32_t precision = 0;
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
        int argc = 0;
        char const *const *argv = nullptr;
        gpi_get_simulator_args(&argc, &argv);
        IpcValue out = IpcValue::array();
        for (int i = 0; i < argc; ++i) {
            out.push_back(IpcValue::string(argv[i] ? argv[i] : ""));
        }
        result = std::move(out);
        return true;
    }

    if (m == "is_running") {
        result = IpcValue::boolean(gpi_has_registered_impl());
        return true;
    }

    if (m == "stop_simulator") {
        gpi_finish();
        result = IpcValue::null();
        return true;
    }

    // --- Hierarchy discovery ---

    if (m == "get_root_handle") {
        Args arg(a, error);
        std::string name;
        bool is_null = false;
        if (!arg.get_string_or_null(0, name, is_null)) {
            return false;
        }
        gpi_sim_hdl hdl =
            gpi_get_root_handle(is_null ? nullptr : name.c_str());
        result = hdl ? IpcValue::integer(static_cast<int64_t>(
                           add_handle(HandleKind::Obj, hdl)))
                     : IpcValue::null();
        return true;
    }

    if (m == "root_iterate") {
        gpi_iterator_hdl it = gpi_iterate(nullptr, GPI_ROOTS);
        result = it ? IpcValue::integer(static_cast<int64_t>(
                          add_handle(HandleKind::Iterator, it)))
                    : IpcValue::null();
        return true;
    }

    if (m == "package_iterate") {
        gpi_iterator_hdl it = gpi_iterate(nullptr, GPI_PACKAGE_SCOPES);
        result = it ? IpcValue::integer(static_cast<int64_t>(
                          add_handle(HandleKind::Iterator, it)))
                    : IpcValue::null();
        return true;
    }

    if (m == "iterate") {
        Args arg(a, error);
        void *base;
        int64_t type;
        if (!arg.get_handle(0, HandleKind::Obj, base) ||
            !arg.get_int(1, type)) {
            return false;
        }
        gpi_iterator_hdl it = gpi_iterate(
            static_cast<gpi_sim_hdl>(base),
            static_cast<gpi_iterator_sel>(type));
        // Like the legacy extension, a NULL iterator maps to Python None.
        result = it ? IpcValue::integer(static_cast<int64_t>(
                          add_handle(HandleKind::Iterator, it)))
                    : IpcValue::null();
        return true;
    }

    if (m == "iterator_next") {
        Args arg(a, error);
        void *it;
        if (!arg.get_handle(0, HandleKind::Iterator, it)) {
            return false;
        }
        gpi_sim_hdl next =
            gpi_next(static_cast<gpi_iterator_hdl>(it));
        // End of iteration: return null (the client raises StopIteration).
        result = next ? IpcValue::integer(static_cast<int64_t>(
                            add_handle(HandleKind::Obj, next)))
                      : IpcValue::null();
        return true;
    }

    if (m == "iterate_all") {
        // Drain an iterator in a single round trip: same semantics as
        // "iterate" followed by repeated "iterator_next", but returns every
        // child id as one array. Like "iterate", a missing iterator maps to
        // null (the client raises RuntimeError).
        Args arg(a, error);
        void *base;
        int64_t type;
        if (!arg.get_handle(0, HandleKind::Obj, base) ||
            !arg.get_int(1, type)) {
            return false;
        }
        gpi_iterator_hdl it = gpi_iterate(
            static_cast<gpi_sim_hdl>(base),
            static_cast<gpi_iterator_sel>(type));
        if (!it) {
            result = IpcValue::null();
            return true;
        }
        IpcValue out = IpcValue::array();
        while (gpi_sim_hdl next = gpi_next(it)) {
            out.push_back(IpcValue::integer(
                static_cast<int64_t>(add_handle(HandleKind::Obj, next))));
        }
        result = std::move(out);
        return true;
    }

    if (m == "get_handle_by_name") {
        Args arg(a, error);
        void *base;
        std::string name;
        int64_t discovery = 0;
        if (!arg.get_handle(0, HandleKind::Obj, base) ||
            !arg.get_string(1, name)) {
            return false;
        }
        if (a.size() > 2 && !a[2].is_null()) {
            if (!arg.get_int(2, discovery)) {
                return false;
            }
        }
        gpi_sim_hdl hdl = gpi_get_handle_by_name(
            static_cast<gpi_sim_hdl>(base), name.c_str(),
            static_cast<gpi_discovery>(discovery));
        result = hdl ? IpcValue::integer(static_cast<int64_t>(
                           add_handle(HandleKind::Obj, hdl)))
                     : IpcValue::null();
        return true;
    }

    if (m == "get_handle_by_index") {
        Args arg(a, error);
        void *base;
        int64_t index;
        if (!arg.get_handle(0, HandleKind::Obj, base) ||
            !arg.get_int(1, index)) {
            return false;
        }
        gpi_sim_hdl hdl = gpi_get_handle_by_index(
            static_cast<gpi_sim_hdl>(base), static_cast<int>(index));
        result = hdl ? IpcValue::integer(static_cast<int64_t>(
                           add_handle(HandleKind::Obj, hdl)))
                     : IpcValue::null();
        return true;
    }

    // --- Object properties ---

    if (m == "get_type") {
        Args arg(a, error);
        void *obj;
        if (!arg.get_handle(0, HandleKind::Obj, obj)) {
            return false;
        }
        result = IpcValue::integer(static_cast<int64_t>(
            gpi_get_object_type(static_cast<gpi_sim_hdl>(obj))));
        return true;
    }

    if (m == "get_type_string") {
        Args arg(a, error);
        void *obj;
        if (!arg.get_handle(0, HandleKind::Obj, obj)) {
            return false;
        }
        const char *s =
            gpi_get_signal_type_str(static_cast<gpi_sim_hdl>(obj));
        if (!s) {
            error = "Simulator yielded a null pointer instead of type string";
            return false;
        }
        result = IpcValue::string(s);
        return true;
    }

    if (m == "get_name_string") {
        Args arg(a, error);
        void *obj;
        if (!arg.get_handle(0, HandleKind::Obj, obj)) {
            return false;
        }
        const char *s =
            gpi_get_signal_name_str(static_cast<gpi_sim_hdl>(obj));
        if (!s) {
            error = "Simulator yielded a null pointer instead of name";
            return false;
        }
        result = IpcValue::string(s);
        return true;
    }

    if (m == "get_definition_name") {
        Args arg(a, error);
        void *obj;
        if (!arg.get_handle(0, HandleKind::Obj, obj)) {
            return false;
        }
        const char *s =
            gpi_get_definition_name(static_cast<gpi_sim_hdl>(obj));
        result = s ? IpcValue::string(s) : IpcValue::null();
        return true;
    }

    if (m == "get_definition_file") {
        Args arg(a, error);
        void *obj;
        if (!arg.get_handle(0, HandleKind::Obj, obj)) {
            return false;
        }
        const char *s =
            gpi_get_definition_file(static_cast<gpi_sim_hdl>(obj));
        result = s ? IpcValue::string(s) : IpcValue::null();
        return true;
    }

    if (m == "get_const") {
        Args arg(a, error);
        void *obj;
        if (!arg.get_handle(0, HandleKind::Obj, obj)) {
            return false;
        }
        result = IpcValue::boolean(
            gpi_is_constant(static_cast<gpi_sim_hdl>(obj)) != 0);
        return true;
    }

    if (m == "get_signed") {
        Args arg(a, error);
        void *obj;
        if (!arg.get_handle(0, HandleKind::Obj, obj)) {
            return false;
        }
        result = IpcValue::integer(
            gpi_is_signed(static_cast<gpi_sim_hdl>(obj)));
        return true;
    }

    if (m == "get_indexable") {
        Args arg(a, error);
        void *obj;
        if (!arg.get_handle(0, HandleKind::Obj, obj)) {
            return false;
        }
        result = IpcValue::boolean(
            gpi_is_indexable(static_cast<gpi_sim_hdl>(obj)) != 0);
        return true;
    }

    if (m == "get_num_elems") {
        Args arg(a, error);
        void *obj;
        if (!arg.get_handle(0, HandleKind::Obj, obj)) {
            return false;
        }
        result = IpcValue::integer(
            gpi_get_num_elems(static_cast<gpi_sim_hdl>(obj)));
        return true;
    }

    if (m == "get_range") {
        Args arg(a, error);
        void *obj;
        if (!arg.get_handle(0, HandleKind::Obj, obj)) {
            return false;
        }
        gpi_sim_hdl hdl = static_cast<gpi_sim_hdl>(obj);
        IpcValue out = IpcValue::array();
        out.push_back(IpcValue::integer(gpi_get_range_left(hdl)));
        out.push_back(IpcValue::integer(gpi_get_range_right(hdl)));
        out.push_back(
            IpcValue::integer(gpi_get_range_dir(hdl)));
        result = std::move(out);
        return true;
    }

    // --- Signal values ---

    if (m == "get_signal_val_binstr") {
        Args arg(a, error);
        void *obj;
        if (!arg.get_handle(0, HandleKind::Obj, obj)) {
            return false;
        }
        const char *s = gpi_get_signal_value_binstr(
            static_cast<gpi_sim_hdl>(obj));
        if (!s) {
            error = "Simulator yielded a null pointer instead of binstr";
            return false;
        }
        result = IpcValue::string(s);
        return true;
    }

    if (m == "get_signal_val_str") {
        Args arg(a, error);
        void *obj;
        if (!arg.get_handle(0, HandleKind::Obj, obj)) {
            return false;
        }
        const char *s =
            gpi_get_signal_value_str(static_cast<gpi_sim_hdl>(obj));
        if (!s) {
            error = "Simulator yielded a null pointer instead of string";
            return false;
        }
        // Legacy returned PyBytes for this method.
        result = IpcValue::bytes(s);
        return true;
    }

    if (m == "get_signal_val_real") {
        Args arg(a, error);
        void *obj;
        if (!arg.get_handle(0, HandleKind::Obj, obj)) {
            return false;
        }
        result = IpcValue::floating(gpi_get_signal_value_real(
            static_cast<gpi_sim_hdl>(obj)));
        return true;
    }

    if (m == "get_signal_val_long") {
        Args arg(a, error);
        void *obj;
        if (!arg.get_handle(0, HandleKind::Obj, obj)) {
            return false;
        }
        result = IpcValue::integer(gpi_get_signal_value_long(
            static_cast<gpi_sim_hdl>(obj)));
        return true;
    }

    if (m == "set_signal_val_binstr") {
        Args arg(a, error);
        void *obj;
        int64_t action = GPI_DEPOSIT;
        std::string value;
        if (!arg.get_handle(0, HandleKind::Obj, obj) ||
            !arg.get_int(1, action) || !arg.get_string(2, value)) {
            return false;
        }
        // Like the legacy extension, failures are reported by the GPI
        // (which logs them) and not raised here.
        gpi_set_signal_value_binstr(static_cast<gpi_sim_hdl>(obj),
                                    value.c_str(),
                                    static_cast<gpi_set_action>(action));
        result = IpcValue::null();
        return true;
    }

    if (m == "set_signal_val_str") {
        Args arg(a, error);
        void *obj;
        int64_t action = GPI_DEPOSIT;
        if (!arg.get_handle(0, HandleKind::Obj, obj) ||
            !arg.get_int(1, action)) {
            return false;
        }
        // Legacy accepted bytes only; be lenient and accept str too.
        if (a.size() < 3 || (!a[2].is_string() && !a[2].is_bytes())) {
            error = "Expected a bytes or str argument";
            return false;
        }
        const std::string &value = a[2].is_string() ? a[2].get_string()
                                                    : a[2].get_bytes();
        gpi_set_signal_value_str(static_cast<gpi_sim_hdl>(obj), value.c_str(),
                                 static_cast<gpi_set_action>(action));
        result = IpcValue::null();
        return true;
    }

    if (m == "set_signal_val_real") {
        Args arg(a, error);
        void *obj;
        int64_t action = GPI_DEPOSIT;
        double value;
        if (!arg.get_handle(0, HandleKind::Obj, obj) ||
            !arg.get_int(1, action) || !arg.get_double(2, value)) {
            return false;
        }
        gpi_set_signal_value_real(static_cast<gpi_sim_hdl>(obj), value,
                                  static_cast<gpi_set_action>(action));
        result = IpcValue::null();
        return true;
    }

    if (m == "set_signal_val_int") {
        Args arg(a, error);
        void *obj;
        int64_t action = GPI_DEPOSIT;
        int64_t value;
        if (!arg.get_handle(0, HandleKind::Obj, obj) ||
            !arg.get_int(1, action) || !arg.get_int(2, value)) {
            return false;
        }
        gpi_set_signal_value_int(static_cast<gpi_sim_hdl>(obj),
                                 static_cast<int32_t>(value),
                                 static_cast<gpi_set_action>(action));
        result = IpcValue::null();
        return true;
    }

    // --- Callback registration ---

    if (m == "register_readonly_callback" || m == "register_rwsynch_callback" ||
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
        if (!hdl) {
            delete data;
            error = "Failed to register callback";
            return false;
        }
        result = IpcValue::integer(
            static_cast<int64_t>(add_handle(HandleKind::Callback, hdl)));
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
        if (!hdl) {
            delete data;
            error = "Failed to register callback";
            return false;
        }
        result = IpcValue::integer(
            static_cast<int64_t>(add_handle(HandleKind::Callback, hdl)));
        return true;
    }

    if (m == "register_value_change_callback") {
        Args arg(a, error);
        uint64_t sig_id;
        int64_t edge;
        uint64_t cb_id;
        if (!arg.get_uint64(0, sig_id) || !arg.get_int(1, edge) ||
            !arg.get_uint64(2, cb_id)) {
            return false;
        }
        gpi_sim_hdl sig_hdl =
            lookup_handle<gpi_sim_hdl>(HandleKind::Obj, sig_id);
        if (!sig_hdl) {
            error = "Invalid signal handle";
            return false;
        }
        IpcCallbackData *data = new IpcCallbackData{cb_id};
        gpi_cb_hdl hdl = gpi_register_value_change_callback(
            ipc_cb_handler, data, sig_hdl, static_cast<gpi_edge>(edge));
        if (!hdl) {
            delete data;
            error = "Failed to register callback";
            return false;
        }
        result = IpcValue::integer(
            static_cast<int64_t>(add_handle(HandleKind::Callback, hdl)));
        return true;
    }

    if (m == "deregister_callback") {
        Args arg(a, error);
        uint64_t id;
        if (!arg.get_uint64(0, id)) {
            return false;
        }
        gpi_cb_hdl hdl =
            lookup_handle<gpi_cb_hdl>(HandleKind::Callback, id);
        if (!hdl) {
            error = "Invalid or stale handle";
            return false;
        }
        // Callbacks are one-shot: deregistration only happens before the
        // callback fires (the GPI frees fired callbacks itself), so the user
        // data is still owned by the pending callback here.
        void *cb_data = nullptr;
        gpi_get_cb_info(hdl, nullptr, &cb_data);
        delete static_cast<IpcCallbackData *>(cb_data);
        gpi_remove_cb(hdl);
        remove_handle(id);
        result = IpcValue::null();
        return true;
    }

    // --- Clocks ---

    if (m == "clock_create") {
        Args arg(a, error);
        void *obj;
        if (!arg.get_handle(0, HandleKind::Obj, obj)) {
            return false;
        }
        GpiClock *clk = new GpiClock(static_cast<GpiObjHdl *>(obj));
        result = IpcValue::integer(
            static_cast<int64_t>(add_handle(HandleKind::Clock, clk)));
        return true;
    }

    if (m == "delete_clock") {
        Args arg(a, error);
        uint64_t id;
        if (!arg.get_uint64(0, id)) {
            return false;
        }
        GpiClock *clk =
            lookup_handle<GpiClock *>(HandleKind::Clock, id);
        if (!clk) {
            error = "Invalid or stale handle";
            return false;
        }
        remove_handle(id);
        delete clk;
        result = IpcValue::null();
        return true;
    }

    if (m == "clock_start") {
        Args arg(a, error);
        void *clk;
        uint64_t period;
        uint64_t high;
        bool start_high;
        int64_t set_action;
        if (!arg.get_handle(0, HandleKind::Clock, clk) ||
            !arg.get_uint64(1, period) || !arg.get_uint64(2, high) ||
            !arg.get_bool(3, start_high) || !arg.get_int(4, set_action)) {
            return false;
        }
        int rv = static_cast<GpiClock *>(clk)->start(
            period, high, start_high,
            static_cast<gpi_set_action>(set_action));
        // Error strings match the legacy extension so client-side exception
        // mapping behaves identically.
        if (rv == EINVAL) {
            error = "Failed to start clock: invalid arguments!\n";
            return false;
        }
        if (rv == EBUSY) {
            error = "Failed to start clock: already started!\n";
            return false;
        }
        if (rv != 0) {
            error = "Failed to start clock!\n";
            return false;
        }
        result = IpcValue::null();
        return true;
    }

    if (m == "clock_stop") {
        Args arg(a, error);
        void *clk;
        if (!arg.get_handle(0, HandleKind::Clock, clk)) {
            return false;
        }
        static_cast<GpiClock *>(clk)->stop();
        result = IpcValue::null();
        return true;
    }

    // --- Logging and simulator events ---

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
        ipc_logging_configure();
        result = IpcValue::null();
        return true;
    }

    if (m == "set_sim_event_callback") {
        // The callback lives entirely on the Python side; there is nothing to
        // register server-side beyond accepting the request (parity with the
        // legacy extension, which stored a PyObject here).
        result = IpcValue::null();
        return true;
    }

    error = "Unknown method: " + m;
    return false;
}

}  // namespace

bool ipc_dispatch_request(const IpcValue &msg, IpcValue &result,
                          std::string &error) {
    const IpcValue *method = msg.get("method");
    const IpcValue *args = msg.get("args");
    if (!method || !method->is_string() || !args || !args->is_array()) {
        error = "Malformed request";
        return false;
    }
    return dispatch_one(method->get_string(), args->array_ref(), result,
                        error);
}

}  // namespace ipc
}  // namespace cocotb
