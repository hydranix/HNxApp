#include "extract.hpp"

#include <algorithm>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "codec/codec.hpp"
#include "paths.hpp"

namespace hnx {
namespace {

struct Task {
    enum class Kind { SharedBlock, SpanningFile } kind;
    u32 blockIndex;    // first block
    u32 blockSpan;     // SpanningFile only
    std::size_t entry; // SpanningFile only
};

// RAII wrapper. Extraction has many early-return error paths and leaking a handle
// on any of them would keep the target directory locked against the retry.
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() { close(); }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h_(std::exchange(o.h_, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) {
            close();
            h_ = std::exchange(o.h_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return h_; }
    [[nodiscard]] bool valid() const noexcept { return h_ != INVALID_HANDLE_VALUE && h_ != nullptr; }

    void close() noexcept {
        if (valid()) ::CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

constexpr FILETIME u64ToFileTime(u64 v) noexcept {
    FILETIME ft{};
    ft.dwLowDateTime = static_cast<DWORD>(v & 0xFFFFFFFFu);
    ft.dwHighDateTime = static_cast<DWORD>(v >> 32);
    return ft;
}

[[nodiscard]] bool writeAll(HANDLE h, std::span<const std::byte> data) noexcept {
    std::size_t done = 0;
    while (done < data.size()) {
        // WriteFile takes a DWORD, so a >4 GB member has to go out in chunks.
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(data.size() - done, 64u * 1024 * 1024));
        DWORD put = 0;
        if (!::WriteFile(h, data.data() + done, chunk, &put, nullptr)) return false;
        if (put == 0) return false;
        done += put;
    }
    return true;
}

// Opens the destination for writing, clearing READONLY first if an existing file
// carries it. Without that, replacing a file the previous extraction marked
// read-only fails with ERROR_ACCESS_DENIED.
[[nodiscard]] std::expected<Handle, Error> openForWrite(const std::wstring& path,
                                                        u64 sizeHint) {
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
        ::SetFileAttributesW(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);

    Handle h{::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (!h.valid())
        return std::unexpected(
            Error{Ec::CreateFileFailed, paths::narrow(paths::toDisplayForm(path)),
                  ::GetLastError()});

    // Preallocate so the file system can pick contiguous extents up front rather
    // than growing the file in steps. Purely a fragmentation optimisation, so a
    // failure here is ignored.
    if (sizeHint > 0) {
        LARGE_INTEGER li{};
        li.QuadPart = static_cast<LONGLONG>(sizeHint);
        if (::SetFilePointerEx(h.get(), li, nullptr, FILE_BEGIN)) {
            ::SetEndOfFile(h.get());
            li.QuadPart = 0;
            ::SetFilePointerEx(h.get(), li, nullptr, FILE_BEGIN);
        }
    }

    return h;
}

// Applies mtime and attributes. Order matters: the timestamp must be set while
// the handle is still writable, and READONLY must go on last or it would block
// both the write and the timestamp update.
[[nodiscard]] std::expected<void, Error> finalizeFile(Handle& h,
                                                      const std::wstring& path,
                                                      const EntryDesc& e) {
    const FILETIME ft = u64ToFileTime(e.mtime);
    // Setting creation time as well as write time keeps a re-extracted tree
    // looking identical to the original rather than dated to the extraction.
    if (!::SetFileTime(h.get(), &ft, nullptr, &ft))
        return std::unexpected(Error{Ec::SetTimeFailed,
                                     paths::narrow(paths::toDisplayForm(path)),
                                     ::GetLastError()});
    h.close();

    const u32 attrs = e.attrs & ATTR_MASK;
    if (attrs != 0) ::SetFileAttributesW(path.c_str(), attrs);

    return {};
}

}  // namespace

std::expected<void, Error> createDirRecursive(const std::wstring& extendedPath) {
    if (::CreateDirectoryW(extendedPath.c_str(), nullptr)) return {};

    const DWORD err = ::GetLastError();
    if (err == ERROR_ALREADY_EXISTS) return {};

    if (err != ERROR_PATH_NOT_FOUND)
        return std::unexpected(Error{Ec::CreateDirFailed,
                                     paths::narrow(paths::toDisplayForm(extendedPath)), err});

    // Walk up to the first parent that exists, then come back down. Recursing on
    // the parent rather than looping keeps the \\?\ and UNC prefix handling in
    // one place.
    const std::size_t slash = extendedPath.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash == 0)
        return std::unexpected(Error{Ec::CreateDirFailed,
                                     paths::narrow(paths::toDisplayForm(extendedPath)), err});

    const std::wstring parent = extendedPath.substr(0, slash);

    // Do not try to create the \\?\C: or \\?\UNC\server prefix itself.
    if (parent.size() <= 6)
        return std::unexpected(Error{Ec::CreateDirFailed,
                                     paths::narrow(paths::toDisplayForm(extendedPath)), err});

    if (auto r = createDirRecursive(parent); !r) return r;

    if (::CreateDirectoryW(extendedPath.c_str(), nullptr)) return {};
    const DWORD err2 = ::GetLastError();
    if (err2 == ERROR_ALREADY_EXISTS) return {};
    return std::unexpected(Error{Ec::CreateDirFailed,
                                 paths::narrow(paths::toDisplayForm(extendedPath)), err2});
}

namespace {

// Lowercased for comparison, because NTFS is case-insensitive: a file the
// archive calls "App.dll" and the disk calls "app.dll" are the same file, and
// treating them as different would delete one the payload actually provides.
[[nodiscard]] std::wstring foldCase(std::wstring s) {
    if (!s.empty()) ::CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

void collectTree(const std::wstring& dirExtended, const std::wstring& relPrefix,
                 std::vector<std::pair<std::wstring, bool>>& out) {
    std::wstring pattern = dirExtended;
    pattern.append(L"\\*");

    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                  FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        const std::wstring_view name{fd.cFileName};
        if (name == L"." || name == L"..") continue;

        // Never descend into or remove our own bookkeeping directory: it holds
        // the lock this very run is standing on.
        if (relPrefix.empty() && name == L".hnx") continue;

        std::wstring rel = relPrefix;
        if (!rel.empty()) rel.push_back(L'\\');
        rel.append(name);

        std::wstring full = dirExtended;
        full.push_back(L'\\');
        full.append(name);

        const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

        // A reparse point is recorded but never descended into: following one
        // would walk outside the target entirely.
        const bool isReparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

        if (isDir && !isReparse) collectTree(full, rel, out);
        out.emplace_back(std::move(rel), isDir);
    } while (::FindNextFileW(h, &fd));

    ::FindClose(h);
}

}  // namespace

std::expected<PruneResult, Error> pruneOrphans(std::wstring_view targetDir,
                                               const Toc& toc) {
    // Everything the payload provides, folded for case-insensitive lookup.
    std::vector<std::wstring> expected;
    expected.reserve(toc.entries().size());
    for (const auto& e : toc.entries()) {
        std::wstring w = paths::widen(toc.path(e));
        std::replace(w.begin(), w.end(), L'/', L'\\');
        expected.push_back(foldCase(std::move(w)));
    }
    std::sort(expected.begin(), expected.end());

    std::vector<std::pair<std::wstring, bool>> present;
    collectTree(std::wstring(paths::toExtendedForm(targetDir)), {}, present);

    PruneResult result;
    const std::wstring base = paths::toExtendedForm(targetDir);

    // collectTree emits children before their parents, so deleting in order
    // empties a directory before trying to remove it.
    for (const auto& [rel, isDir] : present) {
        if (std::binary_search(expected.begin(), expected.end(), foldCase(rel))) continue;

        std::wstring full = base;
        full.push_back(L'\\');
        full.append(rel);

        if (isDir) {
            if (::RemoveDirectoryW(full.c_str())) ++result.dirsDeleted;
            // A non-empty directory means it still holds something the payload
            // does provide, so leaving it is correct.
        } else {
            const DWORD attrs = ::GetFileAttributesW(full.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
                ::SetFileAttributesW(full.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
            if (::DeleteFileW(full.c_str())) ++result.filesDeleted;
        }
    }

    return result;
}

std::expected<ExtractResult, Error> extract(const SelfImage& image, const Toc& toc,
                                            const VerifyPlan& plan,
                                            const std::vector<std::wstring>& diskPaths,
                                            unsigned threads,
                                            const ProgressFn& progress,
                                            const std::atomic<bool>& cancelled) {
    const auto entries = toc.entries();
    const auto blocks = toc.blocks();

    ExtractResult result;

    // --- directories --------------------------------------------------------
    //
    // Single-threaded and in TOC order, which the packer guarantees puts a parent
    // before its children. Parallelising this would mostly contend on the same
    // parents.
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!plan.entryNeeded[i]) continue;
        if (static_cast<EntryType>(entries[i].type) != EntryType::Directory) continue;
        if (auto r = createDirRecursive(diskPaths[i]); !r) return std::unexpected(r.error());
        ++result.dirsCreated;
    }

    // A file whose parent directory is not itself an archive entry still needs
    // that parent to exist. Creating it lazily per file would mean a redundant
    // syscall per file, so collect the distinct parents once.
    {
        std::vector<std::wstring> parents;
        parents.reserve(plan.neededFiles);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (!plan.entryNeeded[i]) continue;
            if (static_cast<EntryType>(entries[i].type) != EntryType::File) continue;
            const std::size_t slash = diskPaths[i].find_last_of(L'\\');
            if (slash != std::wstring::npos) parents.emplace_back(diskPaths[i].substr(0, slash));
        }
        std::sort(parents.begin(), parents.end());
        parents.erase(std::unique(parents.begin(), parents.end()), parents.end());
        for (const auto& p : parents)
            if (auto r = createDirRecursive(p); !r) return std::unexpected(r.error());
    }

    // --- block membership index ---------------------------------------------
    //
    // Built once so a worker can find the members of its block directly. Scanning
    // all entries per block instead would be O(entries * blocks), which is fine
    // for a small app and quadratic pain for a large one.
    std::vector<std::vector<std::size_t>> blockMembers(blocks.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!plan.entryNeeded[i]) continue;
        const EntryDesc& e = entries[i];
        if (static_cast<EntryType>(e.type) != EntryType::File) continue;
        if (e.flags & ENTRY_FLAG_SPANS_BLOCKS) continue;
        blockMembers[e.blockIndex].push_back(i);
    }

    // --- task list ----------------------------------------------------------
    std::vector<Task> tasks;
    {
        // A block belonging to a spanning file is handled by that file's task, so
        // track which blocks are already claimed.
        std::vector<std::uint8_t> claimed(blocks.size(), 0);

        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (!plan.entryNeeded[i]) continue;
            const EntryDesc& e = entries[i];
            if (static_cast<EntryType>(e.type) != EntryType::File) continue;
            if (!(e.flags & ENTRY_FLAG_SPANS_BLOCKS)) continue;

            tasks.push_back({Task::Kind::SpanningFile, e.blockIndex, e.blockSpan, i});
            for (u32 b = e.blockIndex; b < e.blockIndex + e.blockSpan; ++b)
                claimed[b] = 1;
        }

        for (std::size_t b = 0; b < blocks.size(); ++b) {
            if (!plan.blockNeeded[b] || claimed[b]) continue;
            tasks.push_back({Task::Kind::SharedBlock, static_cast<u32>(b), 0, 0});
        }

        // Largest first. With only a handful of workers, starting the big blocks
        // early stops one oversized block from becoming the tail that everything
        // waits on.
        std::sort(tasks.begin(), tasks.end(), [&](const Task& a, const Task& b) {
            const u64 sa = a.kind == Task::Kind::SpanningFile ? entries[a.entry].size
                                                              : blocks[a.blockIndex].rawSize;
            const u64 sb = b.kind == Task::Kind::SpanningFile ? entries[b.entry].size
                                                              : blocks[b.blockIndex].rawSize;
            return sa > sb;
        });
    }

    // --- workers ------------------------------------------------------------
    const unsigned workerCount =
        std::max(1u, std::min<unsigned>(threads, static_cast<unsigned>(tasks.size())));

    std::atomic<std::size_t> nextTask{0};
    std::atomic<unsigned long long> bytesDone{0};
    std::atomic<std::size_t> filesWritten{0};

    std::mutex errMutex;
    std::optional<Error> firstError;

    auto fail = [&](Error e) {
        std::lock_guard lock(errMutex);
        if (!firstError) firstError = std::move(e);
    };
    auto failed = [&] {
        std::lock_guard lock(errMutex);
        return firstError.has_value();
    };

    const u64 totalBytes = plan.neededBytes;

    auto worker = [&] {
        // One buffer per worker, reused across tasks. This is the allocation that
        // bounds peak memory: workers * MAX_BLOCK_RAW, and nothing scales with the
        // size of the archive or of the largest file in it.
        std::vector<std::byte> buf;

        for (;;) {
            if (cancelled.load(std::memory_order_relaxed) || failed()) return;

            const std::size_t ti = nextTask.fetch_add(1, std::memory_order_relaxed);
            if (ti >= tasks.size()) return;
            const Task& task = tasks[ti];

            if (task.kind == Task::Kind::SharedBlock) {
                const BlockDesc& b = blocks[task.blockIndex];

                auto src = image.slice(b.compOffset, b.compSize);
                if (!src) { fail(src.error()); return; }

                buf.resize(b.rawSize);
                auto n = codec::decompress(b.codec, *src, buf);
                if (!n) { fail(n.error()); return; }
                if (*n != b.rawSize) {
                    fail(Error{Ec::SizeMismatch, "block " + std::to_string(task.blockIndex)});
                    return;
                }

                for (const std::size_t i : blockMembers[task.blockIndex]) {
                    if (failed()) return;
                    const EntryDesc& e = entries[i];

                    auto h = openForWrite(diskPaths[i], e.size);
                    if (!h) { fail(h.error()); return; }

                    const std::span<const std::byte> content{
                        buf.data() + e.offsetInBlock, static_cast<std::size_t>(e.size)};

                    if (!writeAll(h->get(), content)) {
                        fail(Error{Ec::WriteFailed,
                                   paths::narrow(paths::toDisplayForm(diskPaths[i])),
                                   ::GetLastError()});
                        return;
                    }

                    if (auto r = finalizeFile(*h, diskPaths[i], e); !r) {
                        fail(r.error());
                        return;
                    }

                    filesWritten.fetch_add(1, std::memory_order_relaxed);
                    const auto done = bytesDone.fetch_add(e.size, std::memory_order_relaxed) + e.size;
                    if (progress) progress(done, totalBytes);
                }
            } else {
                const EntryDesc& e = entries[task.entry];

                auto h = openForWrite(diskPaths[task.entry], e.size);
                if (!h) { fail(h.error()); return; }

                for (u32 bi = task.blockIndex; bi < task.blockIndex + task.blockSpan; ++bi) {
                    if (cancelled.load(std::memory_order_relaxed) || failed()) return;

                    const BlockDesc& b = blocks[bi];
                    auto src = image.slice(b.compOffset, b.compSize);
                    if (!src) { fail(src.error()); return; }

                    buf.resize(b.rawSize);
                    auto n = codec::decompress(b.codec, *src, buf);
                    if (!n) { fail(n.error()); return; }
                    if (*n != b.rawSize) {
                        fail(Error{Ec::SizeMismatch, "block " + std::to_string(bi)});
                        return;
                    }

                    if (!writeAll(h->get(), buf)) {
                        fail(Error{Ec::WriteFailed,
                                   paths::narrow(paths::toDisplayForm(diskPaths[task.entry])),
                                   ::GetLastError()});
                        return;
                    }

                    const auto done =
                        bytesDone.fetch_add(b.rawSize, std::memory_order_relaxed) + b.rawSize;
                    if (progress) progress(done, totalBytes);
                }

                if (auto r = finalizeFile(*h, diskPaths[task.entry], e); !r) {
                    fail(r.error());
                    return;
                }
                filesWritten.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    {
        std::vector<std::jthread> pool;
        pool.reserve(workerCount);
        for (unsigned t = 0; t < workerCount; ++t) pool.emplace_back(worker);
    }  // jthread joins here

    if (firstError) return std::unexpected(*firstError);
    if (cancelled.load()) return std::unexpected(Error{Ec::WriteFailed, "cancelled"});

    result.filesWritten = filesWritten.load();
    result.bytesWritten = bytesDone.load();
    return result;
}

}  // namespace hnx
