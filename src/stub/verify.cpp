#include "verify.hpp"

#include <atomic>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "hash.hpp"
#include "paths.hpp"

namespace hnx {
namespace {

// 2 seconds in FILETIME ticks (100ns each).
constexpr u64 MTIME_TOLERANCE = 2ull * 10'000'000ull;

constexpr u64 fileTimeToU64(const FILETIME& ft) noexcept {
    return (static_cast<u64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

struct DiskInfo {
    bool exists = false;
    u64 size = 0;
    u64 mtime = 0;
    bool isDir = false;
};

[[nodiscard]] DiskInfo statFile(const std::wstring& extendedPath) noexcept {
    DiskInfo info;
    WIN32_FILE_ATTRIBUTE_DATA d{};
    // GetFileAttributesExW with the standard info class is the cheapest way to
    // get existence, size and mtime in one call, which is what makes the sweep
    // over thousands of entries affordable.
    if (!::GetFileAttributesExW(extendedPath.c_str(), GetFileExInfoStandard, &d))
        return info;

    info.exists = true;
    info.isDir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info.size = (static_cast<u64>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    info.mtime = fileTimeToU64(d.ftLastWriteTime);
    return info;
}

// Runs `body(i)` for i in [0, count) across `threads` workers.
//
// A plain atomic counter rather than a chunked split: entries vary enormously in
// cost (a stat is microseconds, a hash of a 200 MB file is not), so static
// partitioning would leave workers idle behind one slow item.
template <typename F>
void parallelFor(std::size_t count, unsigned threads, F&& body) {
    if (count == 0) return;

    if (threads <= 1 || count == 1) {
        for (std::size_t i = 0; i < count; ++i) body(i);
        return;
    }

    std::atomic<std::size_t> next{0};
    std::vector<std::jthread> pool;
    pool.reserve(threads);

    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&] {
            for (;;) {
                const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= count) return;
                body(i);
            }
        });
    }
}

}  // namespace

bool mtimeEqual(u64 a, u64 b) noexcept {
    return (a > b ? a - b : b - a) <= MTIME_TOLERANCE;
}

std::expected<u64, Error> hashFile(const std::wstring& extendedPath) {
    HANDLE h = ::CreateFileW(extendedPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return std::unexpected(
            Error{Ec::CreateFileFailed, paths::narrow(extendedPath), ::GetLastError()});

    Hasher hasher;
    std::vector<std::byte> buf(256 * 1024);

    for (;;) {
        DWORD got = 0;
        if (!::ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
            const DWORD e = ::GetLastError();
            ::CloseHandle(h);
            return std::unexpected(Error{Ec::WriteFailed, paths::narrow(extendedPath), e});
        }
        if (got == 0) break;
        hasher.update(std::span{buf.data(), got});
    }

    ::CloseHandle(h);
    return hasher.digest();
}

std::expected<VerifyPlan, Error> buildPlan(const Toc& toc, const State& state,
                                           std::wstring_view targetDir,
                                           VerifyMode mode, u64 buildId,
                                           unsigned threads) {
    const auto entries = toc.entries();
    const auto blocks = toc.blocks();

    VerifyPlan plan;
    plan.entryNeeded.assign(entries.size(), false);
    plan.blockNeeded.assign(blocks.size(), false);

    const bool stateUsable =
        mode != VerifyMode::Paranoid && state.matches(buildId, entries.size());

    // Fast mode trusts a matching state file and only confirms the exec target is
    // present. It is opt-in precisely because deleting a DLL by hand would go
    // unnoticed until the app failed to start.
    const bool fastPath = stateUsable && mode == VerifyMode::Fast;

    // Precompute the extended target paths once. They are needed by the stat
    // sweep and again by the extractor, and building them is not free at tens of
    // thousands of entries.
    std::vector<std::wstring> diskPaths(entries.size());
    {
        std::expected<void, Error> firstError{};
        std::atomic<bool> failed{false};

        parallelFor(entries.size(), threads, [&](std::size_t i) {
            if (failed.load(std::memory_order_relaxed)) return;
            auto p = paths::joinExtended(targetDir, toc.path(entries[i]));
            if (!p) {
                // Validation already ran during Toc::parse, so reaching here means
                // the target directory itself is pathological (for example a
                // target whose own name pushes a child outside it).
                bool expectedFlag = false;
                if (failed.compare_exchange_strong(expectedFlag, true))
                    firstError = std::unexpected(p.error());
                return;
            }
            diskPaths[i] = std::move(*p);
        });

        if (failed.load()) return std::unexpected(firstError.error());
    }

    std::atomic<std::size_t> neededFiles{0};
    std::atomic<std::size_t> neededDirs{0};
    std::atomic<unsigned long long> neededBytes{0};

    parallelFor(entries.size(), threads, [&](std::size_t i) {
        const EntryDesc& e = entries[i];
        const bool isDir = static_cast<EntryType>(e.type) == EntryType::Directory;

        if (fastPath && !(e.flags & ENTRY_FLAG_EXECUTABLE)) return;

        const DiskInfo info = statFile(diskPaths[i]);

        if (isDir) {
            // A directory is needed only if it is absent. An existing directory is
            // left exactly as it is, including any user data inside it.
            if (!info.exists || !info.isDir) {
                plan.entryNeeded[i] = true;
                neededDirs.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }

        bool need = false;

        if (!info.exists || info.isDir) {
            need = true;
        } else if (info.size != e.size) {
            need = true;
        } else if (mode == VerifyMode::Paranoid) {
            auto h = hashFile(diskPaths[i]);
            need = !h || *h != e.hash;
        } else if (stateUsable && mtimeEqual(info.mtime, state.records()[i].mtime) &&
                   info.size == state.records()[i].size) {
            // The state file says we wrote exactly this, and the file still looks
            // the way we left it. No hashing required.
            need = false;
        } else if (mtimeEqual(info.mtime, e.mtime)) {
            need = false;
        } else {
            // Size matches but the timestamp moved. Only now is hashing worth it:
            // the file is probably identical content that was touched, and
            // hashing avoids rewriting it.
            auto h = hashFile(diskPaths[i]);
            need = !h || *h != e.hash;
        }

        if (need) {
            plan.entryNeeded[i] = true;
            neededFiles.fetch_add(1, std::memory_order_relaxed);
            neededBytes.fetch_add(e.size, std::memory_order_relaxed);
        }
    });

    plan.neededFiles = neededFiles.load();
    plan.neededDirs = neededDirs.load();
    plan.neededBytes = neededBytes.load();

    // Mark the blocks. A file that spans blocks marks its whole exclusive run.
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!plan.entryNeeded[i]) continue;
        const EntryDesc& e = entries[i];
        if (static_cast<EntryType>(e.type) != EntryType::File) continue;

        if (e.flags & ENTRY_FLAG_SPANS_BLOCKS) {
            for (u32 b = e.blockIndex; b < e.blockIndex + e.blockSpan; ++b)
                plan.blockNeeded[b] = true;
        } else {
            plan.blockNeeded[e.blockIndex] = true;
        }
    }

    for (std::uint8_t b : plan.blockNeeded)
        if (b) ++plan.neededBlocks;

    return plan;
}

}  // namespace hnx
