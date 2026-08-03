// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Internal declarations shared by the cocotb IPC layer.
//
// The IPC layer replaces the Python C API bindings of the legacy PyGPI
// library: the simulator process runs a small IPC server that exposes the GPI
// to a Python child process which implements `cocotb.simulator` on top of it.

#ifndef COCOTB_IPC_PRIV_HPP_
#define COCOTB_IPC_PRIV_HPP_

#include <exports.h>
#include <gpi.h>

#include <cstdint>  // uint64_t
#include <string>

#include "./ipc_value.hpp"
#include "./ipc_protocol.hpp"

#ifdef IPC_EXPORTS
#define IPC_EXPORT COCOTB_EXPORT
#else
#define IPC_EXPORT COCOTB_IMPORT
#endif

namespace cocotb {
namespace ipc {

class IpcTransport;

// Handle table mapping integer identifiers handed to Python to raw GPI
// handles. Defined in dispatcher.cpp.
enum class HandleKind { Obj, Iterator, Callback, Clock };

// Entry in the handle table: the raw GPI handle plus its kind.
struct HandleEntry {
    HandleKind kind;
    void *ptr;
};

// Callback user data: identifies the Python-side callback to invoke.
struct IpcCallbackData {
    uint64_t cb_id;
};

uint64_t add_handle(HandleKind kind, void *ptr);
void *get_handle(HandleKind kind, uint64_t id);
void remove_handle(uint64_t id);

// Request dispatch. Defined in dispatcher.cpp.
bool dispatch_request(const IpcValue &msg, IpcValue &result,
                      std::string &error);

// GPI callback invoked when a simulator callback fires; notifies the Python
// process and waits for the callback acknowledgement. Defined in embed.cpp.
int ipc_cb_handler(void *user_data);

// Serializes and sends a message over the current transport using the
// selected codec. Defined in embed.cpp.
bool send_msg(const IpcValue &msg);

}  // namespace ipc
}  // namespace cocotb

void ipc_logging_initialize();
void ipc_logging_finalize();
void ipc_logging_configure();
void ipc_logging_set_level(enum gpi_log_level level);
void ipc_set_transport(cocotb::ipc::IpcTransport *transport);

extern int ipc_debug_enabled;

void ipc_log(enum gpi_log_level level, const char *pathname,
             const char *funcname, long lineno, const char *fmt, ...);

#define IPC_LOG_(level, ...) \
    ipc_log(level, __FILE__, __func__, __LINE__, __VA_ARGS__)

/** Logs a message at TRACE log level if IPC tracing is enabled */
#define IPC_LOG_TRACE(...)                  \
    do {                                    \
        if (ipc_debug_enabled) {            \
            IPC_LOG_(GPI_TRACE, __VA_ARGS__); \
        }                                   \
    } while (0)

/** Logs a message at DEBUG log level using the current log handler. */
#define IPC_LOG_DEBUG(...) IPC_LOG_(GPI_DEBUG, __VA_ARGS__)

/** Logs a message at INFO log level using the current log handler. */
#define IPC_LOG_INFO(...) IPC_LOG_(GPI_INFO, __VA_ARGS__)

/** Logs a message at WARN log level using the current log handler. */
#define IPC_LOG_WARN(...) IPC_LOG_(GPI_WARNING, __VA_ARGS__)

/** Logs a message at ERROR log level using the current log handler. */
#define IPC_LOG_ERROR(...) IPC_LOG_(GPI_ERROR, __VA_ARGS__)

/** Logs a message at CRITICAL log level using the current log handler. */
#define IPC_LOG_CRITICAL(...) IPC_LOG_(GPI_CRITICAL, __VA_ARGS__)

#endif /* COCOTB_IPC_PRIV_HPP_ */
