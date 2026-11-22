// Standalone tests for rt::CheckedTraceFile.
// Temp directories are unique under the filesystem temp dir and removed here,
// except the destructor-failure child, whose directory is removed by the
// CTest Python wrapper because abort() skips destructors.
// No global FILE*, no environment variables.
//
// Limitations covered by tests: exclusive create does not truncate; close
// failure latches without a second CloseFn call; ordinary destruction keeps
// bytes written under default buffering; destructor close failure aborts.

#include "runtime/checked_trace_file.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_close_calls = 0;

void expect(bool cond, const char* what) {
    if (!cond) {
        std::cerr << "FAIL: " << what << '\n';
        ++g_failures;
    }
}

class TempDir {
public:
    TempDir() {
        const fs::path root = fs::temp_directory_path();
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        for (int i = 0; i < 1000; ++i) {
            path_ = root / ("checked_trace_file_" + std::to_string(stamp) +
                            "_" + std::to_string(i));
            if (fs::create_directory(path_)) {
                return;
            }
        }
        throw std::runtime_error("could not create test temp directory");
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    fs::path file(const char* name) const { return path_ / name; }

private:
    fs::path path_;
};

std::string read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("could not read " + path.string());
    }
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

bool threw_on_open(const fs::path& path) {
    try {
        rt::CheckedTraceFile second(path.string().c_str());
        (void)second;
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

// Closes the underlying FILE, then reports failure. One call, no leak.
int fail_close(FILE* handle) {
    ++g_close_calls;
    if (handle != nullptr) {
        std::fclose(handle);
    }
    return EOF;
}

int run_destructor_close_failure(const char* path) {
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
#endif
#if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    if (path == nullptr || path[0] == '\0') {
        return 0;
    }
    {
        rt::CheckedTraceFile trace(path, &fail_close);
        const char tail[] = "tail";
        if (std::fwrite(tail, 1, sizeof(tail) - 1, trace.get()) !=
            sizeof(tail) - 1) {
            return 0;
        }
    }
    // Surviving the destructor means close-failure did not abort.
    return 0;
}

void test_reject_null_and_empty() {
    try {
        rt::CheckedTraceFile trace(nullptr);
        (void)trace;
        expect(false, "null path must throw");
    } catch (const std::invalid_argument&) {
        expect(true, "null path");
    } catch (const std::exception&) {
        expect(false, "null path threw the wrong type");
    }

    try {
        rt::CheckedTraceFile trace("");
        (void)trace;
        expect(false, "empty path must throw");
    } catch (const std::invalid_argument&) {
        expect(true, "empty path");
    } catch (const std::exception&) {
        expect(false, "empty path threw the wrong type");
    }
}

void test_exclusive_create_preserves_bytes() {
    TempDir dir;
    const fs::path path = dir.file("trace.bin");
    const std::string payload = "first-owner-bytes";

    {
        rt::CheckedTraceFile first(path.string().c_str());
        expect(first.get() != nullptr, "owner get() while open");
        const std::size_t n = std::fwrite(
            payload.data(), 1, payload.size(), first.get());
        expect(n == payload.size(), "owner write");
        expect(std::fflush(first.get()) == 0, "owner flush");

        expect(threw_on_open(path), "second open of live path throws");
        expect(read_bytes(path) == payload,
               "live second open left contents intact");

        expect(first.close(), "explicit successful close");
        expect(first.get() == nullptr, "get() null after close");
        expect(first.close(), "repeated successful close stays latched");
        expect(first.get() == nullptr, "get() still null after repeat close");
    }

    expect(read_bytes(path) == payload, "contents intact after owner close");
    expect(threw_on_open(path),
           "reopen of existing path after close is rejected");
    expect(read_bytes(path) == payload,
           "rejected reopen did not truncate");
}

void test_close_failure_latches_once() {
    TempDir dir;
    const fs::path path = dir.file("fail.bin");
    g_close_calls = 0;
    {
        rt::CheckedTraceFile trace(path.string().c_str(), &fail_close);
        const char bytes[] = "partial";
        expect(std::fwrite(bytes, 1, sizeof(bytes) - 1, trace.get()) ==
                   sizeof(bytes) - 1,
               "write before failing close");
        expect(!trace.close(), "failing close returns false");
        expect(trace.get() == nullptr, "handle dropped after failed close");
        expect(!trace.close(), "repeated close preserves failure");
        expect(g_close_calls == 1, "CloseFn invoked once");
    }
    expect(g_close_calls == 1, "destructor did not close again");
    expect(read_bytes(path) == "partial", "bytes from failing close kept");
}

void test_destructor_preserves_unflushed_tail() {
    TempDir dir;
    const fs::path path = dir.file("tail.bin");
    const std::string payload = "ordinary-destructor-tail";
    {
        rt::CheckedTraceFile trace(path.string().c_str());
        const std::size_t n = std::fwrite(
            payload.data(), 1, payload.size(), trace.get());
        expect(n == payload.size(), "write without explicit flush");
        // No fflush: default buffering, destructor fclose must keep the tail.
    }
    expect(read_bytes(path) == payload,
           "ordinary destruction preserved file contents");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 &&
        std::strcmp(argv[1], "--destructor-close-failure") == 0) {
        const char* path = argc >= 3 ? argv[2] : nullptr;
        return run_destructor_close_failure(path);
    }

    try {
        test_reject_null_and_empty();
        test_exclusive_create_preserves_bytes();
        test_close_failure_latches_once();
        test_destructor_preserves_unflushed_tail();
    } catch (const std::exception& ex) {
        std::cerr << "FAIL: unexpected exception: " << ex.what() << '\n';
        return 1;
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "checked_trace_file ok\n";
    return 0;
}
