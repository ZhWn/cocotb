// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

#include "./ipc_priv.hpp"
#include "./ipc_base.hpp"

#include <gpi.h>

#include <cstdarg>  // va_list, va_copy, va_end
#include <cstdio>   // fprintf, vsnprintf
#include <cstring>
#include <map>  // std::map
#include <string>
#include <vector>

#include "../utils.hpp"  // DEFER
#include "./json.hpp"

static int ipc_log_level = GPI_NOTSET;

// Becomes true once the Python side has registered its logging callbacks via
// the `initialize_logger` request. Until then, and whenever no transport is
// available, log messages are forwarded to the previously installed handler.
static int ipc_logging_configured = 0;

static gpi_log_handler_ftype fallback_log_handler = nullptr;
static void *fallback_log_userdata = nullptr;

static cocotb::ipc::IpcTransport *ipc_transport = nullptr;

static int ipc_log_send_failed = 0;

void ipc_set_transport(cocotb::ipc::IpcTransport *transport) {
    ipc_transport = transport;
    ipc_log_send_failed = 0;
}

void ipc_logging_configure() { ipc_logging_configured = 1; }

static void ipc_send_log(const char *name, enum gpi_log_level level,
                         const char *pathname, const char *funcname,
                         long lineno, const char *msg, va_list argp) {
    // Format the message with vsnprintf like the other native loggers do.
    static std::vector<char> log_buff(512);

    log_buff.clear();
    int n = vsnprintf(log_buff.data(), log_buff.capacity(), msg, argp);
    if (n < 0) {
        // LCOV_EXCL_START
        return;
        // LCOV_EXCL_STOP
    }
    if ((unsigned)n >= log_buff.capacity()) {
        log_buff.reserve((unsigned)n + 1);
        n = vsnprintf(log_buff.data(), (unsigned)n + 1, msg, argp);
        if (n < 0) {
            // LCOV_EXCL_START
            return;
            // LCOV_EXCL_STOP
        }
    }

    using cocotb::ipc::JsonValue;
    JsonValue log_msg = JsonValue::object();
    log_msg.set("type", JsonValue::string("log"));
    log_msg.set("logger", JsonValue::string(name));
    log_msg.set("level", JsonValue::integer(level));
    log_msg.set("filename", JsonValue::string(pathname));
    log_msg.set("lineno", JsonValue::integer(lineno));
    log_msg.set("msg", JsonValue::string(std::string(log_buff.data())));
    log_msg.set("function", JsonValue::string(funcname));

    const std::string serialized = cocotb::ipc::serialize(log_msg);
    if (!ipc_transport->send(serialized.data(), serialized.size()) ||
        !ipc_transport->send("\n", 1)) {
        if (!ipc_log_send_failed) {
            ipc_log_send_failed = 1;
            fprintf(stderr,
                    "IPC: failed to send log message to Python process\n");
        }
    }
}

static void ipc_log_handler(void *, const char *name,
                            enum gpi_log_level level, const char *pathname,
                            const char *funcname, long lineno, const char *msg,
                            va_list argp) {
    // Always pass through messages when NOTSET to let Python make the
    // decision. Otherwise, skip logs using the local log level for better
    // performance.
    if (ipc_log_level != GPI_NOTSET && level < ipc_log_level) {
        return;
    }

    if (!ipc_logging_configured || !ipc_transport) {
        if (fallback_log_handler) {
            fallback_log_handler(fallback_log_userdata, name, level, pathname,
                                 funcname, lineno, msg, argp);
        }
        return;
    }

    va_list argp_copy;
    va_copy(argp_copy, argp);
    DEFER(va_end(argp_copy));
    ipc_send_log(name, level, pathname, funcname, lineno, msg, argp_copy);
}

void ipc_log(enum gpi_log_level level, const char *pathname,
             const char *funcname, long lineno, const char *fmt, ...) {
    va_list argp;
    va_start(argp, fmt);
    DEFER(va_end(argp));
    ipc_log_handler(nullptr, "ipc", level, pathname, funcname, lineno, fmt,
                    argp);
}

void ipc_logging_set_level(enum gpi_log_level level) {
    ipc_log_level = level;
    gpi_native_logger_set_level(level);
}

void ipc_logging_initialize() {
    // Default to using the fallback handler until configured.
    gpi_get_log_handler(&fallback_log_handler, &fallback_log_userdata);
    gpi_set_log_handler(ipc_log_handler, nullptr);
}

void ipc_logging_finalize() {
    if (fallback_log_handler) {
        gpi_set_log_handler(fallback_log_handler, fallback_log_userdata);
    }
    ipc_logging_configured = 0;
}

// Disabled by default
int ipc_debug_enabled = 0;
