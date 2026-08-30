#include "packer.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <system_error>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "hash.hpp"
#include "paths.hpp"

namespace hnx::pack {
namespace {

namespace fs = std::filesystem;

constexpr u64 fileTimeToU64(const FILETIME& ft) noexcept {
    return (static_cast<u64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// std::filesystem::last_write_time returns a file_time_type whose epoch is not
// specified by the standard, so going through the Win32 attributes directly is
// both simpler and exactly the value the stub will compare against.
[[nodiscard]] bool win32Stat(const std::wstring& extendedPath, u64& size, u64& mtime,
                             u32& attrs) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!::GetFileAttributesExW(extendedPath.c_str(), GetFileExInfoStandard, &d))
        return false;
    size = (static_cast<u64>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    mtime = fileTimeToU64(d.ftLastWriteTime);
    attrs = d.dwFileAttributes;
    return true;
}

[[nodiscard]] std::expected<u64, Error> hashOf(const std::wstring& extendedPath) {
    HANDLE h = ::CreateFileW(extendedPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return std::unexpected(
            Error{Ec::SourceWalkFailed, paths::narrow(extendedPath), ::GetLastError()});

    Hasher hasher;
    std::vector<std::byte> buf(1024 * 1024);
    for (;;) {
        DWORD got = 0;
        if (!::ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
            const DWORD e = ::GetLastError();
            ::CloseHandle(h);
            return std::unexpected(Error{Ec::SourceWalkFailed, paths::narrow(extendedPath), e});
        }
        if (got == 0) break;
        hasher.update(std::span{buf.data(), got});
    }
    ::CloseHandle(h);
    return hasher.digest();
}

// Converts a path relative to the source root into the archive form: UTF-8,
// forward slashes, no leading separator.
[[nodiscard]] std::string toArchivePath(const fs::path& rel) {
    std::wstring w = rel.wstring();
    std::replace(w.begin(), w.end(), L'\\', L'/');
    return paths::narrow(w);
}

// (the anonymous namespace continues: everything below is internal to this file)

// Prepends a source item's prefix to a path relative to that item.
[[nodiscard]] std::string applyPrefix(std::string_view prefix, std::string_view rel) {
    if (prefix.empty()) return std::string(rel);
    std::string out(prefix);
    if (!out.empty() && out.back() != '/') out.push_back('/');
    out.append(rel);
    return out;
}

// Every directory level implied by a path, so a file added under a prefix gets
// the directories it needs even though no source item enumerated them.
void collectImpliedDirs(std::string_view path, std::vector<std::string>& out) {
    std::size_t pos = 0;
    while ((pos = path.find('/', pos)) != std::string_view::npos) {
        out.emplace_back(path.substr(0, pos));
        ++pos;
    }
}

std::expected<std::vector<SourceEntry>, Error> walkOneDirectory(
    const fs::path& root, std::string_view prefix, std::size_t sourceIndex,
    bool allowUnsafePaths, std::vector<SourceEntry>& entries) {
    std::error_code ec;

    const fs::path rootAbs = fs::absolute(root, ec).lexically_normal();
    if (ec) return std::unexpected(Error{Ec::SourceWalkFailed, root.string()});

    // skip_permission_denied keeps an unreadable subdirectory from aborting the
    // whole walk; a genuinely unreadable file still fails later when it is hashed.
    fs::recursive_directory_iterator it(
        rootAbs, fs::directory_options::skip_permission_denied, ec);
    if (ec) return std::unexpected(Error{Ec::SourceWalkFailed, root.string()});

    // Iterated with an explicit increment(ec) rather than a range-for.
    //
    // The error_code passed to the constructor covers only construction: a
    // range-for calls the throwing operator++, so any mid-walk failure -- a path
    // too long for the API, a directory removed underneath us -- escapes as an
    // exception. In a console tool with no handler that surfaced as a silent
    // exit 127 with no diagnostic at all, which is the least useful thing a build
    // tool can do.
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec)
            return std::unexpected(Error{Ec::SourceWalkFailed,
                                         "cannot enumerate source tree: " + ec.message()});

        const fs::directory_entry& de = *it;

        const fs::path rel = fs::relative(de.path(), rootAbs, ec);
        if (ec || rel.empty()) continue;

        SourceEntry e;
        e.relPath = applyPrefix(prefix, toArchivePath(rel));
        e.absPath = paths::toExtendedForm(de.path().wstring());
        e.sourceIndex = sourceIndex;

        // A reparse point (symlink or junction) is refused rather than followed:
        // following one would silently pull in content from outside the source
        // tree, and recording one would need the stub to create links, which v1
        // deliberately does not do.
        std::error_code sec;
        if (fs::is_symlink(de.symlink_status(sec)))
            return std::unexpected(Error{Ec::SymlinkRejected, e.relPath});

        if (!allowUnsafePaths) {
            if (auto v = paths::validateArchivePath(e.relPath); !v)
                return std::unexpected(v.error());
        }

        u64 size = 0, mtime = 0;
        u32 attrs = 0;
        if (!win32Stat(e.absPath, size, mtime, attrs))
            return std::unexpected(
                Error{Ec::SourceWalkFailed, e.relPath, ::GetLastError()});

        e.isDir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.size = e.isDir ? 0 : size;
        e.mtime = mtime;
        // Only the attributes that round-trip; see docs/format.md section 3.5.
        e.attrs = attrs & ATTR_MASK;

        entries.push_back(std::move(e));
    }

    return {};
}

// Hashes every file in parallel. The dominant cost of a build on a large tree,
// and embarrassingly parallel.
[[nodiscard]] std::expected<void, Error> hashAll(std::vector<SourceEntry>& entries,
                                                 unsigned threads,
                                                 const BuildCallbacks& cb) {
    {
        std::vector<std::size_t> fileIdx;
        fileIdx.reserve(entries.size());
        // Zero-byte files are hashed too. XXH3 of empty input is a specific
        // non-zero constant, so skipping them here would leave hash == 0 in the
        // TOC while every reader computes the real digest -- an eternal mismatch
        // that paranoid verification would report as corruption on every launch.
        for (std::size_t i = 0; i < entries.size(); ++i)
            if (!entries[i].isDir) fileIdx.push_back(i);

        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> done{0};
        std::mutex errMutex;
        std::optional<Error> firstError;
        std::atomic<bool> abort{false};

        const unsigned n = std::max(1u, threads);
        std::vector<std::jthread> pool;
        pool.reserve(n);

        for (unsigned t = 0; t < n; ++t) {
            pool.emplace_back([&] {
                for (;;) {
                    if (abort.load(std::memory_order_relaxed)) return;
                    const std::size_t k = next.fetch_add(1, std::memory_order_relaxed);
                    if (k >= fileIdx.size()) return;
                    {
                        std::lock_guard lock(errMutex);
                        if (firstError) return;
                    }
                    auto h = hashOf(entries[fileIdx[k]].absPath);
                    if (!h) {
                        std::lock_guard lock(errMutex);
                        if (!firstError) firstError = h.error();
                        return;
                    }
                    entries[fileIdx[k]].hash = *h;
                    done.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        // Cancellation is polled here rather than in the workers so the callback
        // is only ever invoked from the calling thread.
        if (cb.cancelled || cb.progress) {
            while (done.load() < fileIdx.size() && next.load() < fileIdx.size() + n) {
                if (cb.isCancelled()) {
                    abort.store(true);
                    break;
                }
                cb.step("Hashing", done.load(), fileIdx.size());
                ::Sleep(30);
            }
        }

        pool.clear();  // joins

        if (firstError) return std::unexpected(*firstError);
        if (abort.load()) return std::unexpected(Error{Ec::SourceWalkFailed, "cancelled"});
    }

    return {};
}

}  // namespace

bool SourceItem::isDirectory() const {
    std::error_code ec;
    return fs::is_directory(hostPath, ec);
}

std::string suggestArchivePrefix(const fs::path& hostPath,
                                 std::span<const SourceItem> existing) {
    std::error_code ec;
    const bool isDir = fs::is_directory(hostPath, ec);

    // A lone file always lands at the root: it is almost always a DLL or a config
    // that belongs beside the application, not in a folder of its own.
    if (!isDir) return {};

    // The first folder contributes its contents at the root, matching a
    // conventional single-folder package. Later folders get their own name, so
    // two unrelated sources cannot collide by accident.
    const bool haveDirAtRoot =
        std::any_of(existing.begin(), existing.end(), [](const SourceItem& s) {
            return s.archivePrefix.empty() && s.isDirectory();
        });
    if (!haveDirAtRoot) return {};

    std::string name = paths::narrow(hostPath.filename().wstring());
    if (name.empty()) name = "folder";

    // Avoid colliding with a prefix already in use.
    std::string candidate = name;
    for (int n = 2; n < 1000; ++n) {
        const bool taken =
            std::any_of(existing.begin(), existing.end(), [&](const SourceItem& s) {
                return s.archivePrefix == candidate;
            });
        if (!taken) break;
        candidate = name + "_" + std::to_string(n);
    }
    return candidate;
}

namespace {

std::expected<std::vector<SourceEntry>, Error> walkSourcesImpl(
    std::span<const SourceItem> items, unsigned threads, bool allowUnsafePaths,
    const BuildCallbacks& cb) {
    if (items.empty())
        return std::unexpected(Error{Ec::SourceWalkFailed, "nothing to package"});

    std::vector<SourceEntry> entries;
    std::vector<std::string> impliedDirs;

    for (std::size_t i = 0; i < items.size(); ++i) {
        const SourceItem& item = items[i];
        if (cb.isCancelled())
            return std::unexpected(Error{Ec::SourceWalkFailed, "cancelled"});

        std::error_code ec;
        if (!fs::exists(item.hostPath, ec))
            return std::unexpected(
                Error{Ec::SourceWalkFailed, item.hostPath.string() + " no longer exists"});

        if (!item.archivePrefix.empty() && !allowUnsafePaths) {
            if (auto v = paths::validateArchivePath(item.archivePrefix); !v)
                return std::unexpected(
                    Error{v.error().code, "target folder: " + item.archivePrefix});
        }

        cb.note("Scanning " + item.hostPath.string());

        if (fs::is_directory(item.hostPath, ec)) {
            if (auto r = walkOneDirectory(item.hostPath, item.archivePrefix, i,
                                          allowUnsafePaths, entries);
                !r)
                return std::unexpected(r.error());
        } else {
            // A single file lands under the prefix keeping its own name.
            SourceEntry e;
            e.relPath = applyPrefix(item.archivePrefix,
                                    paths::narrow(item.hostPath.filename().wstring()));
            e.absPath = paths::toExtendedForm(fs::absolute(item.hostPath, ec).wstring());
            e.sourceIndex = i;

            if (!allowUnsafePaths) {
                if (auto v = paths::validateArchivePath(e.relPath); !v)
                    return std::unexpected(v.error());
            }

            u64 size = 0, mtime = 0;
            u32 attrs = 0;
            if (!win32Stat(e.absPath, size, mtime, attrs))
                return std::unexpected(
                    Error{Ec::SourceWalkFailed, e.relPath, ::GetLastError()});

            e.isDir = false;
            e.size = size;
            e.mtime = mtime;
            e.attrs = attrs & ATTR_MASK;
            entries.push_back(std::move(e));
        }

        if (!item.archivePrefix.empty()) collectImpliedDirs(item.archivePrefix + "/", impliedDirs);
    }

    // Directories implied by a prefix are not enumerated by any walk, so they are
    // synthesised here. Without them the stub would try to create a file inside a
    // directory that no entry ever declared.
    if (!impliedDirs.empty()) {
        std::sort(impliedDirs.begin(), impliedDirs.end());
        impliedDirs.erase(std::unique(impliedDirs.begin(), impliedDirs.end()),
                          impliedDirs.end());

        for (const auto& d : impliedDirs) {
            const bool present = std::any_of(
                entries.begin(), entries.end(),
                [&](const SourceEntry& e) { return e.isDir && e.relPath == d; });
            if (present) continue;

            SourceEntry e;
            e.relPath = d;
            e.isDir = true;
            // No host path: this directory exists only inside the package. The
            // extractor creates it, and nothing ever reads content from it.
            entries.push_back(std::move(e));
        }
    }

    // A collision means two sources want the same archive path. Which one won
    // would depend on enumeration order, so refuse rather than pick.
    {
        std::vector<const SourceEntry*> sorted;
        sorted.reserve(entries.size());
        for (const auto& e : entries) sorted.push_back(&e);
        std::sort(sorted.begin(), sorted.end(),
                  [](const SourceEntry* a, const SourceEntry* b) {
                      return a->relPath < b->relPath;
                  });
        for (std::size_t i = 1; i < sorted.size(); ++i) {
            if (sorted[i]->relPath != sorted[i - 1]->relPath) continue;
            // Two directories at the same path are the same directory.
            if (sorted[i]->isDir && sorted[i - 1]->isDir) continue;
            return std::unexpected(Error{
                Ec::SourceWalkFailed,
                "two sources both want to place " + sorted[i]->relPath +
                    " in the package; give one of them a different target folder"});
        }
    }

    if (auto r = hashAll(entries, threads, cb); !r) return std::unexpected(r.error());

    // Directories first, then files, each group sorted by path. The stub creates
    // directories in TOC order and relies on a parent appearing before its
    // children, which sorting by path guarantees.
    std::stable_sort(entries.begin(), entries.end(),
                     [](const SourceEntry& a, const SourceEntry& b) {
                         if (a.isDir != b.isDir) return a.isDir;
                         return a.relPath < b.relPath;
                     });

    return entries;
}

}  // namespace

// Public entry point.
//
// std::filesystem can still throw from calls that have no error_code overload, so
// the whole walk is wrapped. A build tool that dies with no message is far worse
// than one that reports which path defeated it.
std::expected<std::vector<SourceEntry>, Error> walkSources(
    std::span<const SourceItem> items, unsigned threads, bool allowUnsafePaths,
    const BuildCallbacks& cb) {
    try {
        return walkSourcesImpl(items, threads, allowUnsafePaths, cb);
    } catch (const fs::filesystem_error& e) {
        return std::unexpected(Error{Ec::SourceWalkFailed,
                                     std::string(e.what()) + " [" + e.path1().string() + "]"});
    } catch (const std::exception& e) {
        return std::unexpected(Error{Ec::SourceWalkFailed, e.what()});
    }
}

std::expected<std::vector<SourceEntry>, Error> walkSource(const fs::path& root,
                                                          unsigned threads,
                                                          bool allowUnsafePaths) {
    const SourceItem one{root, {}};
    return walkSources({&one, 1}, threads, allowUnsafePaths, {});
}

}  // namespace hnx::pack
