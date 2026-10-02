// Copyright cocotb contributors
// Copyright (c) 2013, 2018 Potential Ventures Ltd
// Copyright (c) 2013 SolarFlare Communications Inc
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// IPC server inside the simulator process: spawns a Python child process
// implementing `cocotb.simulator`, supervises it, and drives it over the IPC
// connection.
//
// The GPI_USERS entry point `initialize` is loaded by the GPI layer after the
// simulator interface library has been registered. It:
//   1. starts the IPC transport (a loopback TCP server),
//   2. spawns the Python child process (`PYGPI_PYTHON_BIN -m cocotb._ipc`),
//   3. completes a hello/ready handshake with it,
//   4. registers the GPI callbacks that drive the child process, and
//   5. serves requests from the child process reentrantly while waiting for
//      callback acknowledgements.
//
// Supervision guarantees:
//   - the child is reaped in `finalize` (no zombies), escalating to a kill
//     if it does not exit on its own,
//   - child death is detected immediately through the connection (EOF), and
//   - orphans are prevented: on Linux the child gets PR_SET_PDEATHSIG, on
//     Windows it runs inside a kill-on-close Job Object, and on all POSIX
//     platforms the child is killed explicitly when the simulator exits.

#include "./ipc_priv.hpp"

#include <gpi.h>

#include <cerrno>   // errno, ERANGE
#include <climits>  // UINT_MAX
#include <cstdint>  // uint64_t
#include <cstdio>   // fprintf
#include <cstdlib>  // getenv, strtoul
#include <cstring>  // strcmp, strerror
#include <mutex>
#include <string>
#include <utility>  // move
#include <vector>

#include "../utils.hpp"  // DEFER

#if defined(_WIN32)
#include <windows.h>
#define sleep(n) Sleep(1000 * (n))
#define getpid() GetCurrentProcessId()
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>  // clock_gettime
#include <unistd.h>  // fork, execl, getpid, usleep
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

namespace cocotb {
namespace ipc {

IpcTransport *g_transport = nullptr;
bool g_connection_alive = false;
bool ipc_debug_enabled = false;

}  // namespace ipc
}  // namespace cocotb

// The helpers and lifecycle callbacks below use IPC internals unqualified.
using namespace cocotb::ipc;

namespace {

using cocotb::ipc::IpcTransport;
using cocotb::ipc::IpcValue;

/*******************************************************************************
 * Monotonic-ish clock for deadlines
 *******************************************************************************/

int64_t now_ms() {
#if defined(_WIN32)
    return static_cast<int64_t>(GetTickCount64());
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
#endif
}

// Milliseconds until `deadline`, capped at `cap`; -1 when expired.
long remaining(int64_t deadline, long cap) {
    int64_t rem = deadline - now_ms();
    if (rem <= 0) {
        return -1;
    }
    if (rem < cap) {
        return static_cast<long>(rem);
    }
    return cap;
}

/*******************************************************************************
 * Child process state and helpers
 *******************************************************************************/

struct ChildProcess {
#if defined(_WIN32)
    HANDLE process = nullptr;
    HANDLE job = nullptr;
#endif
    long native_pid = -1;
    bool reaped = false;
    int exit_code = 0;
};

ChildProcess child;

// Fill `exit_code` and return true when the child has exited (reaping it on
// POSIX, where that is how its status becomes available).
bool child_poll(int *exit_code) {
    if (child.reaped) {
        *exit_code = child.exit_code;
        return true;
    }
#if defined(_WIN32)
    if (child.process == nullptr) {
        return false;
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(child.process, &code)) {
        return false;
    }
    if (code == STILL_ACTIVE) {
        return false;
    }
    child.reaped = true;
    child.exit_code = static_cast<int>(code);
    *exit_code = child.exit_code;
    return true;
#else
    if (child.native_pid <= 0) {
        return false;
    }
    int status = 0;
    pid_t pid = static_cast<pid_t>(child.native_pid);
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == 0) {
        return false;
    }
    if (r == pid) {
        child.reaped = true;
        if (WIFEXITED(status)) {
            child.exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            child.exit_code = 128 + WTERMSIG(status);
        } else {
            child.exit_code = -1;
        }
        *exit_code = child.exit_code;
        return true;
    }
    // r < 0: ECHILD, someone else reaped it (or never spawned).
    return false;
#endif
}

#if defined(_WIN32)

std::wstring widen(const std::string &s) {
    if (s.empty()) {
        return std::wstring();
    }
    int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()),
                                nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()),
                        &out[0], n);
    return out;
}

// UTF-8 -> wide for the embedded zip path: widen() above decodes the
// ANSI codepage, which would mangle non-ASCII temp directories.
std::wstring widen_utf8(const std::string &s) {
    if (s.empty()) {
        return std::wstring();
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {
        return std::wstring();
    }
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &out[0], n);
    return out;
}

wchar_t env_key_lower(wchar_t ch) {
    return (ch >= L'A' && ch <= L'Z')
               ? static_cast<wchar_t>(ch - L'A' + L'a')
               : ch;
}

// Does `entry` (a "KEY=VALUE" string) hold `key`, compared
// case-insensitively the way Windows treats environment names?
bool env_entry_has_key(const std::wstring &entry, const wchar_t *key) {
    size_t i = 0;
    for (; key[i] != L'\0'; ++i) {
        if (i >= entry.size() || entry[i] == L'=') {
            return false;
        }
        if (env_key_lower(entry[i]) != env_key_lower(key[i])) {
            return false;
        }
    }
    return i < entry.size() && entry[i] == L'=';
}

// Case-insensitive comparison of the first `a_len`/`b_len` characters.
int env_key_compare(const std::wstring &a, size_t a_len,
                    const std::wstring &b, size_t b_len) {
    const size_t common = a_len < b_len ? a_len : b_len;
    for (size_t i = 0; i < common; ++i) {
        const wchar_t ca = env_key_lower(a[i]);
        const wchar_t cb = env_key_lower(b[i]);
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
    }
    if (a_len == b_len) {
        return 0;
    }
    return a_len < b_len ? -1 : 1;
}

// Insert `entry` keeping the block sorted the way Windows maintains
// environment strings (some lookups assume sorted blocks).
void env_insert_sorted(std::vector<std::wstring> *entries,
                       const std::wstring &entry) {
    const size_t entry_sep = entry.find(L'=');
    const size_t entry_key_len =
        entry_sep == std::wstring::npos ? entry.size() : entry_sep;
    typedef std::vector<std::wstring>::difference_type diff_type;
    for (size_t i = 0; i < entries->size(); ++i) {
        const std::wstring &other = (*entries)[i];
        const size_t other_sep = other.find(L'=');
        const size_t other_key_len =
            other_sep == std::wstring::npos ? other.size() : other_sep;
        if (env_key_compare(entry, entry_key_len, other, other_key_len) < 0) {
            entries->insert(entries->begin() + static_cast<diff_type>(i),
                            entry);
            return;
        }
    }
    entries->push_back(entry);
}

// The environment block for CreateProcessW: the simulator's own
// environment, PYTHONUNBUFFERED=1 (the POSIX path sets it already;
// Windows previously passed no block at all), and PYTHONPATH extended
// with the embedded package zip when given. Existing PYTHONPATH entries
// keep priority over the zip, which preserves the documented
// resolution order: user PYTHONPATH > embedded zip > installed cocotb.
std::vector<wchar_t> build_env_block(const std::string &pythonpath_zip) {
    std::vector<std::wstring> entries;
    LPWCH raw = GetEnvironmentStringsW();
    if (raw != nullptr) {
        for (LPWCH cursor = raw; *cursor != L'\0';) {
            entries.push_back(std::wstring(cursor));
            cursor += entries.back().size() + 1;
        }
        FreeEnvironmentStringsW(raw);
    }

    bool have_unbuffered = false;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (env_entry_has_key(entries[i], L"PYTHONUNBUFFERED")) {
            entries[i] = L"PYTHONUNBUFFERED=1";
            have_unbuffered = true;
            break;
        }
    }
    if (!have_unbuffered) {
        env_insert_sorted(&entries, L"PYTHONUNBUFFERED=1");
    }

    if (!pythonpath_zip.empty()) {
        const std::wstring zip = widen_utf8(pythonpath_zip);
        size_t index = entries.size();
        for (size_t i = 0; i < entries.size(); ++i) {
            if (env_entry_has_key(entries[i], L"PYTHONPATH")) {
                index = i;
                break;
            }
        }
        if (index == entries.size()) {
            env_insert_sorted(&entries, L"PYTHONPATH=" + zip);
        } else {
            // Append the zip behind the user's entries; an empty value
            // must not grow a leading separator (that means "cwd").
            const size_t eq = entries[index].find(L'=');
            const std::wstring value = entries[index].substr(eq + 1);
            if (value.empty()) {
                entries[index].append(zip);
            } else {
                entries[index].push_back(L';');
                entries[index].append(zip);
            }
        }
    }

    std::vector<wchar_t> block;
    for (size_t i = 0; i < entries.size(); ++i) {
        block.insert(block.end(), entries[i].begin(), entries[i].end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

bool spawn_child(const std::string &python_bin, const std::string &module,
                 const std::string &endpoint,
                 const std::string &pythonpath_zip) {
    // Kill-on-close job object: if the simulator process dies for any
    // reason, Windows kills the child with it. No orphans.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
        std::memset(&info, 0, sizeof(info));
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                     &info, sizeof(info))) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    std::wstring cmdline =
        L"\"" + widen(python_bin) + L"\" -m " + widen(module) + L" " +
        widen(endpoint);
    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(L'\0');

    STARTUPINFOW si;
    std::memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    // Inherit stdio: Python tracebacks and print() output must be visible
    // in the same place as the simulator's own output.
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi;
    std::memset(&pi, 0, sizeof(pi));

    // Own environment block so PYTHONUNBUFFERED and the embedded zip's
    // PYTHONPATH reach the child (an inherited environment cannot be
    // extended per process).
    std::vector<wchar_t> env_block = build_env_block(pythonpath_zip);

    BOOL ok = CreateProcessW(nullptr, &buf[0], nullptr, nullptr, TRUE,
                             CREATE_SUSPENDED, env_block.data(), nullptr, &si,
                             &pi);
    if (!ok) {
        IPC_LOG_ERROR("Failed to spawn Python process '%s' (error %lu)",
                      cmdline.c_str(), GetLastError());
        if (job != nullptr) {
            CloseHandle(job);
        }
        return false;
    }

    if (job != nullptr) {
        if (!AssignProcessToJobObject(job, pi.hProcess)) {
            // Not fatal: can happen when the simulator itself is not in a
            // job on older Windows versions; explicit reaping still applies.
            IPC_LOG_WARN("Could not assign Python process to a job object "
                         "(error %lu)",
                         GetLastError());
            CloseHandle(job);
            job = nullptr;
        }
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    child.process = pi.hProcess;
    child.job = job;
    child.native_pid = static_cast<long>(pi.dwProcessId);
    return true;
}

#else  // POSIX

bool spawn_child(const std::string &python_bin, const std::string &module,
                 const std::string &endpoint,
                 const std::string &pythonpath_zip) {
    pid_t parent_pid = getpid();
    pid_t pid = fork();
    if (pid < 0) {
        IPC_LOG_ERROR("fork() failed: %s", strerror(errno));
        return false;
    }
    if (pid == 0) {
        // Child: request a kill signal if the simulator exits, so the child
        // can never outlive us as an orphan.
#ifdef __linux__
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        // PR_SET_PDEATHSIG is racy against the parent dying between fork()
        // and prctl(); check that the parent is still who we expect.
        if (getppid() != parent_pid) {
            _exit(127);
        }
#endif
        // Line-buffered/unbuffered output so Python output interleaves with
        // the simulator's output in real time.
        setenv("PYTHONUNBUFFERED", "1", 1);

        // Put the embedded package zip behind the user's PYTHONPATH
        // entries, preserving the documented resolution order:
        // user PYTHONPATH > embedded zip > installed cocotb.
        if (!pythonpath_zip.empty()) {
            const char *existing = getenv("PYTHONPATH");
            const std::string pythonpath =
                (existing != nullptr && existing[0] != '\0')
                    ? std::string(existing) + ":" + pythonpath_zip
                    : pythonpath_zip;
            setenv("PYTHONPATH", pythonpath.c_str(), 1);
        }

        execl(python_bin.c_str(), python_bin.c_str(), "-m", module.c_str(),
              endpoint.c_str(), (char *)nullptr);
        // Only reached when exec fails.
        fprintf(stderr, "cocotb: failed to execute %s: %s\n",
                python_bin.c_str(), strerror(errno));
        _exit(127);
    }

    child.native_pid = pid;
    return true;
}

#endif

void ipc_reap_child_impl() {
#if defined(_WIN32)
    if (child.process == nullptr) {
        return;
    }
    if (WaitForSingleObject(child.process,
                            cocotb::ipc::kShutdownTimeoutMs) == WAIT_TIMEOUT) {
        IPC_LOG_WARN("Python process did not exit, terminating it");
        TerminateProcess(child.process, 1);
        WaitForSingleObject(child.process, INFINITE);
    }
    DWORD code = 0;
    GetExitCodeProcess(child.process, &code);
    if (code != 0) {
        IPC_LOG_WARN("Python process exited with status %lu", code);
    }
    CloseHandle(child.process);
    child.process = nullptr;
    if (child.job != nullptr) {
        CloseHandle(child.job);
        child.job = nullptr;
    }
#else
    if (child.native_pid <= 0 || child.reaped) {
        return;
    }
    int status = 0;
    pid_t cpid = static_cast<pid_t>(child.native_pid);
    for (int i = 0; i < 50; ++i) {  // wait up to 5 s
        pid_t r = waitpid(cpid, &status, WNOHANG);
        if (r == cpid) {
            child.reaped = true;
            if (WIFEXITED(status)) {
                child.exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                child.exit_code = 128 + WTERMSIG(status);
            }
            if (child.exit_code != 0) {
                IPC_LOG_WARN("Python process exited with status %d",
                             child.exit_code);
            }
            return;
        }
        if (r < 0) {
            return;  // ECHILD: nothing to reap
        }
        usleep(100 * 1000);
    }
    IPC_LOG_WARN("Python process (pid %ld) did not exit, killing it",
                 child.native_pid);
    kill(cpid, SIGKILL);
    waitpid(cpid, &status, 0);
    child.reaped = true;
    child.exit_code = 128 + SIGKILL;
#endif
}

/*******************************************************************************
 * Message plumbing
 *******************************************************************************/

std::mutex send_mutex;

uint64_t next_msg_id = 1;

// One-shot guards mirroring the legacy embed module.
bool initialize_called = false;
bool start_of_sim_called = false;

void serve_request(const IpcValue &req);

}  // namespace

namespace cocotb {
namespace ipc {

bool ipc_send_message(const IpcValue &msg) {
    std::lock_guard<std::mutex> lock(send_mutex);
    if (g_transport == nullptr || !g_connection_alive) {
        return false;
    }
    return send_value(*g_transport, g_ipc_protocol, msg);
}

void ipc_mark_connection_lost(const char *reason) {
    if (!g_connection_alive) {
        return;
    }
    g_connection_alive = false;
    // Note: set alive=false *before* logging; the log handler will fail to
    // forward and print through the native handler instead.
    int code = 0;
    if (child_poll(&code)) {
        IPC_LOG_ERROR("Lost connection to the Python process (%s); it "
                      "exited with status %d",
                      reason, code);
    } else {
        IPC_LOG_ERROR("Lost connection to the Python process (%s); it is "
                      "still running and will be cleaned up at shutdown",
                      reason);
    }
}

int send_callback_and_wait(IpcValue &msg, long timeout_ms) {
    if (!g_connection_alive || g_transport == nullptr) {
        return -1;
    }

    uint64_t id = next_msg_id++;
    msg.set("id", IpcValue::integer(static_cast<int64_t>(id)));
    if (!ipc_send_message(msg)) {
        ipc_mark_connection_lost("failed to send an event");
        return -1;
    }

    int64_t deadline = timeout_ms >= 0 ? now_ms() + timeout_ms : -1;
    while (true) {
        long wait_ms = -1;
        if (deadline >= 0) {
            wait_ms = remaining(deadline, 250);
            if (wait_ms < 0) {
                IPC_LOG_ERROR("timed out waiting for the Python process to "
                              "acknowledge an event");
                return -1;
            }
        }

        if (!g_transport->wait_readable(wait_ms)) {
            if (!g_transport->is_open()) {
                ipc_mark_connection_lost(
                    "connection closed while waiting for an acknowledgement");
                return -1;
            }
            continue;  // deadline expired; loop re-checks it
        }

        IpcValue incoming;
        if (!recv_value(*g_transport, g_ipc_protocol, incoming)) {
            ipc_mark_connection_lost(
                "connection closed while waiting for an acknowledgement");
            return -1;
        }

        const IpcValue *type = incoming.get("type");
        if (type == nullptr || !type->is_string()) {
            continue;  // ignore anything unexpected
        }
        const std::string &t = type->get_string();

        if (t == "request") {
            // Serve the child's request reentrantly while waiting: the GPI
            // call that led here is still on the stack, so GPI calls from
            // the child are safe to dispatch now. (`ipc_send_message` takes
            // the send lock internally for frame atomicity.)
            serve_request(incoming);
            continue;
        }

        if (t == "callback_ack") {
            const IpcValue *mid = incoming.get("id");
            if (mid != nullptr && mid->is_int() &&
                static_cast<uint64_t>(mid->get_int()) == id) {
                const IpcValue *result = incoming.get("result");
                return result != nullptr
                           ? static_cast<int>(result->get_int())
                           : 0;
            }
            continue;  // stale ack from a timed-out wait
        }
    }
}

}  // namespace ipc
}  // namespace cocotb

namespace {

using cocotb::ipc::ipc_send_message;
using cocotb::ipc::ipc_mark_connection_lost;

// Build and send the response for one request. Assumes the caller holds
// `send_mutex`.
void serve_request(const IpcValue &req) {
    IpcValue response = IpcValue::object();
    response.set("type", IpcValue::string("response"));
    const IpcValue *id = req.get("id");
    response.set("id", id != nullptr ? *id : IpcValue::null());

    IpcValue result;
    std::string error;
    bool ok = cocotb::ipc::ipc_dispatch_request(req, result, error);
    response.set("ok", IpcValue::boolean(ok));
    if (ok) {
        response.set("result", std::move(result));
    } else {
        response.set("error", IpcValue::string(std::move(error)));
    }

    if (!ipc_send_message(response)) {
        ipc_mark_connection_lost("failed to send a response");
    }
}

/*******************************************************************************
 * Handshake
 *******************************************************************************/

bool handshake() {
    const int64_t deadline = now_ms() + cocotb::ipc::kSpawnTimeoutMs;

    // 1. Wait for the TCP connection while watching the child's health, so
    //    an import error (child exits) is reported immediately rather than
    //    after a timeout.
    while (true) {
        long slice = remaining(deadline, 250);
        if (slice < 0) {
            IPC_LOG_ERROR("Timed out waiting for the Python process to "
                          "connect");
            return false;
        }
        if (g_transport->wait_for_client(slice)) {
            break;
        }
        int code = 0;
        if (child_poll(&code)) {
            IPC_LOG_ERROR(
                "Python process exited during startup (status %d); see the "
                "errors printed above",
                code);
            return false;
        }
    }

    // 2. Read the hello message.
    while (true) {
        long slice = remaining(deadline, 250);
        if (slice < 0) {
            IPC_LOG_ERROR("Timed out waiting for the handshake message from "
                          "the Python process");
            return false;
        }
        if (g_transport->wait_readable(slice)) {
            break;
        }
        int code = 0;
        if (child_poll(&code)) {
            IPC_LOG_ERROR(
                "Python process exited during startup (status %d); see the "
                "errors printed above",
                code);
            return false;
        }
    }

    IpcValue hello;
    if (!recv_value(*g_transport, cocotb::ipc::g_ipc_protocol, hello)) {
        int code = 0;
        child_poll(&code);
        IPC_LOG_ERROR(
            "Connection closed during handshake (Python process status %d)",
            code);
        return false;
    }

    const IpcValue *type = hello.get("type");
    const IpcValue *version = hello.get("version");
    bool ok = type != nullptr && type->is_string() &&
              type->get_string() == "hello" && version != nullptr &&
              version->is_int() &&
              static_cast<uint64_t>(version->get_int()) ==
                  cocotb::ipc::kProtocolVersion;
    if (!ok) {
        IPC_LOG_ERROR(
            "Unexpected handshake message from the Python process (protocol "
            "version mismatch? simulator expects %llu)",
            static_cast<unsigned long long>(cocotb::ipc::kProtocolVersion));
    }

    IpcValue ready = IpcValue::object();
    ready.set("type", IpcValue::string("ready"));
    ready.set("version",
              IpcValue::integer(static_cast<int64_t>(
                  cocotb::ipc::kProtocolVersion)));
    ready.set("ok", IpcValue::boolean(ok));
    if (!send_value(*g_transport, cocotb::ipc::g_ipc_protocol, ready)) {
        return false;
    }
    return ok;
}

/*******************************************************************************
 * GPI lifecycle callbacks
 *******************************************************************************/

int start_of_sim_time(void *) {
    IPC_LOG_TRACE("GPI Start Sim => [ IPC Start Sim ]");
    DEFER(IPC_LOG_TRACE("[ IPC Start Sim ] => GPI Start Sim"));

    if (start_of_sim_called) {
        // LCOV_EXCL_START
        IPC_LOG_ERROR("IPC library initialized again!");
        return -1;
        // LCOV_EXCL_STOP
    }
    start_of_sim_called = true;

    if (!g_connection_alive) {
        return -1;
    }

    uint32_t high = 0;
    uint32_t low = 0;
    gpi_get_sim_time(&high, &low);

    IpcValue msg = IpcValue::object();
    msg.set("type", IpcValue::string("callback"));
    msg.set("func", IpcValue::string("start_of_sim_time"));
    msg.set("cb_id", IpcValue::integer(0));
    IpcValue time = IpcValue::array();
    time.push_back(IpcValue::integer(high));
    time.push_back(IpcValue::integer(low));
    msg.set("time", std::move(time));

    return cocotb::ipc::send_callback_and_wait(msg);
}

void end_of_sim_time(void *) {
    IPC_LOG_TRACE("GPI End Sim => [ IPC End Sim ]");
    DEFER(IPC_LOG_TRACE("[ IPC End Sim ] => GPI End Sim"));

    if (!g_connection_alive) {
        return;
    }

    uint32_t high = 0;
    uint32_t low = 0;
    gpi_get_sim_time(&high, &low);

    IpcValue msg = IpcValue::object();
    msg.set("type", IpcValue::string("callback"));
    msg.set("func", IpcValue::string("end_of_sim_time"));
    msg.set("cb_id", IpcValue::integer(0));
    IpcValue time = IpcValue::array();
    time.push_back(IpcValue::integer(high));
    time.push_back(IpcValue::integer(low));
    msg.set("time", std::move(time));

    int result = cocotb::ipc::send_callback_and_wait(msg);
    if (result == -1) {
        IPC_LOG_ERROR("Passing event to upper layer failed");
    }
}

void finalize(void *) {
    IPC_LOG_TRACE("GPI Finalize => [ IPC Finalize ]");
    DEFER(IPC_LOG_TRACE("[ IPC Finalize ] => GPI Finalize"));

    // 1. Tell the Python process to shut down (runs its end-of-sim cleanup:
    //    regression results, atexit handlers, ...). Bounded so a wedged
    //    child cannot hang simulator teardown.
    if (g_connection_alive) {
        IpcValue msg = IpcValue::object();
        msg.set("type", IpcValue::string("callback"));
        msg.set("func", IpcValue::string("finalize"));
        msg.set("cb_id", IpcValue::integer(0));
        cocotb::ipc::send_callback_and_wait(msg, cocotb::ipc::kShutdownTimeoutMs);
    }

    // 2. Restore native logging so shutdown diagnostics go to the console.
    ipc_logging_finalize();

    // 3. Close the connection; the child notices EOF and exits on its own.
    g_connection_alive = false;
    if (g_transport != nullptr) {
        g_transport->close();
    }

    // 4. Reap the child (waiting, then killing if necessary) so no zombie or
    //    orphan is left behind.
    ipc_reap_child_impl();

    delete g_transport;
    g_transport = nullptr;
}

void attach_pause() {
    /* Before returning we check if the user wants to pause the simulator
       thread such that they can attach a debugger. */
    const char *pause = getenv("COCOTB_ATTACH");
    if (pause) {
        unsigned long sleep_time = strtoul(pause, NULL, 10);
        if (errno == ERANGE || sleep_time >= UINT_MAX) {
            // LCOV_EXCL_START
            IPC_LOG_ERROR("COCOTB_ATTACH only needs to be set to ~30 seconds");
            return;
            // LCOV_EXCL_STOP
        }
        if ((errno != 0 && sleep_time == 0) || (sleep_time <= 0)) {
            // LCOV_EXCL_START
            IPC_LOG_ERROR(
                "COCOTB_ATTACH must be set to an integer base 10 or omitted");
            return;
            // LCOV_EXCL_STOP
        }

        IPC_LOG_INFO(
            "Waiting for %lu seconds - attach to PID %d with your debugger",
            sleep_time, getpid());
        sleep((unsigned int)sleep_time);
    }
}

}  // namespace

extern "C" IPC_EXPORT void initialize(void) {
    cocotb::ipc::ipc_debug_enabled =
        (getenv("COCOTB_IPC_DEBUG") != nullptr) ||
        (getenv("PYGPI_DEBUG") != nullptr);
    ipc_logging_initialize();

    IPC_LOG_TRACE("GPI Init => [ IPC Init ]");
    DEFER(IPC_LOG_TRACE("[ IPC Init ] => GPI Init"));

    if (initialize_called) {
        // LCOV_EXCL_START
        IPC_LOG_ERROR("IPC library initialized again!");
        return;
        // LCOV_EXCL_STOP
    }
    initialize_called = true;

    // Pause first: the handshake has a deadline, and a debugger session
    // must not burn it.
    attach_pause();

    cocotb::ipc::resolve_protocol();

    const char *python_bin = getenv("PYGPI_PYTHON_BIN");
    if (python_bin == nullptr || python_bin[0] == '\0') {
        // LCOV_EXCL_START
        IPC_LOG_ERROR(
            "PYGPI_PYTHON_BIN variable not set. Can't start the Python "
            "test process!");
        return;
        // LCOV_EXCL_STOP
    }

    cocotb::ipc::g_transport = IpcTransport::create();
    if (cocotb::ipc::g_transport == nullptr ||
        !cocotb::ipc::g_transport->open()) {
        // LCOV_EXCL_START
        IPC_LOG_ERROR("Failed to open the IPC transport");
        delete cocotb::ipc::g_transport;
        cocotb::ipc::g_transport = nullptr;
        return;
        // LCOV_EXCL_STOP
    }

    // Connection considered alive from here: sends can flow once the child
    // connects (the ready message below goes over it).
    cocotb::ipc::g_connection_alive = true;

    // Materialize the cocotb package zip embedded in this library (when
    // built with COCOTB_IPC_EMBED_ZIP) so the child can import cocotb
    // from it without an installed copy.
    const std::string package_zip = cocotb::ipc::embedded_zip_path();

    if (!spawn_child(python_bin, "cocotb._ipc",
                     cocotb::ipc::g_transport->get_endpoint(), package_zip)) {
        cocotb::ipc::g_connection_alive = false;
        cocotb::ipc::g_transport->close();
        delete cocotb::ipc::g_transport;
        cocotb::ipc::g_transport = nullptr;
        return;
    }

    if (!handshake()) {
        cocotb::ipc::g_connection_alive = false;
        cocotb::ipc::g_transport->close();
        ipc_reap_child_impl();
        delete cocotb::ipc::g_transport;
        cocotb::ipc::g_transport = nullptr;
        return;
    }

    gpi_register_start_of_sim_time_callback(start_of_sim_time, nullptr);
    gpi_register_end_of_sim_time_callback(end_of_sim_time, nullptr);
    gpi_register_finalize_callback(finalize, nullptr);

    IPC_LOG_INFO("Started Python process (pid %ld), endpoint %s",
                 child.native_pid,
                 cocotb::ipc::g_transport->get_endpoint().c_str());
}
