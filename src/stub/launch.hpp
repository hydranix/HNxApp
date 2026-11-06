// Launching the extracted application, and the cross-process extraction lock.
#pragma once

#include <expected>
#include <string>
#include <vector>

#include "error.hpp"
#include "meta.hpp"

namespace hnx {

// Exclusive lock over one target directory.
//
// A file lock rather than a named mutex on purpose: a mutex in the Local\
// namespace is per-session, so the same user in two sessions (fast user
// switching, or an RDP session alongside the console) would get two mutexes and
// two concurrent extractions into one directory. Global\ would fix that but
// needs SeCreateGlobalPrivilege, which a portable app should never require.
//
// The lock file is created once and never deleted. The OS releases the lock when
// the handle closes, including on a crash, so there is no stale-lock recovery to
// get wrong.
class ExtractionLock {
public:
    ExtractionLock() = default;
    ~ExtractionLock();

    ExtractionLock(const ExtractionLock&) = delete;
    ExtractionLock& operator=(const ExtractionLock&) = delete;
    ExtractionLock(ExtractionLock&&) noexcept;
    ExtractionLock& operator=(ExtractionLock&&) noexcept;

    // Blocks until the lock is held. A second stub launched at the same time
    // waits here, then finds the tree already complete and launches immediately.
    [[nodiscard]] static std::expected<ExtractionLock, Error> acquire(
        std::wstring_view targetDir);

    void release() noexcept;

private:
    void* handle_ = nullptr;  // HANDLE
    bool locked_ = false;
};

struct LaunchOptions {
    std::wstring exePath;      // absolute, display form (not \\?\)
    std::wstring commandLine;  // full command line including argv[0]
    std::wstring workDir;
    std::vector<EnvVar> env;
    int showWindow = 1;
    bool requireAdmin = false;
    WaitMode waitMode = WaitMode::None;
};

struct LaunchResult {
    // Named Direct/Shell rather than CreateProcess/ShellExecute because both of
    // those are preprocessor macros under UNICODE: an enumerator called
    // CreateProcess expands to CreateProcessW and fails to compile.
    enum class Path { Direct, Shell } path{};
    unsigned long exitCode = 0;
    bool waited = false;
};

// Starts the application.
//
// CreateProcessW is tried first because it is the only way to control the
// environment block and the working directory in one call, and because it
// returns a handle we can actually wait on.
//
// ShellExecuteExW is the fallback for entry points CreateProcessW cannot start.
// Measured on Windows 11 26100 with this toolchain, rather than assumed:
//
//   .exe   CreateProcessW succeeds
//   .bat   CreateProcessW succeeds -- it spawns cmd.exe itself. Contrary to
//          long-standing folklore, this does NOT fail on current Windows.
//   .txt   CreateProcessW fails, ERROR_BAD_EXE_FORMAT (193)
//   .lnk   CreateProcessW fails; only the shell resolves shortcuts
//
// So the fallback exists for shortcuts and documents. Note that a process
// started through the shell is detached from our console, so its stdout does not
// reach a terminal the stub was launched from.
[[nodiscard]] std::expected<LaunchResult, Error> launch(const LaunchOptions& opt);

// Splits our own command line and drops argv[0], returning the remainder to be
// forwarded to the child. Stub flags have already been removed by the caller.
[[nodiscard]] std::wstring forwardedArguments(std::wstring_view fullCommandLine);

// Quotes one argument per the Windows command-line parsing rules, so a path with
// spaces survives the round trip into the child.
[[nodiscard]] std::wstring quoteArgument(std::wstring_view arg);

// True when this process holds an elevated (administrator) token. Used to decide
// whether firewall rules -- which need elevation to apply to the Windows Filtering
// Platform -- require relaunching the stub elevated first.
[[nodiscard]] bool isProcessElevated();

// Relaunches this executable elevated via ShellExecuteExW with the "runas" verb,
// forwarding `args` and preserving the working directory, then waits for the new
// instance and returns its exit code. The elevated instance carries out the whole
// run, so the caller returns this straight through.
//
// Returns ElevationFailed if the user declines the UAC prompt or the relaunch
// cannot be started.
[[nodiscard]] std::expected<int, Error> relaunchElevated(std::wstring_view exePath,
                                                         std::wstring_view args);

}  // namespace hnx
