#include "app/system_info.h"

#include <cctype>
#include <fstream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

namespace app {

namespace {
std::string trimmed(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t first = 0;
    while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) ++first;
    return s.substr(first);
}
} // namespace

std::string processor_name() {
    static const std::string name = [] {
        std::string out;
#if defined(_WIN32)
        char buf[256] = {};
        DWORD size = sizeof buf;
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                         RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            out = buf;
#elif defined(__APPLE__)
        char buf[256] = {};
        size_t size = sizeof buf;
        if (sysctlbyname("machdep.cpu.brand_string", buf, &size, nullptr, 0) == 0) out = buf;
#else
        std::ifstream info("/proc/cpuinfo");
        for (std::string line; std::getline(info, line);)
            if (line.rfind("model name", 0) == 0) {
                if (const auto colon = line.find(':'); colon != std::string::npos) out = line.substr(colon + 1);
                break;
            }
#endif
        return trimmed(out);
    }();
    return name;
}

bool restart_self(int argc, char **argv) {
#if defined(_WIN32)
    (void)argc, (void)argv;
    wchar_t exe[MAX_PATH * 4];
    if (!GetModuleFileNameW(nullptr, exe, DWORD(sizeof exe / sizeof exe[0]))) return false;
    std::wstring line = GetCommandLineW(); // CreateProcessW may write into it
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    (void)argc;
    std::string exe;
#if defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) exe = buf;
#else
    exe = "/proc/self/exe"; // the running file itself (inside an AppImage too: its runtime waits for this process)
#endif
    if (exe.empty()) return false;
    execv(exe.c_str(), argv);
    return false; // only when execv failed
#endif
}

} // namespace app
