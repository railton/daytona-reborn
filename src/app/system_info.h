// What a log (and the image quality window) says of the computer besides
// SDL's own: the processor's name. Read from the system once: the registry
// on Windows, /proc/cpuinfo on Linux, sysctl on macOS; empty when unknown.
#pragma once

#include <string>

namespace app {

std::string processor_name();

// Starts this program again with the same arguments, in the same working
// folder: Windows a new process (this one then ends), elsewhere this one
// replaced (execv of the running executable). Call after the window and the
// devices are released. False when it could not.
bool restart_self(int argc, char **argv);

} // namespace app
