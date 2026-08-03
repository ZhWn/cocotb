// Copyright cocotb contributors
// Copyright (c) 2013, 2018 Potential Ventures Ltd
// Copyright (c) 2013 SolarFlare Communications Inc
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Start an IPC server inside the simulator process and spawn a Python child
// process implementing `cocotb.simulator` over it.
//
// The GPI_USERS entry point `initialize` is loaded by the GPI layer after the
// simulator interface library has been registered. It:
//   1. starts the IPC transport (a loopback TCP server),
//   2. spawns the Python child process (`PYGPI_PYTHON_BIN -m cocotb.ipc`),
//   3. registers the GPI callbacks that drive the child process, and
//   4. serves requests from the child process reentrantly while waiting for
//      callback acknowledgements.

#include "./ipc_base.hpp"
#include "./ipc_priv.hpp"

#include <gpi.h>

#include <cerrno>  // errno, ERANGE
#include <cstdint>  // uint64_t
#include <cstdio>
#include <cstdlib>  // getenv, strtoul
#include <cstring>
#include <string>
#include <utility>  // move
#include <vector>

#include "../utils.hpp"  // DEFER

#if defined(_WIN32)
#include <windows.h>
#include <process.h>  // _getpid
#else
#include <unistd.h>  // fork, execl, getpid, sleep
#endif

using cocotb::ipc::IpcTransport;
using cocotb::ipc::IpcValue;

static IpcTransport *ipc_transport = nullptr;

static uint64_t next_msg_id = 1;

static bool initialized = false;

bool cocotb::ipc::send_msg(const IpcValue &msg) {
    return cocotb::ipc::send_value(*ipc_transport, cocotb::ipc::g_ipc_protocol,
                                   msg, 0);
}

// Sends a `callback` message and processes incoming requests reentrantly
// until the matching `callback_ack` arrives. Returns 0 on success.
static int wait_for_ack(uint64_t expected_id, int *result) {
    while (true) {
        IpcValue msg;
        if (!cocotb::ipc::recv_value(*ipc_transport,
                                     cocotb::ipc::g_ipc_protocol, msg, 0)) {
            IPC_LOG_ERROR("IPC: connection to Python process lost");
            return -1;
        }

        const IpcValue *type = msg.get("type");
        if (!type || !type->is_string()) {
            continue;
        }
        const std::string &type_str = type->get_string();

        if (type_str == "callback_ack") {
            const IpcValue *id = msg.get("id");
            if (id && id->is_int() &&
                id->get_int() == static_cast<int64_t>(expected_id)) {
                const IpcValue *res = msg.get("result");
                *result = (res && res->is_int())
                              ? static_cast<int>(res->get_int())
                              : -1;
                return 0;
            }
            continue;
        }

        if (type_str == "request") {
            IpcValue request_result;
            std::string error;
            bool ok = cocotb::ipc::dispatch_request(msg, request_result, error);
            IpcValue response =
                cocotb::ipc::make_result(ok, std::move(request_result), error,
                                         cocotb::ipc::request_id(msg));
            if (!cocotb::ipc::send_msg(response)) {
                return -1;
            }
            continue;
        }

        IPC_LOG_WARN("IPC: unexpected message type '%s'", type_str.c_str());
    }
}

/*******************************************************************************
 * GPI callbacks driving the Python child process
 *******************************************************************************/

int cocotb::ipc::ipc_cb_handler(void *user_data) {
    IPC_LOG_TRACE("GPI => [ IPC (callback) ]");
    DEFER(IPC_LOG_TRACE("[ IPC (callback) ] => GPI"));

    cocotb::ipc::IpcCallbackData *data =
        static_cast<cocotb::ipc::IpcCallbackData *>(user_data);
    DEFER(delete data);
    IpcValue msg = IpcValue::object();
    msg.set("type", IpcValue::string("callback"));
    msg.set("func", IpcValue::string("gpi"));
    msg.set("id", IpcValue::integer(static_cast<int64_t>(next_msg_id++)));
    msg.set("cb_id", IpcValue::integer(static_cast<int64_t>(data->cb_id)));
    if (!cocotb::ipc::send_msg(msg)) {
        return -1;
    }

    int result = -1;
    if (wait_for_ack(next_msg_id - 1, &result) != 0) {
        return -1;
    }
    return result;
}

static int start_of_sim_time(void *) {
    IPC_LOG_INFO("IPC: start_of_sim_time callback");
    IPC_LOG_TRACE("GPI Start Sim => [ IPC Start ]");
    DEFER(IPC_LOG_TRACE("[ IPC Start ] => GPI Start Sim"));

    if (initialized) {
        IPC_LOG_ERROR("IPC library initialized again!");
        return -1;
    }
    initialized = true;

    IpcValue msg = IpcValue::object();
    msg.set("type", IpcValue::string("callback"));
    msg.set("func", IpcValue::string("start_of_sim_time"));
    msg.set("id", IpcValue::integer(static_cast<int64_t>(next_msg_id++)));
    if (!cocotb::ipc::send_msg(msg)) {
        return -1;
    }

    int result = -1;
    if (wait_for_ack(next_msg_id - 1, &result) != 0) {
        return -1;
    }
    return result;
}

static void end_of_sim_time(void *) {
    IPC_LOG_INFO("IPC: end_of_sim_time callback");
    IPC_LOG_TRACE("GPI End Sim => [ IPC End ]");
    DEFER(IPC_LOG_TRACE("[ IPC End ] => GPI End Sim"));

    if (!ipc_transport) {
        return;
    }

    IpcValue msg = IpcValue::object();
    msg.set("type", IpcValue::string("callback"));
    msg.set("func", IpcValue::string("end_of_sim_time"));
    msg.set("id", IpcValue::integer(static_cast<int64_t>(next_msg_id++)));
    if (!cocotb::ipc::send_msg(msg)) {
        return;
    }

    int result = 0;
    wait_for_ack(next_msg_id - 1, &result);
}

static void finalize(void *) {
    IPC_LOG_INFO("IPC: finalize callback");
    IPC_LOG_TRACE("GPI Finalize => [ IPC Finalize ]");
    DEFER(IPC_LOG_TRACE("[ IPC Finalize ] => GPI Finalize"));

    if (ipc_transport) {
        IpcValue msg = IpcValue::object();
        msg.set("type", IpcValue::string("callback"));
        msg.set("func", IpcValue::string("finalize"));
        msg.set("id", IpcValue::integer(static_cast<int64_t>(next_msg_id++)));
        if (cocotb::ipc::send_msg(msg)) {
            int result = 0;
            wait_for_ack(next_msg_id - 1, &result);
        }

        ipc_transport->close();
        delete ipc_transport;
        ipc_transport = nullptr;
    }

    ipc_logging_finalize();
}

/*******************************************************************************
 * Python child process management
 *******************************************************************************/

static int spawn_python_child(const std::string &endpoint) {
    const char *python_bin = getenv("PYGPI_PYTHON_BIN");
    if (!python_bin) {
        IPC_LOG_ERROR(
            "PYGPI_PYTHON_BIN variable not set. Can't start the Python "
            "process!");
        return -1;
    }

    IPC_LOG_INFO("Starting Python interpreter %s -m cocotb.ipc %s",
                 python_bin, endpoint.c_str());

#ifdef _WIN32
    std::string cmdline = "\"";
    cmdline += python_bin;
    cmdline += "\" -m cocotb.ipc ";
    cmdline += endpoint;

    std::vector<char> mutable_cmdline(cmdline.begin(), cmdline.end());
    mutable_cmdline.push_back('\0');

    // Redirect the child's standard handles to NUL. Without STARTF_USESTDHANDLES
    // the child inherits the simulator's std handles (even with bInheritHandles
    // set to FALSE), keeping the simulator's stdout/stderr file (e.g. the
    // runner's log file) locked until the child process exits, which races
    // log-file cleanup on Windows.
    SECURITY_ATTRIBUTES sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE nul_handle = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                    OPEN_EXISTING, 0, nullptr);
    HANDLE out_handle = nul_handle;
    // When debugging, redirect the child's stdout/stderr to a file so that
    // Python tracebacks are visible (the child's std handles are otherwise
    // on NUL).
    const char *debug_file = getenv("COCOTB_IPC_SPAWN_DEBUG");
    if (debug_file && *debug_file) {
        HANDLE fh = CreateFileA(debug_file, GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                OPEN_ALWAYS, 0, nullptr);
        if (fh != INVALID_HANDLE_VALUE) {
            SetFilePointer(fh, 0, nullptr, FILE_END);
        }
        out_handle = fh;
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    std::memset(&si, 0, sizeof(si));
    std::memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul_handle;
    si.hStdOutput = out_handle;
    si.hStdError = out_handle;

    if (!CreateProcessA(nullptr, mutable_cmdline.data(), nullptr, nullptr,
                        TRUE, 0, nullptr, nullptr, &si, &pi)) {
        if (out_handle != nul_handle) CloseHandle(out_handle);
        CloseHandle(nul_handle);
        IPC_LOG_ERROR("Failed to start Python process: %lu",
                      static_cast<unsigned long>(GetLastError()));
        return -1;
    }
    if (out_handle != nul_handle) CloseHandle(out_handle);
    CloseHandle(nul_handle);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
#else
    pid_t pid = fork();
    if (pid < 0) {
        IPC_LOG_ERROR("Failed to fork Python process");
        return -1;
    }
    if (pid == 0) {
        execl(python_bin, python_bin, "-m", "cocotb.ipc",
              endpoint.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    return 0;
#endif
}

static void ipc_init_debug() {
    char *debug_env = getenv("IPC_DEBUG");
    if (debug_env) {
        std::string ipc_debug = debug_env;
        // If it's explicitly set to 0, don't enable
        if (ipc_debug != "0") {
            ipc_debug_enabled = 1;
        }
    }
}

/*******************************************************************************
 * GPI_USERS entry point
 *******************************************************************************/

extern "C" IPC_EXPORT void initialize(void) {
    ipc_init_debug();
    ipc_logging_initialize();
    cocotb::ipc::resolve_protocol();

    IPC_LOG_TRACE("GPI Init => [ IPC Init ]");
    DEFER(IPC_LOG_TRACE("[ IPC Init ] => GPI Init"));

    // Before starting anything we check if the user wants to pause the
    // simulator thread such that they can attach a debugger.
    const char *pause = getenv("COCOTB_ATTACH");
    if (pause) {
        unsigned long sleep_time = strtoul(pause, NULL, 10);
        if (errno == ERANGE || sleep_time >= UINT_MAX) {
            IPC_LOG_ERROR("COCOTB_ATTACH only needs to be set to ~30 seconds");
            return;
        }
        if ((errno != 0 && sleep_time == 0) || (sleep_time <= 0)) {
            IPC_LOG_ERROR(
                "COCOTB_ATTACH must be set to an integer base 10 or omitted");
            return;
        }

        IPC_LOG_INFO("Waiting for %lu seconds - attach to PID %d with your "
                     "debugger",
                     sleep_time,
#if defined(_WIN32)
                     static_cast<int>(_getpid()));
#else
                     static_cast<int>(getpid()));
#endif
#if defined(_WIN32)
        Sleep(static_cast<DWORD>(sleep_time) * 1000);
#else
        sleep(sleep_time);
#endif
    }

    ipc_transport = IpcTransport::create();
    if (!ipc_transport || !ipc_transport->open()) {
        IPC_LOG_ERROR("Failed to start IPC transport");
        delete ipc_transport;
        ipc_transport = nullptr;
        return;
    }

    ipc_set_transport(ipc_transport);

    if (spawn_python_child(ipc_transport->get_endpoint())) {
        ipc_transport->close();
        delete ipc_transport;
        ipc_transport = nullptr;
        return;
    }

    // Wait for the Python process to connect. A generous timeout is used so
    // that a failure to start the child process doesn't hang the simulator.
    IPC_LOG_INFO("Waiting for Python process to connect on %s",
                 ipc_transport->get_endpoint().c_str());
    if (!ipc_transport->wait_for_client(60000)) {
        IPC_LOG_ERROR("Timed out waiting for the Python process to connect");
        ipc_transport->close();
        delete ipc_transport;
        ipc_transport = nullptr;
        return;
    }

    gpi_register_start_of_sim_time_callback(start_of_sim_time, nullptr);
    gpi_register_end_of_sim_time_callback(end_of_sim_time, nullptr);
    gpi_register_finalize_callback(finalize, nullptr);
}
