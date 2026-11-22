#pragma once

// rt::CheckedTraceFile — exclusive diagnostic trace create plus a checked close.
//
// Open is C11/MSVC fopen mode "wbx" only. An existing path fails the
// constructor and is not truncated. There is no "wb" overwrite fallback.
//
// stdio buffering stays at the library default (no setvbuf). A checked writer
// flushes at each vblank; fclose from close() or the destructor flushes the
// stdio tail. Successful fclose still does not fsync.
//
// Known limitations:
// - Open failures share one exception. Exists, permissions, and a lost race
//   with another creator are not separate error codes.
// - Paths are narrow strings as fopen accepts them. The object is not
//   thread-safe. get() is borrowed and is null after close; the FILE* must
//   not be used again.
// - close() latches the first result. A false return does not abort. Ignoring
//   it leaves a closed handle and a trace that may be short of the last
//   buffer. The destructor aborts only when it itself closes and that close
//   fails, after a concise stderr line. abort() does not unwind.
// - CloseFn replaces fclose and must release the FILE exactly once (a failure
//   stub should close the underlying FILE, then return nonzero). It must not
//   throw. Null CloseFn selects fclose.
// - No durability past a successful fclose (no FlushFileBuffers / fsync).

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace rt {

class CheckedTraceFile {
public:
    using CloseFn = int (*)(FILE*);

    explicit CheckedTraceFile(const char* path, CloseFn close_fn = nullptr)
        : close_fn_(close_fn != nullptr ? close_fn : &fclose_stdio) {
        if (path == nullptr || path[0] == '\0') {
            throw std::invalid_argument(
                "rt::CheckedTraceFile: null or empty path");
        }
        path_.assign(path);
        errno = 0;
        // Exclusive binary create. "x" is C11; MSVC accepts it on "w".
        // Do not retry with "wb": that would truncate an existing trace.
        file_ = std::fopen(path_.c_str(), "wbx");
        if (file_ == nullptr) {
            const int err = errno;
            throw std::runtime_error(
                "rt::CheckedTraceFile: exclusive create failed for '" + path_ +
                "': " + std::generic_category().message(err));
        }
    }

    CheckedTraceFile(const CheckedTraceFile&) = delete;
    CheckedTraceFile& operator=(const CheckedTraceFile&) = delete;
    CheckedTraceFile(CheckedTraceFile&&) = delete;
    CheckedTraceFile& operator=(CheckedTraceFile&&) = delete;

    ~CheckedTraceFile() noexcept {
        if (!closed_) {
            if (!close()) {
                std::fprintf(
                    stderr,
                    "rt::CheckedTraceFile: close failed for '%s'\n",
                    path_.c_str());
                std::fflush(stderr);
                std::abort();
            }
        }
    }

    // Borrowed. Null once close has been attempted. Never own or fclose this.
    FILE* get() noexcept { return file_; }

    // Closes at most once. Later calls return the latched result and do not
    // touch the handle. False means CloseFn returned nonzero.
    bool close() noexcept {
        if (closed_) {
            return close_ok_;
        }
        closed_ = true;
        FILE* handle = file_;
        file_ = nullptr;
        if (handle == nullptr || close_fn_ == nullptr) {
            close_ok_ = false;
            return false;
        }
        close_ok_ = (close_fn_(handle) == 0);
        return close_ok_;
    }

private:
    static int fclose_stdio(FILE* handle) { return std::fclose(handle); }

    FILE* file_ = nullptr;
    CloseFn close_fn_ = nullptr;
    std::string path_;
    bool closed_ = false;
    bool close_ok_ = true;
};

}  // namespace rt
