// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Access to the cocotb package zip embedded in this shared library and
// its publication as a file the spawned Python child can put on
// PYTHONPATH (see tools/make_ipc_zip.py and the COCOTB_IPC_EMBED_ZIP
// build option).
//
// zipimport only imports from real filesystem paths, so the embedded
// bytes are materialized as a flat file in the system temp directory
// (no subdirectories):
//
//     <temp>/cocotb-ipc-<hash>.zip       (Linux: cocotb-ipc-<uid>-<hash>.zip)
//
// where <hash> is the FNV-1a-64 hash of the embedded content:
//
//   * content addressing keeps concurrent cocotb versions from fighting
//     over one file name,
//   * a file found in place is fully re-read and re-hashed before
//     reuse, so truncated or tampered content is replaced transparently,
//   * files are created exclusively (O_CREAT|O_EXCL resp. CREATE_NEW,
//     mode 0600) and published with an atomic rename (resp.
//     MoveFileEx), so a concurrent child never observes partial
//     content; if the rename loses a Windows sharing violation, the
//     verified temporary file is used directly instead,
//   * cache files untouched for 30 days (other cocotb versions, leftover
//     temporaries) are removed opportunistically after a rewrite; the OS
//     cleans the temp directory anyway.
//
// An empty return value means "no injection": the library was built
// without an embedded zip (the default for wheels), COCOTB_IPC_EMBED is
// "never", or publication failed. The child then resolves cocotb from
// its environment exactly as an installed-cocotb setup does.

#include "./ipc_priv.hpp"

#include <cstdio>   // rename
#include <cstdlib>   // getenv
#include <cstring>   // strcmp
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>   // open, O_*
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>  // getpid, getuid, read, unlink
#include <ctime>     // time
#endif

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#if defined(COCOTB_HAVE_EMBEDDED_ZIP) && !defined(_WIN32)
extern "C" {
// Defined by the generated assembly (cocotb_ipc_zip.s) baked into this
// library; resolved at static link time.
extern const unsigned char cocotb_zip_start[];
extern const unsigned char cocotb_zip_end[];
}  // extern "C"
#endif

namespace {

#if defined(COCOTB_HAVE_EMBEDDED_ZIP)

// Remove cache files we do not use once they are this old.
const long kStaleAgeSeconds = 30L * 24 * 60 * 60;

uint64_t fnv1a64(const void *data, size_t len) {
    const unsigned char *bytes = static_cast<const unsigned char *>(data);
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < len; ++i) {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

// Lowercase, zero-padded, fixed-width 16 hex digits.
std::string hash_hex(uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (size_t i = 0; i < 16; ++i) {
        out[15 - i] = digits[value & 0xf];
        value >>= 4;
    }
    return out;
}

#if defined(_WIN32)

using PathString = std::wstring;

// The zip as an RCDATA resource of this DLL, or nullptr when absent.
const void *zip_payload(size_t *size) {
    HMODULE module = nullptr;
    const void *self = reinterpret_cast<const void *>(
        reinterpret_cast<void *>(&zip_payload));
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(self), &module)) {
        return nullptr;
    }
    // MAKEINTRESOURCEW(10) is RT_RCDATA in its wide form; RT_RCDATA itself
    // only expands to a wide value when the build defines UNICODE.
    HRSRC resource =
        FindResourceW(module, L"IDR_COCOTBIPCZIP", MAKEINTRESOURCEW(10));
    if (resource == nullptr) {
        return nullptr;
    }
    HGLOBAL loaded = LoadResource(module, resource);
    if (loaded == nullptr) {
        return nullptr;
    }
    const void *data = LockResource(loaded);
    if (data == nullptr) {
        return nullptr;
    }
    *size = static_cast<size_t>(SizeofResource(module, resource));
    return data;
}

std::string wide_to_utf8(const std::wstring &text) {
    if (text.empty()) {
        return std::string();
    }
    int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                     static_cast<int>(text.size()), nullptr, 0,
                                     nullptr, nullptr);
    if (needed <= 0) {
        return std::string();
    }
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                        static_cast<int>(text.size()), &out[0], needed,
                        nullptr, nullptr);
    return out;
}

// API/logging form of a cache path (gpi_log takes narrow strings).
std::string path_to_utf8(const PathString &path) {
    return wide_to_utf8(path);
}

// The temp directory with a trailing separator (GetTempPathW always
// provides one; the join below copes either way).
std::wstring temp_directory() {
    DWORD needed = GetTempPathW(0, nullptr);
    if (needed == 0) {
        return std::wstring();
    }
    std::vector<wchar_t> buffer(needed);
    DWORD written = GetTempPathW(needed, &buffer[0]);
    if (written == 0 || written >= needed) {
        return std::wstring();
    }
    return std::wstring(&buffer[0], written);
}

PathString cache_path(uint64_t hash) {
    std::wstring dir = temp_directory();
    if (dir.empty()) {
        return PathString();
    }
    if (dir.back() != L'\\' && dir.back() != L'/') {
        dir += L'\\';
    }
    // The temp directory is per user on Windows: no uid in the name.
    std::wstring path = dir + L"cocotb-ipc-";
    const std::string hex = hash_hex(hash);
    for (size_t i = 0; i < hex.size(); ++i) {
        path += static_cast<wchar_t>(hex[i]);
    }
    path += L".zip";
    return path;
}

// True when `path` is a regular, non-reparse-point file of exactly the
// embedded bytes (size check first, then a full re-hash against
// `expected`, the hash of the embedded payload).
bool file_matches(const PathString &path, size_t len, uint64_t expected) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    if (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        return false;
    }
    HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool matches = false;
    LARGE_INTEGER size;
    if (GetFileSizeEx(handle, &size) &&
        static_cast<uint64_t>(size.QuadPart) == static_cast<uint64_t>(len)) {
        std::vector<char> buffer(len);
        size_t offset = 0;
        while (offset < len) {
            DWORD got = 0;
            if (!ReadFile(handle, &buffer[offset],
                          static_cast<DWORD>(len - offset), &got, nullptr) ||
                got == 0) {
                break;
            }
            offset += static_cast<size_t>(got);
        }
        matches = offset == len && fnv1a64(&buffer[0], len) == expected;
    }
    CloseHandle(handle);
    return matches;
}

// Write `data` to a private temporary file and publish it at `path`.
// Returns the path holding verified content, or empty on failure.
PathString publish(const PathString &path, const void *data, size_t len,
                   uint64_t expected) {
    std::wstring tmp = path + L".tmp." +
                       std::to_wstring(GetCurrentProcessId());
    auto write_all = [data, len](HANDLE handle) -> bool {
        const char *bytes = static_cast<const char *>(data);
        size_t offset = 0;
        while (offset < len) {
            DWORD written = 0;
            if (!WriteFile(handle, bytes + offset,
                           static_cast<DWORD>(len - offset), &written,
                           nullptr) ||
                written == 0) {
                return false;
            }
            offset += static_cast<size_t>(written);
        }
        return true;
    };

    HANDLE handle = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        // A leftover from a run with a recycled pid: clear and retry.
        DeleteFileW(tmp.c_str());
        handle = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return PathString();
        }
    }
    bool ok = write_all(handle);
    CloseHandle(handle);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return PathString();
    }

    if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());  // no-op; kept for clarity
        return path;
    }
    // Typical cause: a child has `path` open without share-delete. Use
    // our verified copy when `path` itself is not already valid.
    if (file_matches(path, len, expected)) {
        DeleteFileW(tmp.c_str());
        return path;
    }
    return tmp;
}

// Opportunistically drop cache files this run does not use and that
// have not been touched in kStaleAgeSeconds. Failures are ignored.
void gc_stale_caches(const PathString &keep) {
    std::wstring dir = temp_directory();
    if (dir.empty()) {
        return;
    }
    if (dir.back() != L'\\' && dir.back() != L'/') {
        dir += L'\\';
    }
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW((dir + L"cocotb-ipc-*").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    ULARGE_INTEGER now;
    FILETIME now_filetime;
    GetSystemTimeAsFileTime(&now_filetime);
    now.LowPart = now_filetime.dwLowDateTime;
    now.HighPart = now_filetime.dwHighDateTime;
    const ULONGLONG stale_age =
        static_cast<ULONGLONG>(kStaleAgeSeconds) * 10000000ULL;
    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        std::wstring full = dir + entry.cFileName;
        if (full == keep) {
            continue;
        }
        ULARGE_INTEGER written;
        written.LowPart = entry.ftLastWriteTime.dwLowDateTime;
        written.HighPart = entry.ftLastWriteTime.dwHighDateTime;
        if (now.QuadPart < written.QuadPart ||
            now.QuadPart - written.QuadPart < stale_age) {
            continue;
        }
        DeleteFileW(full.c_str());
    } while (FindNextFileW(find, &entry));
    FindClose(find);
}

#else  // POSIX

using PathString = std::string;

// The zip baked into this shared library, or nullptr when absent.
const void *zip_payload(size_t *size) {
    *size = static_cast<size_t>(cocotb_zip_end - cocotb_zip_start);
    return cocotb_zip_start;
}

std::string temp_directory() {
    const char *from_env = getenv("TMPDIR");
    if (from_env == nullptr || from_env[0] == '\0') {
        from_env = "/tmp";
    }
    std::string dir(from_env);
    while (dir.size() > 1 && dir.back() == '/') {
        dir.erase(dir.size() - 1);
    }
    return dir;
}

// API/logging form of a cache path (byte strings on POSIX).
std::string path_to_utf8(const PathString &path) {
    return path;
}

PathString cache_path(uint64_t hash) {
    std::string dir = temp_directory();
    if (dir.empty()) {
        return PathString();
    }
    // /tmp is shared between users: include the uid. On macOS TMPDIR is
    // already per user; only the shared /tmp fallback needs it.
#if defined(__APPLE__)
    bool shared = (dir == "/tmp");
#else
    bool shared = true;
#endif
    std::string path = dir + "/cocotb-ipc-";
    if (shared) {
        path += std::to_string(static_cast<long long>(getuid()));
        path += "-";
    }
    path += hash_hex(hash);
    path += ".zip";
    return path;
}

// True when `path` is a regular file of exactly the embedded bytes.
// O_NOFOLLOW refuses to open symlinks planted by someone else.
bool file_matches(const PathString &path, size_t len, uint64_t expected) {
    int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        return false;
    }
    bool matches = false;
    struct stat info;
    if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
        static_cast<uint64_t>(info.st_size) == static_cast<uint64_t>(len)) {
        std::vector<char> buffer(len);
        size_t offset = 0;
        while (offset < len) {
            ssize_t got = read(fd, &buffer[offset], len - offset);
            if (got <= 0) {
                break;
            }
            offset += static_cast<size_t>(got);
        }
        matches = offset == len && fnv1a64(&buffer[0], len) == expected;
    }
    close(fd);
    return matches;
}

// Write `data` to a private temporary file and publish it at `path`.
// Returns the path holding verified content, or empty on failure.
PathString publish(const PathString &path, const void *data, size_t len,
                   uint64_t expected) {
    std::string tmp =
        path + ".tmp." + std::to_string(static_cast<long long>(getpid()));
    const int flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW;
    int fd = open(tmp.c_str(), flags, 0600);
    if (fd < 0) {
        // A leftover from a run with a recycled pid: clear and retry.
        unlink(tmp.c_str());
        fd = open(tmp.c_str(), flags, 0600);
        if (fd < 0) {
            return PathString();
        }
    }
    const char *bytes = static_cast<const char *>(data);
    size_t offset = 0;
    bool ok = true;
    while (offset < len) {
        ssize_t written = write(fd, bytes + offset, len - offset);
        if (written <= 0) {
            ok = false;
            break;
        }
        offset += static_cast<size_t>(written);
    }
    close(fd);
    if (!ok) {
        unlink(tmp.c_str());
        return PathString();
    }

    // rename() atomically replaces whatever is at `path`, including a
    // symlink (the link itself, not its target).
    if (rename(tmp.c_str(), path.c_str()) == 0) {
        return path;
    }
    if (file_matches(path, len, expected)) {
        unlink(tmp.c_str());
        return path;
    }
    return tmp;
}

// Opportunistically drop cache files this run does not use and that
// have not been touched in kStaleAgeSeconds. Failures are ignored.
void gc_stale_caches(const PathString &keep) {
    std::string dir = temp_directory();
    DIR *handle = opendir(dir.c_str());
    if (handle == nullptr) {
        return;
    }
    const time_t now = time(nullptr);
    const std::string prefix = "cocotb-ipc-";
    struct dirent *entry = nullptr;
    while ((entry = readdir(handle)) != nullptr) {
        const std::string name(entry->d_name);
        if (name.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        const std::string full = dir + "/" + name;
        if (full == keep) {
            continue;
        }
        struct stat info;
        if (stat(full.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
            continue;
        }
        if (now >= info.st_mtime &&
            now - info.st_mtime < kStaleAgeSeconds) {
            continue;
        }
        unlink(full.c_str());
    }
    closedir(handle);
}

#endif  // _WIN32

#endif  // COCOTB_HAVE_EMBEDDED_ZIP

}  // namespace

namespace cocotb {
namespace ipc {

std::string embedded_zip_path() {
#if !defined(COCOTB_HAVE_EMBEDDED_ZIP)
    return std::string();
#else
    const char *mode = getenv("COCOTB_IPC_EMBED");
    if (mode != nullptr && std::strcmp(mode, "never") == 0) {
        return std::string();
    }

    size_t len = 0;
    const void *data = zip_payload(&len);
    if (data == nullptr || len == 0) {
        return std::string();
    }
    const uint64_t expected = fnv1a64(data, len);

    const PathString path = cache_path(expected);
    if (path.empty()) {
        IPC_LOG_WARN("Cannot locate a temp directory for the embedded "
                     "cocotb package zip");
        return std::string();
    }
    if (file_matches(path, len, expected)) {
        const std::string result = path_to_utf8(path);
        IPC_LOG_DEBUG("Embedded cocotb package zip: %s", result.c_str());
        return result;
    }

    const PathString published = publish(path, data, len, expected);
    if (published.empty()) {
        IPC_LOG_WARN("Could not materialize the embedded cocotb package "
                     "zip in the temp directory");
        return std::string();
    }
    gc_stale_caches(published);
    const std::string result = path_to_utf8(published);
    IPC_LOG_INFO("Embedded cocotb package zip: %s", result.c_str());
    return result;
#endif
}

}  // namespace ipc
}  // namespace cocotb
