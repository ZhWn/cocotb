// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Shared-memory transport: server side (implemented in Python's `_shm.py`
// for the client side). See ipc_shm.hpp for the wire layout.

#include "./ipc_base.hpp"
#include "./ipc_priv.hpp"
#include "./ipc_shm.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <process.h>   // _getpid
#include <windows.h>
#else
#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

namespace cocotb {
namespace ipc {

namespace {

constexpr uint64_t kMagic = 0x434F434F54494250ULL;  // "COCOTBIP"
constexpr uint32_t kVersion = 1;
constexpr uint64_t kHeaderSize = 256;
// Minimum ring capacity per direction (guards against silly env values).
constexpr uint64_t kMinCap = 64 * 1024;
constexpr uint64_t kMaxFrame = 1u << 30;

struct ShmLayout {
    uint8_t *base = nullptr;
    size_t region_size = 0;

    uint64_t req_cap = 0;
    uint64_t resp_cap = 0;
    uint8_t *req_ring = nullptr;
    uint8_t *resp_ring = nullptr;

    std::atomic<uint64_t> *req_prod = nullptr;   // Python writes
    std::atomic<uint64_t> *req_cons = nullptr;   // server reads
    std::atomic<uint64_t> *resp_prod = nullptr;  // server writes
    std::atomic<uint64_t> *resp_cons = nullptr;  // Python reads
    std::atomic<uint32_t> *state = nullptr;

    std::mutex send_mutex;

#ifdef _WIN32
    HANDLE map_handle = nullptr;
    HANDLE sem_req = nullptr;
    HANDLE sem_resp = nullptr;
    HANDLE sem_conn = nullptr;
#else
    int shm_fd = -1;
    sem_t *sem_req = nullptr;
    sem_t *sem_resp = nullptr;
    sem_t *sem_conn = nullptr;
#endif
};

std::string make_token() {
    char buf[64];
#ifdef _WIN32
    std::snprintf(buf, sizeof(buf), "%lu_%04x",
                  static_cast<unsigned long>(_getpid()),
                  static_cast<unsigned>(GetCurrentProcessId() & 0xFFFF) );
#else
    std::snprintf(buf, sizeof(buf), "%ld_%04x", static_cast<long>(getpid()),
                  static_cast<unsigned>(std::rand() & 0xFFFF));
#endif
    return std::string(buf);
}

#ifdef _WIN32
std::string shm_name(const std::string &tok) { return "cocotb_ipc_" + tok; }
std::string sem_name(const std::string &tok, const char *kind) {
    return "cocotb_ipc_" + tok + "_" + kind;
}
#else
std::string shm_name(const std::string &tok) { return "/cocotb_ipc_" + tok; }
std::string sem_name(const std::string &tok, const char *kind) {
    return "/cocotb_ipc_" + tok + "_" + kind;
}
#endif

// Total shared-memory size per direction, in bytes, from COCOTB_IPC_SHMMB
// (total MiB, split evenly; default 32 MiB total => 16 MiB per ring).
uint64_t ring_capacity() {
    const char *env = std::getenv("COCOTB_IPC_SHMMB");
    uint64_t total_mb = 32;
    if (env && *env) {
        char *end = nullptr;
        unsigned long v = std::strtoul(env, &end, 10);
        if (end != env && v > 0) total_mb = v;
    }
    uint64_t per = (total_mb * 1024 * 1024) / 2;
    return per < kMinCap ? kMinCap : per;
}

bool wait_sem(void *sem, long timeout_ms) {
#ifdef _WIN32
    HANDLE h = static_cast<HANDLE>(sem);
    DWORD ms = timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms);
    return WaitForSingleObject(h, ms) == WAIT_OBJECT_0;
#else
    sem_t *s = static_cast<sem_t *>(sem);
    if (timeout_ms < 0) return sem_wait(s) == 0;
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return false;
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    return sem_timedwait(s, &deadline) == 0;
#endif
}

bool post_sem(void *sem) {
#ifdef _WIN32
    return ReleaseSemaphore(static_cast<HANDLE>(sem), 1, nullptr) != 0;
#else
    return sem_post(static_cast<sem_t *>(sem)) == 0;
#endif
}

// Copy len bytes from data into the ring starting at position pos (the ring
// capacity is assumed to be a multiple of the frame contents; a frame may
// still wrap around the ring end).
void ring_write(uint8_t *ring, uint64_t cap, uint64_t pos,
                const uint8_t *data, size_t len) {
    if (len == 0) return;
    uint64_t idx = pos % cap;
    if (idx + len <= cap) {
        std::memcpy(ring + idx, data, len);
    } else {
        size_t first = static_cast<size_t>(cap - idx);
        std::memcpy(ring + idx, data, first);
        std::memcpy(ring, data + first, len - first);
    }
}

void ring_read(const uint8_t *ring, uint64_t cap, uint64_t pos,
               uint8_t *out, size_t len) {
    if (len == 0) return;
    uint64_t idx = pos % cap;
    if (idx + len <= cap) {
        std::memcpy(out, ring + idx, len);
    } else {
        size_t first = static_cast<size_t>(cap - idx);
        std::memcpy(out, ring + idx, first);
        std::memcpy(out + first, ring, len - first);
    }
}

class ShmTransport final : public cocotb::ipc::IpcTransport {
  public:
    ShmTransport() = default;
    ~ShmTransport() override { close(); }

    bool open() override {
        token_ = make_token();
        std::string ctx = "shm(" + token_ + ")";

        uint64_t cap = ring_capacity();
        size_t region_size = static_cast<size_t>(kHeaderSize + 2 * cap);

        ShmLayout *L = new ShmLayout();
        impl_.reset(L);

        uint8_t *base = nullptr;
#ifdef _WIN32
        DWORD size_hi = static_cast<DWORD>(region_size >> 32);
        DWORD size_lo = static_cast<DWORD>(region_size & 0xFFFFFFFF);
        L->map_handle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr,
                                           PAGE_READWRITE, size_hi, size_lo,
                                           shm_name(token_).c_str());
        if (!L->map_handle) {
            IPC_LOG_ERROR("IPC: failed to create shared memory (%s)",
                          ctx.c_str());
            return false;
        }
        // Refuse to attach to a region created by someone else.
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            IPC_LOG_ERROR("IPC: shared memory name already in use (%s)",
                          ctx.c_str());
            return false;
        }
        base = static_cast<uint8_t *>(MapViewOfFile(
            L->map_handle, FILE_MAP_ALL_ACCESS, 0, 0, region_size));
        if (!base) {
            IPC_LOG_ERROR("IPC: failed to map view of shared memory (%s)",
                          ctx.c_str());
            return false;
        }
        L->sem_req = CreateSemaphoreA(nullptr, 0, 0x7FFFFFFF,
                                      sem_name(token_, "req").c_str());
        L->sem_resp = CreateSemaphoreA(nullptr, 0, 0x7FFFFFFF,
                                       sem_name(token_, "resp").c_str());
        L->sem_conn = CreateSemaphoreA(nullptr, 0, 0x7FFFFFFF,
                                       sem_name(token_, "conn").c_str());
        if (!L->sem_req || !L->sem_resp || !L->sem_conn) {
            IPC_LOG_ERROR("IPC: failed to create semaphores (%s)",
                          ctx.c_str());
            return false;
        }
#else
        int fd = ::shm_open(shm_name(token_).c_str(),
                            O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0) {
            IPC_LOG_ERROR("IPC: failed to create shared memory (%s): %s",
                          ctx.c_str(), std::strerror(errno));
            return false;
        }
        if (::ftruncate(fd, static_cast<off_t>(region_size)) != 0) {
            IPC_LOG_ERROR("IPC: ftruncate failed (%s): %s", ctx.c_str(),
                          std::strerror(errno));
            ::close(fd);
            return false;
        }
        base = static_cast<uint8_t *>(
            ::mmap(nullptr, region_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, 0));
        ::close(fd);
        if (base == MAP_FAILED) {
            IPC_LOG_ERROR("IPC: mmap failed (%s): %s", ctx.c_str(),
                          std::strerror(errno));
            return false;
        }
        L->sem_req = ::sem_open(sem_name(token_, "req").c_str(),
                                O_CREAT | O_EXCL, 0600, 0);
        L->sem_resp = ::sem_open(sem_name(token_, "resp").c_str(),
                                 O_CREAT | O_EXCL, 0600, 0);
        L->sem_conn = ::sem_open(sem_name(token_, "conn").c_str(),
                                 O_CREAT | O_EXCL, 0600, 0);
        if (L->sem_req == SEM_FAILED || L->sem_resp == SEM_FAILED ||
            L->sem_conn == SEM_FAILED) {
            IPC_LOG_ERROR("IPC: failed to create semaphores (%s): %s",
                          ctx.c_str(), std::strerror(errno));
            return false;
        }
#endif

        L->base = base;
        L->region_size = region_size;
        L->req_cap = cap;
        L->resp_cap = cap;
        L->req_ring = base + kHeaderSize;
        L->resp_ring = base + kHeaderSize + cap;

        // Publish the header (aligns with the Python client layout).
        std::memset(base, 0, kHeaderSize);
        uint64_t magic = kMagic;
        uint32_t version = kVersion;
        uint32_t state = 1;
        std::memcpy(base + 0, &magic, sizeof(magic));
        std::memcpy(base + 8, &version, sizeof(version));
        std::memcpy(base + 12, &state, sizeof(state));
        std::memcpy(base + 48, &cap, sizeof(cap));
        std::memcpy(base + 56, &cap, sizeof(cap));

        L->req_prod = reinterpret_cast<std::atomic<uint64_t> *>(base + 16);
        L->req_cons = reinterpret_cast<std::atomic<uint64_t> *>(base + 24);
        L->resp_prod = reinterpret_cast<std::atomic<uint64_t> *>(base + 32);
        L->resp_cons = reinterpret_cast<std::atomic<uint64_t> *>(base + 40);
        L->state = reinterpret_cast<std::atomic<uint32_t> *>(base + 12);

        IPC_LOG_INFO("IPC: shared memory transport ready (%s, %zu MiB)",
                     ctx.c_str(), (region_size / (1024 * 1024)));
        return true;
    }

    std::string get_endpoint() const override {
        return "shm:" + token_;
    }

    bool wait_for_client(long timeout_ms) override {
        return impl_ && wait_sem(impl_->sem_conn, timeout_ms);
    }

    bool send(const char *data, size_t len) override {
        if (!impl_ || !impl_->base) return false;
        std::lock_guard<std::mutex> lock(impl_->send_mutex);
        if (len + 4 > impl_->resp_cap) {
            IPC_LOG_ERROR("IPC: response frame too large for ring (%zu)",
                          len);
            return false;
        }
        uint64_t prod = impl_->resp_prod->load(std::memory_order_relaxed);
        uint64_t cons = impl_->resp_cons->load(std::memory_order_acquire);
        if (prod - cons + len + 4 > impl_->resp_cap) {
            IPC_LOG_ERROR("IPC: response ring full (%zu bytes pending)", len);
            return false;
        }
        uint8_t hdr[4];
        uint32_t ll = static_cast<uint32_t>(len);
        std::memcpy(hdr, &ll, 4);
        ring_write(impl_->resp_ring, impl_->resp_cap, prod, hdr, 4);
        ring_write(impl_->resp_ring, impl_->resp_cap, prod + 4,
                   reinterpret_cast<const uint8_t *>(data), len);
        impl_->resp_prod->store(prod + 4 + len, std::memory_order_release);
        return post_sem(impl_->sem_resp);
    }

    bool recv_frame(std::string &out) override {
        if (!impl_ || !impl_->base) return false;
        for (;;) {
            uint64_t prod = impl_->req_prod->load(std::memory_order_acquire);
            uint64_t cons = impl_->req_cons->load(std::memory_order_relaxed);
            uint64_t avail = prod - cons;
            if (avail >= 4) {
                uint8_t hdr[4];
                ring_read(impl_->req_ring, impl_->req_cap, cons, hdr, 4);
                uint32_t len;
                std::memcpy(&len, hdr, 4);
                if (len > kMaxFrame || 4 + len > (prod - cons)) {
                    // Malformed frame: refuse to spin.
                    close();
                    return false;
                }
                out.resize(len);
                ring_read(impl_->req_ring, impl_->req_cap, cons + 4,
                          reinterpret_cast<uint8_t *>(&out[0]), len);
                impl_->req_cons->store(cons + 4 + len,
                                       std::memory_order_release);
                return true;
            }
            if (impl_->state->load(std::memory_order_acquire) == 2) {
                // Peer closed and no more data.
                return false;
            }
            if (!wait_sem(impl_->sem_req, -1)) {
                return false;
            }
        }
    }

    void close() override {
        if (!impl_) return;
        ShmLayout *L = impl_.get();
        impl_.reset();

        // Announce closure so the peer wakes up and sees EOF.
        if (L->base && L->state) {
            L->state->store(2, std::memory_order_release);
            post_sem(L->sem_resp);
        }

#ifdef _WIN32
        if (L->base) UnmapViewOfFile(L->base);
        if (L->map_handle) CloseHandle(L->map_handle);
        if (L->sem_req) CloseHandle(L->sem_req);
        if (L->sem_resp) CloseHandle(L->sem_resp);
        if (L->sem_conn) CloseHandle(L->sem_conn);
#else
        if (L->base) ::munmap(L->base, L->region_size);
        if (L->sem_req != SEM_FAILED) ::sem_close(L->sem_req);
        if (L->sem_resp != SEM_FAILED) ::sem_close(L->sem_resp);
        if (L->sem_conn != SEM_FAILED) ::sem_close(L->sem_conn);
        // Best-effort cleanup; tolerate other processes holding them open.
        ::shm_unlink(shm_name(token_).c_str());
        ::sem_unlink(sem_name(token_, "req").c_str());
        ::sem_unlink(sem_name(token_, "resp").c_str());
        ::sem_unlink(sem_name(token_, "conn").c_str());
#endif
    }

  private:
    std::string token_;
    std::unique_ptr<ShmLayout> impl_;
};

}  // namespace

IpcTransport *create_shm_transport() { return new ShmTransport(); }

}  // namespace ipc
}  // namespace cocotb