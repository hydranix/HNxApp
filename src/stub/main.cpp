// HNx portable application stub.
//
// Locates the archive appended to its own executable, unpacks only what is
// missing or stale into a cached target directory, then launches the packaged
// application.
//
// The sequence is ordered so that the warm path -- everything already extracted,
// which is the overwhelmingly common case -- does the least possible work: read a
// 64-byte footer, decompress a small TOC, read one state file, stat the tree in
// parallel, launch. No window is created, no block is decompressed, and nothing
// is written.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "error.hpp"
#include "extract.hpp"
#include "firewall.hpp"
#include "hash.hpp"
#include "launch.hpp"
#include "meta.hpp"
#include "paths.hpp"
#include "selfmap.hpp"
#include "state.hpp"
#include "toc.hpp"
#include "ui.hpp"
#include "verify.hpp"
#include "winpe.hpp"

namespace hnx {
namespace {

// Owns a HANDLE for the lifetime of a scope. Used for the singleInstance mutex,
// which must stay held while the stub lives rather than being closed at the end
// of the block that created it.
class HandleGuard {
public:
    HandleGuard() = default;
    explicit HandleGuard(HANDLE h) : h_(h) {}
    ~HandleGuard() { reset(nullptr); }

    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;

    void reset(HANDLE h) {
        if (h_ && h_ != INVALID_HANDLE_VALUE) ::CloseHandle(h_);
        h_ = h;
    }

private:
    HANDLE h_ = nullptr;
};

struct StubFlags {
    bool info = false;
    bool extractOnly = false;
    bool clean = false;
    bool help = false;
    bool haveVerify = false;
    VerifyMode verify = VerifyMode::Normal;
    bool haveTarget = false;
    std::wstring target;
};

[[nodiscard]] bool startsWithFold(std::wstring_view s, std::wstring_view prefix) {
    if (s.size() < prefix.size()) return false;
    return ::CompareStringOrdinal(s.data(), static_cast<int>(prefix.size()), prefix.data(),
                                  static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

// Splits the tail of our command line, pulling out --hnx-* flags and leaving the
// rest to be forwarded to the child.
//
// The reserved prefix exists so a packaged application can define whatever flags
// it likes without colliding with ours, and allowStubFlags lets a package turn
// the mechanism off entirely.
[[nodiscard]] std::wstring parseStubFlags(std::wstring_view tail, bool allow,
                                          StubFlags& out) {
    if (!allow) return std::wstring(tail);

    int argc = 0;
    // CommandLineToArgvW needs a full command line; give it a dummy argv[0] so
    // the tail is parsed as arguments rather than as a program name.
    std::wstring synthetic = L"x ";
    synthetic.append(tail);

    LPWSTR* argv = ::CommandLineToArgvW(synthetic.c_str(), &argc);
    if (!argv) return std::wstring(tail);

    std::wstring passthrough;
    for (int i = 1; i < argc; ++i) {
        std::wstring_view a{argv[i]};

        if (!startsWithFold(a, L"--hnx-")) {
            if (!passthrough.empty()) passthrough.push_back(L' ');
            passthrough.append(quoteArgument(a));
            continue;
        }

        if (a == L"--hnx-info") {
            out.info = true;
        } else if (a == L"--hnx-extract-only") {
            out.extractOnly = true;
        } else if (a == L"--hnx-clean") {
            out.clean = true;
        } else if (a == L"--hnx-help") {
            out.help = true;
        } else if (startsWithFold(a, L"--hnx-verify=")) {
            const std::wstring_view v = a.substr(13);
            out.haveVerify = true;
            if (v == L"fast") out.verify = VerifyMode::Fast;
            else if (v == L"normal") out.verify = VerifyMode::Normal;
            else if (v == L"paranoid") out.verify = VerifyMode::Paranoid;
            else out.haveVerify = false;
        } else if (startsWithFold(a, L"--hnx-target=")) {
            out.haveTarget = true;
            out.target = a.substr(13);
        }
        // Unknown --hnx-* flags are swallowed rather than forwarded, so a typo
        // does not silently reach the child as an argument it cannot parse.
    }

    ::LocalFree(argv);
    return passthrough;
}

void printHelp(const Metadata& meta) {
    std::wstring s;
    s.append(meta.appName.empty() ? meta.appId : meta.appName);
    if (!meta.appVersion.empty()) {
        s.append(L" ");
        s.append(meta.appVersion);
    }
    s.append(L"\r\n\r\nPackaged with HNx. Stub options:\r\n"
             L"  --hnx-info            show payload details and exit\r\n"
             L"  --hnx-extract-only    unpack without launching\r\n"
             L"  --hnx-verify=MODE     fast | normal | paranoid\r\n"
             L"  --hnx-target=DIR      unpack somewhere else\r\n"
             L"  --hnx-clean           remove files not present in the payload\r\n"
             L"  --hnx-help            this text\r\n"
             L"\r\nAll other arguments are passed to the application.");
    ui::writeConsoleLine(s);
    if (!ui::hasConsole())
        ::MessageBoxW(nullptr, s.c_str(), L"HNx", MB_OK | MB_ICONINFORMATION);
}

void printInfo(const SelfImage& img, const Metadata& meta, const Toc& toc,
               std::wstring_view targetDir) {
    wchar_t buf[512];
    std::wstring s;

    s.append(L"appId       : ").append(meta.appId).append(L"\r\n");
    s.append(L"appName     : ").append(meta.appName).append(L"\r\n");
    s.append(L"appVersion  : ").append(meta.appVersion).append(L"\r\n");
    s.append(L"exec        : ").append(meta.exec).append(L"\r\n");
    s.append(L"target      : ").append(targetDir).append(L"\r\n");

    const Footer& f = img.footer();
    ::wsprintfW(buf, L"buildId     : %08X%08X\r\n",
                static_cast<unsigned>(f.buildId >> 32),
                static_cast<unsigned>(f.buildId & 0xFFFFFFFFu));
    s.append(buf);
    ::wsprintfW(buf, L"archive     : offset %u, %u MB\r\n",
                static_cast<unsigned>(f.archiveOffset),
                static_cast<unsigned>(f.archiveSize / (1024 * 1024)));
    s.append(buf);
    ::wsprintfW(buf, L"contents    : %u files, %u dirs, %u blocks, %u MB raw\r\n",
                static_cast<unsigned>(toc.fileCount()),
                static_cast<unsigned>(toc.dirCount()),
                static_cast<unsigned>(toc.blocks().size()),
                static_cast<unsigned>(toc.totalRawBytes() / (1024 * 1024)));
    s.append(buf);

    ui::writeConsoleLine(s);
    if (!ui::hasConsole())
        ::MessageBoxW(nullptr, s.c_str(), L"HNx payload", MB_OK | MB_ICONINFORMATION);
}

[[nodiscard]] unsigned workerCount() {
    const unsigned hw = std::thread::hardware_concurrency();
    // Capped because this is I/O bound past a handful of threads: more workers
    // mostly add seek contention on a spinning disk and buy nothing on an SSD.
    return std::clamp(hw == 0 ? 2u : hw, 1u, 8u);
}

// The real work, returning an error rather than reporting it, so that every
// failure path surfaces through one place in wWinMain.
[[nodiscard]] std::expected<int, Error> run() {
    ui::attachParentConsole();

    auto image = SelfImage::open();
    if (!image) return std::unexpected(image.error());

    auto meta = Metadata::loadFromResource();
    if (!meta) return std::unexpected(meta.error());

    StubFlags flags;
    const std::wstring tail = forwardedArguments(::GetCommandLineW());
    const std::wstring childArgs = parseStubFlags(tail, meta->allowStubFlags, flags);

    if (flags.help) {
        printHelp(*meta);
        return 0;
    }

    const bool winPe = winpe::active();

    // Firewall rules modify the Windows Filtering Platform, which requires an
    // elevated process. When the package asks for them and we are not elevated,
    // relaunch the stub elevated and let that instance carry out the whole run, so
    // the work is never done twice. Skipped under WinPE (no firewall service, and
    // everything already runs as SYSTEM) and for operations that never launch the
    // application.
    const bool willLaunch = !flags.info && !flags.extractOnly;
    const bool applyFirewall = willLaunch && meta->firewall.active() && !winPe;
    if (applyFirewall && !isProcessElevated())
        return relaunchElevated(image->exePath(), tail);

    // --- resolve the target directory ---------------------------------------
    std::wstring exeDir{image->exePath()};
    if (const auto slash = exeDir.find_last_of(L'\\'); slash != std::wstring::npos)
        exeDir.resize(slash);

    paths::TokenValues tokens{
        .target = {},  // filled below; targetDir cannot reference itself
        .appDir = exeDir,
        .appId = meta->appId,
        .version = meta->appVersion,
    };

    std::wstring targetDir = flags.haveTarget
                                 ? paths::expandTokens(flags.target, tokens)
                                 : paths::expandTokens(meta->targetDir, tokens);
    if (targetDir.empty())
        return std::unexpected(Error{Ec::MetaFieldInvalid, "targetDir resolved to nothing"});

    // WinPE defines none of the profile variables, so the default targetDir --
    // %LOCALAPPDATA%\HNx\%APPID%\%VERSION% -- expands to itself and resolves
    // against the current directory, leaving a directory literally named
    // "%LOCALAPPDATA%" wherever the package happened to be run from.
    //
    // Anything that did not resolve to an absolute path there goes to the WinPE
    // scratch directory instead. The test is on the result rather than on the
    // template, so a package that names a real location -- D:\apps\%APPID% --
    // still gets it, and an explicit --hnx-target= is never second-guessed.
    if (winPe && !flags.haveTarget && !winpe::isAbsolute(targetDir)) {
        targetDir = winpe::scratchRoot();
        targetDir.append(L"\\HNx\\");
        targetDir.append(meta->appId);
        targetDir.push_back(L'\\');
        targetDir.append(meta->appVersion);
    }

    // GetFullPathNameW makes a relative target (from --hnx-target) absolute
    // against the current directory, which is what a user typing one expects.
    {
        std::wstring full(32768, L'\0');
        const DWORD n = ::GetFullPathNameW(targetDir.c_str(),
                                           static_cast<DWORD>(full.size()), full.data(),
                                           nullptr);
        if (n > 0 && n < full.size()) {
            full.resize(n);
            targetDir = full;
        }
    }
    tokens.target = targetDir;

    auto toc = Toc::parse(image->archive(), image->footer());
    if (!toc) return std::unexpected(toc.error());

    if (flags.info) {
        printInfo(*image, *meta, *toc, targetDir);
        return 0;
    }

    const VerifyMode verifyMode = flags.haveVerify ? flags.verify : meta->verify;
    const unsigned threads = workerCount();

    // --- lock, verify, extract ---------------------------------------------
    //
    // The lock is taken before verification, not just before extraction. Checking
    // the tree while another instance is midway through writing it would produce
    // a plan built from a moving target.
    auto lock = ExtractionLock::acquire(targetDir);
    if (!lock) return std::unexpected(lock.error());

    const State state = State::load(targetDir);

    auto plan = buildPlan(*toc, state, targetDir, verifyMode, image->footer().buildId,
                          threads);
    if (!plan) return std::unexpected(plan.error());

    if (!plan->nothingToDo()) {
        // Free space is checked before the first write rather than discovered by
        // a worker part-way through. Under WinPE the target is the X: RAM disk,
        // whose scratch space is 32 MB by default, so a full volume is the
        // ordinary failure there -- and a half-written tree plus a write error
        // from one thread is a much worse way to learn about it.
        //
        // The lock has already created <target>\.hnx, so the path exists.
        ULARGE_INTEGER freeBytes{};
        if (::GetDiskFreeSpaceExW(targetDir.c_str(), &freeBytes, nullptr, nullptr) &&
            freeBytes.QuadPart < plan->neededBytes) {
            std::string ctx = paths::narrow(paths::toDisplayForm(targetDir));
            ctx += " needs ";
            ctx += std::to_string(plan->neededBytes / (1024 * 1024));
            ctx += " MB, ";
            ctx += std::to_string(freeBytes.QuadPart / (1024 * 1024));
            ctx += " MB free";
            return std::unexpected(Error{Ec::InsufficientSpace, std::move(ctx)});
        }

        // Rebuild the disk paths for the extractor. buildPlan computes these
        // internally but does not hand them back, and recomputing is cheaper than
        // widening the interface for a path that only runs when work is needed.
        std::vector<std::wstring> diskPaths(toc->entries().size());
        for (std::size_t i = 0; i < diskPaths.size(); ++i) {
            auto p = paths::joinExtended(targetDir, toc->path(i));
            if (!p) return std::unexpected(p.error());
            diskPaths[i] = std::move(*p);
        }

        ui::Progress progress;
        const bool wantProgress = meta->ui == UiMode::Progress && !ui::hasConsole();
        if (wantProgress)
            progress.start(meta->appName.empty() ? meta->appId : meta->appName);

        ProgressFn progressFn;
        if (wantProgress)
            progressFn = [&progress](u64 done, u64 total) { progress.update(done, total); };

        // The state file is removed before the first write, so that a crash
        // part-way through cannot leave a stale file claiming the tree is
        // complete. It is rewritten only on success.
        State::invalidate(targetDir);

        auto result = extract(*image, *toc, *plan, diskPaths, threads, progressFn,
                              progress.cancelled());
        progress.finish();

        if (!result) return std::unexpected(result.error());

        // Record what we just wrote. Sizes and times come from the TOC rather than
        // from a fresh stat: those are the values just applied, and re-reading
        // them would cost another full pass over the tree.
        std::vector<StateRecord> records(toc->entries().size());
        for (std::size_t i = 0; i < records.size(); ++i) {
            const EntryDesc& e = toc->entries()[i];
            records[i] = {e.size, e.mtime, e.hash};
        }
        if (auto w = State::store(targetDir, image->footer().buildId, records); !w)
            return std::unexpected(w.error());

        if (ui::hasConsole() && meta->ui == UiMode::Console) {
            wchar_t buf[256];
            ::wsprintfW(buf, L"HNx: wrote %u files, %u MB",
                        static_cast<unsigned>(result->filesWritten),
                        static_cast<unsigned>(result->bytesWritten / (1024 * 1024)));
            ui::writeConsoleLine(buf);
        }
    } else if (!state.matches(image->footer().buildId, toc->entries().size())) {
        // The tree is already correct but the state file is missing or names
        // another build: an identical rebuild, or a state file deleted by hand.
        // Without rewriting it here, nothing would ever write it again, so every
        // later launch would repeat the full sweep and verify=fast would never
        // engage. This happens once; the next warm launch matches and writes
        // nothing. Best effort: a read-only target that is otherwise complete
        // must still launch.
        std::vector<StateRecord> records(toc->entries().size());
        for (std::size_t i = 0; i < records.size(); ++i) {
            const EntryDesc& e = toc->entries()[i];
            records[i] = {e.size, e.mtime, e.hash};
        }
        (void)State::store(targetDir, image->footer().buildId, records);
    }

    // Orphan pruning happens under the lock, after any extraction, so a
    // concurrent instance cannot be reading a file while this one removes it.
    if (flags.clean) {
        auto pruned = pruneOrphans(targetDir, *toc);
        if (!pruned) return std::unexpected(pruned.error());
        if (ui::hasConsole()) {
            wchar_t buf[128];
            ::wsprintfW(buf, L"HNx: removed %u files and %u directories not in the payload",
                        static_cast<unsigned>(pruned->filesDeleted),
                        static_cast<unsigned>(pruned->dirsDeleted));
            ui::writeConsoleLine(buf);
        }
    }

    lock->release();

    if (flags.extractOnly) return 0;

    // singleInstance: refuse to start a second copy.
    //
    // The mutex is held for the lifetime of this process and named from the
    // target directory, so two packages sharing a target are treated as the same
    // application while two versions with distinct targets are not.
    //
    // Deliberately not released before launching: for waitMode none the stub
    // exits immediately, so the mutex would die with it and the guard would do
    // nothing. Holding it means the guard covers the stub's lifetime, which is
    // the whole session only when the stub waits.
    HandleGuard instanceMutex;
    if (meta->singleInstance) {
        std::wstring name = L"Local\\HNx.instance.";
        wchar_t hex[24];
        ::wsprintfW(hex, L"%08X%08X",
                    static_cast<unsigned>(hashBytes(std::as_bytes(
                        std::span{targetDir.data(), targetDir.size()})) >> 32),
                    static_cast<unsigned>(hashBytes(std::as_bytes(
                        std::span{targetDir.data(), targetDir.size()})) & 0xFFFFFFFFu));
        name.append(hex);

        HANDLE m = ::CreateMutexW(nullptr, TRUE, name.c_str());
        if (m && ::GetLastError() == ERROR_ALREADY_EXISTS) {
            ::CloseHandle(m);
            if (ui::hasConsole())
                ui::writeConsoleLine(L"HNx: already running (singleInstance)");
            return 0;
        }
        instanceMutex.reset(m);
    }

    // --- launch -------------------------------------------------------------
    auto execPath = paths::joinExtended(targetDir, [&] {
        std::string s = paths::narrow(meta->exec);
        for (auto& c : s)
            if (c == '\\') c = '/';
        return s;
    }());
    if (!execPath) return std::unexpected(execPath.error());

    LaunchOptions opt;
    opt.exePath = paths::toDisplayForm(*execPath);
    opt.workDir = paths::toDisplayForm(paths::expandTokens(meta->workDir, tokens));
    opt.showWindow = meta->showWindow;
    // requireAdmin is honoured by routing the launch through the shell with the
    // runas verb. WinPE runs everything as SYSTEM and ships no consent UI, so
    // there is nothing to elevate to and the request would only turn a launch
    // that would have worked into one that fails.
    opt.requireAdmin = meta->requireAdmin && !winPe;
    // Firewall rules live only as long as this process holds the WFP session open,
    // so the stub must outlive the child to tear them down: a package with rules
    // waits for the application even if it asked for waitMode none.
    opt.waitMode = (applyFirewall && meta->waitMode == WaitMode::None) ? WaitMode::Wait
                                                                       : meta->waitMode;

    for (const auto& e : meta->env)
        opt.env.push_back({e.name, paths::expandTokens(e.value, tokens)});

    opt.commandLine = quoteArgument(opt.exePath);
    if (!meta->args.empty()) {
        opt.commandLine.push_back(L' ');
        opt.commandLine.append(paths::expandTokens(meta->args, tokens));
    }
    if (meta->argPassthrough && !childArgs.empty()) {
        opt.commandLine.push_back(L' ');
        opt.commandLine.append(childArgs);
    }

    // Apply the temporary firewall rules immediately before launch and hold the
    // WFP session open across it. The session is dynamic, so the OS removes every
    // rule when this process exits -- there is nothing to undo on the way out, and
    // nothing persists if the stub is killed. We are elevated here: a non-elevated
    // run relaunched itself above.
    FirewallSession firewall;
    if (applyFirewall) {
        std::vector<std::wstring> programs;
        programs.push_back(opt.exePath);
        for (const auto& prog : meta->firewall.programs) {
            auto p = paths::joinExtended(targetDir, [&] {
                std::string s = paths::narrow(prog);
                for (auto& c : s)
                    if (c == '\\') c = '/';
                return s;
            }());
            if (!p) return std::unexpected(p.error());
            programs.push_back(paths::toDisplayForm(*p));
        }
        if (auto r = firewall.apply(meta->firewall, programs); !r)
            return std::unexpected(r.error());
    }

    auto launched = launch(opt);
    if (!launched) return std::unexpected(launched.error());

    // Which path was taken is worth reporting: a package that silently falls back
    // to the shell is usually a sign the entry point is not the executable the
    // author thought it was, and it is otherwise invisible.
    if (ui::hasConsole() && meta->ui == UiMode::Console) {
        ui::writeConsoleLine(launched->path == LaunchResult::Path::Direct
                                 ? L"HNx: launched via CreateProcessW"
                                 : L"HNx: launched via ShellExecuteExW");
    }

    if (meta->waitMode == WaitMode::WaitAndClean && launched->waited) {
        // Deleted directly rather than through SHFileOperationW: that API is a
        // shell service, and the shell is one of the parts a minimal Windows
        // image such as WinPE does not necessarily bring. The walk this uses is
        // the same one that backs --hnx-clean.
        removeTree(targetDir);
    }

    return static_cast<int>(launched->exitCode);
}

}  // namespace
}  // namespace hnx

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    auto r = hnx::run();
    if (!r) {
        // Metadata may itself have failed to load, in which case the dialog falls
        // back to a generic title.
        auto meta = hnx::Metadata::loadFromResource();
        hnx::ui::reportError(r.error(), meta ? &*meta : nullptr);
        return 1;
    }
    return *r;
}
