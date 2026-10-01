// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Internal declarations shared by the IPC server implementation files.

#ifndef COCOTB_IPC_PRIV_HPP_
#define COCOTB_IPC_PRIV_HPP_

#include "./ipc_base.hpp"
#include "./ipc_protocol.hpp"
#include "./ipc_value.hpp"

#include "../gpi/logging.hpp"  // gpi_log, LOG_* macros

#include <exports.h>
#include <gpi.h>

#include <cstdint>  // uint64_t
#include <string>
#include <vector>

#ifdef IPC_EXPORTS
#define IPC_EXPORT COCOTB_EXPORT
#else
#define IPC_EXPORT COCOTB_IMPORT
#endif

namespace cocotb {
namespace ipc {

/** Protocol version exchanged in the hello/ready handshake. */
constexpr uint64_t kProtocolVersion = 1;

/** Timeout waiting for the Python child process to spawn and connect. */
constexpr long kSpawnTimeoutMs = 30000;

/** Timeout waiting for the Python child to acknowledge shutdown. */
constexpr long kShutdownTimeoutMs = 5000;

extern bool ipc_debug_enabled;

/** Logs a message at TRACE log level if IPC debugging is enabled. */
#define IPC_LOG_TRACE(...)                    \
    do {                                      \
        if (cocotb::ipc::ipc_debug_enabled) { \
            gpi_log("ipc", GPI_TRACE, __FILE__, __func__, __LINE__, \
                    __VA_ARGS__);             \
        }                                     \
    } while (0)

#define IPC_LOG_DEBUG(...) \
    gpi_log("ipc", GPI_DEBUG, __FILE__, __func__, __LINE__, __VA_ARGS__)
#define IPC_LOG_INFO(...) \
    gpi_log("ipc", GPI_INFO, __FILE__, __func__, __LINE__, __VA_ARGS__)
#define IPC_LOG_WARN(...) \
    gpi_log("ipc", GPI_WARNING, __FILE__, __func__, __LINE__, __VA_ARGS__)
#define IPC_LOG_ERROR(...) \
    gpi_log("ipc", GPI_ERROR, __FILE__, __func__, __LINE__, __VA_ARGS__)

/*******************************************************************************
 * Connection state (owned by embed.cpp)
 *******************************************************************************/

/** The active transport, or nullptr when the connection is not up. */
extern IpcTransport *g_transport;

/** True once the handshake completed and the connection is usable. */
extern bool g_connection_alive;

/**
 * Encode and send one message.
 *
 * Returns false when there is no connection or the send failed; a failed
 * send marks the connection lost so later calls fail fast. Safe to call
 * from the logging path: it never logs on failure (the logging path is
 * itself a caller and must not recurse).
 */
bool ipc_send_message(const IpcValue &msg);

/** Mark the connection as lost, logging a diagnostic exactly once. */
void ipc_mark_connection_lost(const char *reason);

/**
 * Assign a message id to `msg`, send it, and serve requests from Python
 * reentrantly until the matching `callback_ack` arrives. Returns the value
 * Python returned, or -1 if the connection is lost.
 *
 * `timeout_ms` bounds the wait (used during shutdown so a wedged child
 * cannot hang the simulator); negative waits forever.
 */
int send_callback_and_wait(IpcValue &msg, long timeout_ms = -1);

/*******************************************************************************
 * Request dispatch (implemented by dispatcher.cpp)
 *******************************************************************************/

/** User data attached to every GPI callback registered for Python. */
struct IpcCallbackData {
    uint64_t cb_id;
};

/**
 * GPI callback trampoline: forwards a fired callback to the Python process
 * and blocks until Python acknowledges. Returns the value Python returned,
 * or -1 if the connection is lost.
 */
int ipc_cb_handler(void *user_data);

/**
 * Dispatch one request message (of type "request") against the GPI.
 * Returns true on success; on failure returns false and fills `error`.
 * Also implements the "batch" method, which runs a list of requests in one
 * round trip.
 */
bool ipc_dispatch_request(const IpcValue &msg, IpcValue &result,
                          std::string &error);

/*******************************************************************************
 * Logging (implemented by logging.cpp)
 *******************************************************************************/

/** Install the IPC log handler; logs fall back to the native handler until
 * the Python process configures logging. */
void ipc_logging_initialize();

/** The Python process registered its log callbacks: forward from now on. */
void ipc_logging_configure();

/** Set the local log level filter and pass it to the native logger. */
void ipc_logging_set_level(enum gpi_log_level level);

/** Restore the native log handler. */
void ipc_logging_finalize();

}  // namespace ipc
}  // namespace cocotb

#endif /* COCOTB_IPC_PRIV_HPP_ */
