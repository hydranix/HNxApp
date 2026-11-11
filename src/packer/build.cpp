// The build pipeline, shared by the `build` command and the interactive flow.
//
// Ordering is the thing to be careful about here: resources must be written
// before the container is appended, because EndUpdateResourceW rewrites the whole
// file and silently discards any overlay already present.
#include "packer.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <print>
#include <string>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include "meta.hpp"
#include "paths.hpp"

namespace hnx::pack {
namespace {

namespace fs = std::filesystem;

// A build identity must not collide across builds, because the stub uses it to
// decide whether a cached directory belongs to this exact payload. A PRNG seeded
// from the clock would collide for two builds in the same tick, so this comes
// from the system CSPRNG.
[[nodiscard]] u64 makeBuildId() {
    u64 v = 0;
    if (::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&v), sizeof(v),
                          BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 &&
        v != 0)
        return v;

    LARGE_INTEGER qpc{};
    ::QueryPerformanceCounter(&qpc);
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    return (static_cast<u64>(ft.dwHighDateTime) << 32) ^ ft.dwLowDateTime ^
           static_cast<u64>(qpc.QuadPart) * 0x9E3779B97F4A7C15ull ^
           (static_cast<u64>(::GetCurrentProcessId()) << 17);
}

[[nodiscard]] std::string readTextFile(const fs::path& p, bool& ok) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        ok = false;
        return {};
    }
    ok = true;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

[[nodiscard]] std::string jsonEscape(std::string_view s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

// ASCII-only case fold, which is all archive paths need: they are already
// validated UTF-8, and Windows path comparison for the ASCII range is what the
// exclusion list is matching against.
[[nodiscard]] std::string foldAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == '\\') c = '/';
    }
    return out;
}

[[nodiscard]] std::string defaultMetadata(std::string_view appId, std::string_view appName,
                                          std::string_view exec) {
    std::string s = "{\n";
    s += "  \"schemaVersion\": 1,\n";
    s += "  \"appId\": \"" + jsonEscape(appId) + "\",\n";
    s += "  \"appName\": \"" + jsonEscape(appName) + "\",\n";
    s += "  \"appVersion\": \"1.0\",\n";
    s += "  \"targetDir\": \"%LOCALAPPDATA%\\\\HNx\\\\%APPID%\\\\%VERSION%\",\n";
    s += "  \"exec\": \"" + jsonEscape(exec) + "\",\n";
    s += "  \"verify\": \"normal\",\n";
    s += "  \"ui\": \"progress\",\n";
    s += "  \"waitMode\": \"none\",\n";
    s += "  \"argPassthrough\": true,\n";
    s += "  \"allowStubFlags\": true\n";
    s += "}\n";
    return s;
}

}  // namespace

std::string humanBytes(u64 n) {
    char buf[64];
    if (n >= 1024ull * 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.2f GB",
                      static_cast<double>(n) / (1024.0 * 1024 * 1024));
    else if (n >= 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.2f MB", static_cast<double>(n) / (1024.0 * 1024));
    else if (n >= 1024)
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(n) / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(n));
    return buf;
}

int buildPackage(BuildOptions opt, const BuildCallbacks& cb) {
    // Everything user-visible goes through these two, so a GUI sees exactly what
    // the console does without scraping stdout.
    auto say = [&](std::string line) {
        std::println("{}", line);
        cb.note(line);
    };

    // One reporting path, so a run with no visible console cannot fail silently.
    auto fail = [&](const Error& e) -> int {
        std::println(stderr, "hnxpack: {}", e.message());
        cb.note("ERROR: " + e.message());
        if (opt.reportInDialogs)
            ::MessageBoxW(nullptr, paths::widen(e.message()).c_str(), L"HNx",
                          MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return 1;
    };

    if (opt.sources.empty() || opt.output.empty()) {
        std::println(stderr, "hnxpack build requires a source and --out");
        return 2;
    }

    // The stub comes from inside hnxpack unless one was named explicitly.
    //
    // This used to default to a hnxstub.exe beside the executable, which only
    // ever worked inside a build tree: running hnxpack from anywhere else failed
    // at the final step with a path the user never chose.
    //
    // The specific architecture is checked later, once the entry point has been
    // resolved and its own architecture read. Checking here would mean checking
    // for the wrong one.
    if (!opt.stub.empty() && !fs::exists(opt.stub))
        return fail(Error{Ec::StubMissing, paths::narrow(opt.stub.wstring())});

    if (opt.stub.empty() && !hasEmbeddedStub(StubArch::X64) &&
        !hasEmbeddedStub(StubArch::X86))
        return fail(Error{Ec::StubMissing,
                          "this hnxpack has no embedded stub; rebuild it or pass --stub"});

    if (opt.threads == 0) opt.threads = std::max(1u, std::thread::hardware_concurrency());

    const auto t0 = std::chrono::steady_clock::now();

    // --- metadata -----------------------------------------------------------
    std::string metaJson;
    if (!opt.inlineConfig.empty()) {
        metaJson = opt.inlineConfig;
    } else if (!opt.configFile.empty()) {
        bool ok = false;
        metaJson = readTextFile(opt.configFile, ok);
        if (!ok) return fail(Error{Ec::MetaResourceMissing, paths::narrow(opt.configFile.wstring())});
    } else {
        std::println(stderr,
                     "hnxpack: no --config given; generating default metadata. "
                     "Pass --config to control targetDir, exec and launch behaviour.");
        // Named after the first source, looking through a trailing separator
        // ("--src C:\MyApp\" has an empty filename). The appId is sanitized the
        // way the interactive flow does it, because Metadata::parse refuses an
        // empty or unsafe one and a raw folder name can be either.
        fs::path src = opt.sources.front().hostPath;
        if (src.filename().empty()) src = src.parent_path();
        const std::string name = paths::narrow(src.filename().wstring());
        metaJson = defaultMetadata(sanitizeAppId(name), name.empty() ? "app" : name,
                                   "app.exe");
    }

    // Parse it here rather than letting the stub discover a problem at runtime: a
    // broken metadata resource should fail the build, not the launch.
    auto parsedMeta = Metadata::parse(metaJson);
    if (!parsedMeta) return fail(parsedMeta.error());

    say("Packing " + std::to_string(opt.sources.size()) +
        (opt.sources.size() == 1 ? " source" : " sources"));

    // --- walk ---------------------------------------------------------------
    auto entries =
        walkSources(opt.sources, opt.threads, opt.allowUnsafePaths, cb, opt.excluded);
    if (!entries) return fail(entries.error());

    // --- exclusions ----------------------------------------------------------
    //
    // Removing an entry here leaves the file untouched on disk: the tree in the
    // UI chooses what goes into the package, it does not edit the source.
    //
    // An entry is dropped when its own path is excluded or when any ancestor
    // directory is, so unticking a folder does not require the UI to enumerate
    // everything beneath it.
    if (!opt.excluded.empty()) {
        std::vector<std::string> excl;
        excl.reserve(opt.excluded.size());
        for (const auto& e : opt.excluded) excl.push_back(foldAscii(e));
        std::sort(excl.begin(), excl.end());
        excl.erase(std::unique(excl.begin(), excl.end()), excl.end());

        const std::size_t before = entries->size();
        std::erase_if(*entries, [&](const SourceEntry& e) {
            const std::string folded = foldAscii(e.relPath);
            if (std::binary_search(excl.begin(), excl.end(), folded)) return true;
            // Any ancestor excluded?
            for (std::size_t i = 0; i < folded.size(); ++i) {
                if (folded[i] != '/') continue;
                if (std::binary_search(excl.begin(), excl.end(), folded.substr(0, i)))
                    return true;
            }
            return false;
        });

        const std::size_t removed = before - entries->size();
        if (removed > 0) say("  excluded " + std::to_string(removed) + " entries");
    }

    if (entries->empty())
        return fail(Error{Ec::SourceWalkFailed, "everything was excluded"});

    std::size_t nFiles = 0, nDirs = 0;
    u64 totalRaw = 0;
    for (const auto& e : *entries) {
        if (e.isDir) ++nDirs;
        else {
            ++nFiles;
            totalRaw += e.size;
        }
    }
    say("  " + std::to_string(nFiles) + " files, " + std::to_string(nDirs) +
        " directories, " + humanBytes(totalRaw));

    // Locate the declared entry point and mark it.
    //
    // Two things depend on this: the build fails now if the exec is missing --
    // by far the most common packaging mistake, and otherwise invisible until
    // someone runs the package -- and the entry carries ENTRY_FLAG_EXECUTABLE so
    // the stub's fast verify mode knows which single file to check.
    //
    // Matched case-insensitively because Windows paths are, so a config saying
    // "bin/App.exe" still matches a file named "bin/app.exe".
    fs::path resolvedExec;
    {
        std::string execRel = paths::narrow(parsedMeta->exec);
        for (auto& c : execRel)
            if (c == '\\') c = '/';

        const std::wstring wantW = paths::widen(execRel);
        bool found = false;
        for (auto& e : *entries) {
            if (e.isDir) continue;
            const std::wstring haveW = paths::widen(e.relPath);
            if (::CompareStringOrdinal(haveW.c_str(), -1, wantW.c_str(), -1, TRUE) ==
                CSTR_EQUAL) {
                e.isExec = true;
                found = true;
                // The host path, not source-root plus relative path: with several
                // sources the entry may not live under the first one at all.
                resolvedExec = fs::path(paths::toDisplayForm(e.absPath));
                break;
            }
        }
        if (!found)
            return fail(Error{Ec::ExecMissing,
                              execRel + " is not in the package (check the entry "
                                        "point, and that it is not excluded)"});
    }

    // The output must not be one of the files being packaged. Writing the stub
    // over it would destroy a source, which packaging never does, and the
    // archive would then hold content that no longer exists anywhere.
    // Compared case-insensitively because NTFS paths are.
    {
        std::error_code aec;
        const std::wstring outAbs =
            paths::toExtendedForm(fs::absolute(opt.output, aec).lexically_normal().wstring());
        for (const auto& e : *entries) {
            if (e.isDir || e.absPath.empty()) continue;
            if (::CompareStringOrdinal(e.absPath.c_str(), -1, outAbs.c_str(), -1, TRUE) ==
                CSTR_EQUAL)
                return fail(Error{Ec::OutputWriteFailed,
                                  paths::narrow(opt.output.wstring()) +
                                      " is one of the files being packaged; choose "
                                      "another output path"});
        }
    }

    // --- stub architecture ---------------------------------------------------
    //
    // Matched to the entry point so a 32-bit application produces a package that
    // still runs on 32-bit Windows. An explicit --arch wins; a non-PE entry point
    // (.bat, .lnk) carries no architecture and falls back to x64.
    if (!opt.stub.empty()) {
        // An explicitly supplied stub file is used as given; its architecture is
        // whatever the caller built.
    } else if (opt.archExplicit) {
        std::println("  arch: {} (from --arch)", stubArchName(opt.arch));
    } else {
        const auto detected = detectTargetArch(resolvedExec);
        if (detected) {
            opt.arch = *detected;
            std::println("  arch: {} (matched to {})", stubArchName(opt.arch),
                         paths::narrow(resolvedExec.filename().wstring()));
        } else {
            opt.arch = StubArch::X64;
            std::println("  arch: x64 (default; {} is not a PE image)",
                         paths::narrow(resolvedExec.filename().wstring()));
        }
    }

    if (opt.stub.empty() && !hasEmbeddedStub(opt.arch)) {
        // Fall back rather than fail: a package built around the other stub still
        // works, since a 64-bit stub can launch a 32-bit application and a 32-bit
        // stub runs under WOW64. Silence would be wrong, though -- an x86 package
        // silently becoming x64 would not run on 32-bit Windows.
        const StubArch other = opt.arch == StubArch::X64 ? StubArch::X86 : StubArch::X64;
        if (!hasEmbeddedStub(other))
            return fail(Error{Ec::StubMissing, "this hnxpack has no embedded stub"});

        std::println(stderr,
                     "hnxpack: warning: no embedded {} stub, using {} instead. "
                     "The package will not run on {} Windows.",
                     stubArchName(opt.arch), stubArchName(other),
                     opt.arch == StubArch::X86 ? "32-bit" : "64-bit-only");
        opt.arch = other;
    }

    // --- blocks -------------------------------------------------------------
    auto blocks = planBlocks(*entries, opt.blockSize);
    say("  " + std::to_string(blocks.size()) + " blocks (target " + humanBytes(opt.blockSize) + ")");

    const u64 buildId = makeBuildId();

    auto archive = buildArchive(*entries, blocks, opt, buildId, cb);
    if (!archive) return fail(archive.error());

    // --- assemble the output ------------------------------------------------
    std::error_code ec;
    fs::create_directories(opt.output.parent_path(), ec);

    if (opt.stub.empty()) {
        if (auto w = writeEmbeddedStub(opt.arch, opt.output); !w) return fail(w.error());
        say(std::string("  stub: embedded ") + stubArchName(opt.arch));
    } else {
        fs::copy_file(opt.stub, opt.output, fs::copy_options::overwrite_existing, ec);
        if (ec) return fail(Error{Ec::OutputWriteFailed, ec.message()});
        say("  stub: " + paths::narrow(opt.stub.filename().wstring()));
    }

    // Icons come from the entry point by default, so a package looks like the
    // application it contains rather than like every other HNx package.
    fs::path iconSource;
    if (!opt.noIcon) iconSource = opt.iconSource.empty() ? resolvedExec : opt.iconSource;

    // Resources FIRST. EndUpdateResourceW rewrites the entire file, so appending
    // before this point would silently discard the container.
    if (auto r = writeResources(opt.output, metaJson, iconSource); !r) return fail(r.error());

    if (!iconSource.empty()) {
        // Reported because a silent absence is confusing: an entry point with no
        // icons of its own is common and fine, but the user should know why their
        // package came out blank.
        auto probe = readVersionStrings(iconSource);
        say("  icons from " + paths::narrow(iconSource.filename().wstring()));
        if (!probe.productName.empty() || !probe.fileVersion.empty())
            std::println("  version info: {} {}", paths::narrow(probe.productName),
                         paths::narrow(probe.fileVersion));
    }

    const auto stubSize = fs::file_size(opt.output, ec);
    if (ec) return fail(Error{Ec::OutputWriteFailed, ec.message()});

    archive->footer.archiveOffset = stubSize;
    archive->footer.footerCrc32 = footerCrc(archive->footer);

    {
        std::ofstream out(opt.output, std::ios::binary | std::ios::app);
        if (!out) return fail(Error{Ec::OutputWriteFailed, paths::narrow(opt.output.wstring())});
        out.write(reinterpret_cast<const char*>(archive->bytes.data()),
                  static_cast<std::streamsize>(archive->bytes.size()));
        out.write(reinterpret_cast<const char*>(&archive->footer), sizeof(Footer));
        if (!out) return fail(Error{Ec::OutputWriteFailed, "short write"});
    }

    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    const auto finalSize = fs::file_size(opt.output, ec);
    const double ratio = totalRaw > 0 ? static_cast<double>(archive->bytes.size()) /
                                            static_cast<double>(totalRaw)
                                      : 0.0;

    std::println("  payload {} ({:.1f}% of raw)", humanBytes(archive->bytes.size()),
                 ratio * 100.0);
    std::println("Wrote {} ({}) in {} ms", paths::narrow(opt.output.wstring()), humanBytes(finalSize), ms);

    return 0;
}

}  // namespace hnx::pack
