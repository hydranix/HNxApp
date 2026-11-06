#include "launch.hpp"

#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include "extract.hpp"
#include "paths.hpp"
#include "state.hpp"

namespace hnx {
namespace {

constexpr std::wstring_view kLockName = L"lock";

}  // namespace

// ---------------------------------------------------------------------------
// ExtractionLock
// ---------------------------------------------------------------------------

ExtractionLock::~ExtractionLock() { release(); }

ExtractionLock::ExtractionLock(ExtractionLock&& o) noexcept
    : handle_(std::exchange(o.handle_, nullptr)),
      locked_(std::exchange(o.locked_, false)) {}

ExtractionLock& ExtractionLock::operator=(ExtractionLock&& o) noexcept {
    if (this != &o) {
        release();
        handle_ = std::exchange(o.handle_, nullptr);
        locked_ = std::exchange(o.locked_, false);
    }
    return *this;
}

void ExtractionLock::release() noexcept {
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
        if (locked_) {
            OVERLAPPED ov{};
            ::UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &ov);
        }
        ::CloseHandle(handle_);
    }
    handle_ = nullptr;
    locked_ = false;
}

std::expected<ExtractionLock, Error> ExtractionLock::acquire(std::wstring_view targetDir) {
    const std::wstring dir = State::metaDir(targetDir);

    // The lock lives in .hnx, so that directory must exist before anything else.
    // This is the one write that happens outside the lock, and it is safe because
    // CreateDirectoryW is atomic and ERROR_ALREADY_EXISTS is success here.
    if (auto r = createDirRecursive(dir); !r) return std::unexpected(r.error());
    ::SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_HIDDEN);

    std::wstring path(dir);
    path.push_back(L'\\');
    path.append(kLockName);

    ExtractionLock lock;

    // FILE_SHARE_READ|WRITE so other instances can open the same file; exclusion
    // comes from LockFileEx, not from the open mode.
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return std::unexpected(
            Error{Ec::LockFailed, paths::narrow(paths::toDisplayForm(path)), ::GetLastError()});
    lock.handle_ = h;

    OVERLAPPED ov{};
    if (!::LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &ov))
        return std::unexpected(Error{Ec::LockFailed, "LockFileEx", ::GetLastError()});
    lock.locked_ = true;

    return lock;
}

// ---------------------------------------------------------------------------
// Command line handling
// ---------------------------------------------------------------------------

std::wstring quoteArgument(std::wstring_view arg) {
    // Only quote when necessary, so simple arguments stay readable in a process
    // listing.
    if (!arg.empty() &&
        arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
        return std::wstring(arg);

    std::wstring out;
    out.push_back(L'"');

    for (std::size_t i = 0;; ++i) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++backslashes;
        }

        if (i == arg.size()) {
            // Backslashes immediately before the closing quote must be doubled,
            // or they would escape the quote itself.
            out.append(backslashes * 2, L'\\');
            break;
        }

        if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(arg[i]);
        }
    }

    out.push_back(L'"');
    return out;
}

std::wstring forwardedArguments(std::wstring_view fullCommandLine) {
    std::size_t i = 0;
    const std::size_t n = fullCommandLine.size();

    while (i < n && (fullCommandLine[i] == L' ' || fullCommandLine[i] == L'\t')) ++i;

    // argv[0] follows its own rules: inside quotes everything is literal, and
    // backslash is NOT an escape character there.
    if (i < n && fullCommandLine[i] == L'"') {
        ++i;
        while (i < n && fullCommandLine[i] != L'"') ++i;
        if (i < n) ++i;
    } else {
        while (i < n && fullCommandLine[i] != L' ' && fullCommandLine[i] != L'\t') ++i;
    }

    while (i < n && (fullCommandLine[i] == L' ' || fullCommandLine[i] == L'\t')) ++i;

    return std::wstring(fullCommandLine.substr(i));
}

namespace {

// Builds an environment block: the current environment plus the metadata
// additions. A null environment would inherit, but then the metadata `env`
// entries could not be applied without mutating our own process environment.
[[nodiscard]] std::vector<wchar_t> buildEnvironmentBlock(const std::vector<EnvVar>& extra) {
    std::vector<wchar_t> block;

    LPWCH env = ::GetEnvironmentStringsW();
    if (env) {
        const wchar_t* p = env;
        while (*p) {
            const std::size_t len = ::wcslen(p);
            const wchar_t* eq = ::wcschr(p + 1, L'=');  // +1: drive vars start with '='

            bool overridden = false;
            if (eq) {
                const std::wstring_view name{p, static_cast<std::size_t>(eq - p)};
                for (const auto& e : extra) {
                    if (::CompareStringOrdinal(name.data(), static_cast<int>(name.size()),
                                               e.name.c_str(), static_cast<int>(e.name.size()),
                                               TRUE) == CSTR_EQUAL) {
                        overridden = true;
                        break;
                    }
                }
            }

            if (!overridden) block.insert(block.end(), p, p + len + 1);
            p += len + 1;
        }
        ::FreeEnvironmentStringsW(env);
    }

    for (const auto& e : extra) {
        block.insert(block.end(), e.name.begin(), e.name.end());
        block.push_back(L'=');
        block.insert(block.end(), e.value.begin(), e.value.end());
        block.push_back(L'\0');
    }

    block.push_back(L'\0');  // block terminator
    return block;
}

}  // namespace

std::expected<LaunchResult, Error> launch(const LaunchOptions& opt) {
    LaunchResult result;

    const DWORD attrs = ::GetFileAttributesW(paths::toExtendedForm(opt.exePath).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return std::unexpected(Error{Ec::ExecMissing, paths::narrow(opt.exePath)});

    // requireAdmin can only be honoured by the shell: CreateProcessW has no way
    // to request elevation.
    const bool shellOnly = opt.requireAdmin;

    if (!shellOnly) {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = static_cast<WORD>(opt.showWindow);

        PROCESS_INFORMATION pi{};

        std::wstring cmdline = opt.commandLine;  // CreateProcessW may modify this
        auto envBlock = buildEnvironmentBlock(opt.env);

        const BOOL ok = ::CreateProcessW(
            opt.exePath.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
            CREATE_UNICODE_ENVIRONMENT, envBlock.data(),
            opt.workDir.empty() ? nullptr : opt.workDir.c_str(), &si, &pi);

        if (ok) {
            result.path = LaunchResult::Path::Direct;
            ::CloseHandle(pi.hThread);

            if (opt.waitMode != WaitMode::None) {
                ::WaitForSingleObject(pi.hProcess, INFINITE);
                DWORD code = 0;
                ::GetExitCodeProcess(pi.hProcess, &code);
                result.exitCode = code;
                result.waited = true;
            }
            ::CloseHandle(pi.hProcess);
            return result;
        }

        const DWORD err = ::GetLastError();

        // These are the failures the shell can actually resolve: a .lnk shortcut
        // or a document with an associated handler. Anything else (a sharing
        // violation, a corrupt image) would fail identically through the shell,
        // so report the original error rather than retrying and replacing it with
        // a vaguer one.
        //
        // ERROR_BAD_EXE_FORMAT is the one that matters here; .bat does not reach
        // this path on current Windows because CreateProcessW handles it.
        const bool shellCanHelp = err == ERROR_BAD_EXE_FORMAT ||
                                  err == ERROR_ACCESS_DENIED ||
                                  err == ERROR_ELEVATION_REQUIRED ||
                                  err == ERROR_FILE_NOT_FOUND;
        if (!shellCanHelp)
            return std::unexpected(Error{Ec::LaunchFailed, paths::narrow(opt.exePath), err});
    }

    // --- ShellExecuteExW fallback -------------------------------------------
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    sei.lpVerb = opt.requireAdmin ? L"runas" : nullptr;
    sei.lpFile = opt.exePath.c_str();

    const std::wstring shellArgs = forwardedArguments(opt.commandLine);
    sei.lpParameters = shellArgs.empty() ? nullptr : shellArgs.c_str();
    sei.lpDirectory = opt.workDir.empty() ? nullptr : opt.workDir.c_str();
    sei.nShow = opt.showWindow;

    if (!::ShellExecuteExW(&sei))
        return std::unexpected(
            Error{Ec::LaunchFailed, paths::narrow(opt.exePath), ::GetLastError()});

    result.path = LaunchResult::Path::Shell;

    if (sei.hProcess) {
        if (opt.waitMode != WaitMode::None) {
            ::WaitForSingleObject(sei.hProcess, INFINITE);
            DWORD code = 0;
            ::GetExitCodeProcess(sei.hProcess, &code);
            result.exitCode = code;
            result.waited = true;
        }
        ::CloseHandle(sei.hProcess);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Elevation
// ---------------------------------------------------------------------------

bool isProcessElevated() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL ok = ::GetTokenInformation(token, TokenElevation, &elevation,
                                          sizeof(elevation), &size);
    ::CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

std::expected<int, Error> relaunchElevated(std::wstring_view exePath,
                                           std::wstring_view args) {
    const std::wstring file(exePath);
    const std::wstring params(args);

    // Preserve the working directory, which "runas" would otherwise reset to
    // system32 -- an explicit --hnx-target= is resolved against it.
    std::wstring cwd(32768, L'\0');
    const DWORD n = ::GetCurrentDirectoryW(static_cast<DWORD>(cwd.size()), cwd.data());
    if (n > 0 && n < cwd.size())
        cwd.resize(n);
    else
        cwd.clear();

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = file.c_str();
    sei.lpParameters = params.empty() ? nullptr : params.c_str();
    sei.lpDirectory = cwd.empty() ? nullptr : cwd.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (!::ShellExecuteExW(&sei))
        return std::unexpected(
            Error{Ec::ElevationFailed, "ShellExecuteExW runas", ::GetLastError()});

    int exitCode = 0;
    if (sei.hProcess) {
        ::WaitForSingleObject(sei.hProcess, INFINITE);
        DWORD code = 0;
        ::GetExitCodeProcess(sei.hProcess, &code);
        exitCode = static_cast<int>(code);
        ::CloseHandle(sei.hProcess);
    }
    return exitCode;
}

}  // namespace hnx
