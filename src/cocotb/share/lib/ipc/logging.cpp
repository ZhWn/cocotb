// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Logging bridge: routes GPI log messages to the Python child process over
// the IPC connection.
//
// Lifecycle mirrors the legacy pygpi logging module:
//  - `ipc_logging_initialize()` installs the handler; until Python
//    configures logging, messages go to the native (fallback) handler so
//    nothing is lost while the child is starting up.
//  - `ipc_logging_configure()` is called when the Python side registers its
//    logging callbacks; from then on messages are forwarded as `log`
//    messages.
//  - `ipc_logging_finalize()` restores the native handler.
//
// Failures while forwarding (connection lost) fall back to the native
// handler so logs are never silently dropped.

#include "./ipc_priv.hpp"

#include <gpi.h>

#include <cstdarg>  // va_list, va_copy, va_end
#include <cstdio>   // fprintf, vsnprintf
#include <string>
#include <vector>

#include "../utils.hpp"  // DEFER

namespace cocotb {
namespace ipc {

namespace {

enum gpi_log_level ipc_log_level = GPI_NOTSET;

// Set once Python registers its logging callbacks.
bool ipc_log_configured = false;

gpi_log_handler_ftype fallback_log_handler = nullptr;
void *fallback_log_userdata = nullptr;

// Guards against re-entering the handler from code paths that log (for
// example a failed send triggering an error log).
thread_local bool in_handler = false;

void format_message(const char *msg, va_list argp, va_list argp_copy,
                    std::vector<char> &log_buff) {
    log_buff.clear();
    int n = vsnprintf(log_buff.data(), log_buff.capacity(), msg, argp);
    if (n < 0) {
        // Windows CRT prior to 2015 returns -1 on overflow instead of the
        // required size; retry with a NULL buffer to get the length.
        va_list argp_again;
        va_copy(argp_again, argp_copy);
        DEFER(va_end(argp_again));
        n = vsnprintf(nullptr, 0, msg, argp_again);
        if (n < 0) {
            // LCOV_EXCL_START
            fprintf(stderr, "Log message construction failed: (error code) %d\n",
                    n);
            return;
            // LCOV_EXCL_STOP
        }
    }
    if (static_cast<unsigned>(n) >= log_buff.capacity()) {
        log_buff.reserve(static_cast<unsigned>(n) + 1);
        n = vsnprintf(log_buff.data(), static_cast<unsigned>(n) + 1, msg,
                      argp_copy);
        if (n < 0) {
            // LCOV_EXCL_START
            fprintf(stderr, "Log message construction failed: (error code) %d\n",
                    n);
            return;
            // LCOV_EXCL_STOP
        }
    }
}

}  // namespace

void ipc_log_handler(void *, const char *name, enum gpi_log_level level,
                     const char *pathname, const char *funcname, long lineno,
                     const char *msg, va_list argp) {
    // Always pass through messages when NOTSET to let Python make the
    // decision. Otherwise, skip logs below the local level for performance.
    if (ipc_log_level != GPI_NOTSET && level < ipc_log_level) {
        return;
    }

    // Before Python configures logging (and if re-entered while logging
    // itself), use the native handler so nothing is lost.
    if (!ipc_log_configured || in_handler) {
        return fallback_log_handler(fallback_log_userdata, name, level,
                                    pathname, funcname, lineno, msg, argp);
    }

    va_list argp_copy;
    va_copy(argp_copy, argp);
    DEFER(va_end(argp_copy));

    static std::vector<char> log_buff(512);
    format_message(msg, argp, argp_copy, log_buff);

    in_handler = true;
    DEFER(in_handler = false);

    IpcValue log = IpcValue::object();
    log.set("type", IpcValue::string("log"));
    log.set("level", IpcValue::integer(static_cast<int64_t>(level)));
    log.set("logger", IpcValue::string(name ? name : ""));
    log.set("filename", IpcValue::string(pathname ? pathname : ""));
    log.set("lineno", IpcValue::integer(lineno));
    log.set("msg", IpcValue::string(log_buff.data()));
    log.set("function", IpcValue::string(funcname ? funcname : ""));

    if (ipc_send_message(log)) {
        return;
    }

    // Forwarding failed (connection lost): fall back to the native handler
    // so the message reaches the console instead of vanishing.
    fallback_log_handler(fallback_log_userdata, name, level, pathname,
                         funcname, lineno, msg, argp_copy);
}

void ipc_logging_initialize() {
    // Default to the native handler until Python configures logging.
    gpi_get_log_handler(&fallback_log_handler, &fallback_log_userdata);
    gpi_set_log_handler(ipc_log_handler, nullptr);
    ipc_log_configured = false;
}

void ipc_logging_configure() { ipc_log_configured = true; }

void ipc_logging_set_level(enum gpi_log_level level) {
    ipc_log_level = level;
    gpi_native_logger_set_level(level);
}

void ipc_logging_finalize() {
    if (fallback_log_handler) {
        gpi_set_log_handler(fallback_log_handler, fallback_log_userdata);
    }
    ipc_log_configured = false;
}

}  // namespace ipc
}  // namespace cocotb
