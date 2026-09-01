// Extraction: turn a VerifyPlan into files on disk.
//
// Work is expressed as tasks rather than as raw blocks, because the two shapes
// behave differently:
//
//   SharedBlock - one block holding many small files. Decompress once into a
//                 worker-local buffer, then write out each needed member.
//   SpanningFile - one large file occupying a run of exclusive blocks. One worker
//                 owns the whole run and streams it to disk block by block, which
//                 is what keeps peak memory at one block per worker instead of
//                 the size of the largest file in the archive.
//
// Splitting a spanning file across workers would mean several threads writing to
// one handle at different offsets, so a file is always owned end to end by a
// single worker.
#pragma once

#include <atomic>
#include <cstddef>
#include <expected>
#include <functional>
#include <string>
#include <vector>

#include "error.hpp"
#include "format.hpp"
#include "selfmap.hpp"
#include "toc.hpp"
#include "verify.hpp"

namespace hnx {

// Called periodically from worker threads, so it must be thread-safe. Used only
// to drive the progress window.
using ProgressFn = std::function<void(u64 bytesDone, u64 bytesTotal)>;

struct ExtractResult {
    std::size_t filesWritten = 0;
    std::size_t dirsCreated = 0;
    u64 bytesWritten = 0;
};

// Creates every directory the plan calls for, then writes every needed file.
//
// `diskPaths` is parallel to toc.entries() and holds the extended-form target
// path of each entry. It is produced during verification and passed through so
// the work is not repeated.
[[nodiscard]] std::expected<ExtractResult, Error> extract(
    const SelfImage& image, const Toc& toc, const VerifyPlan& plan,
    const std::vector<std::wstring>& diskPaths, unsigned threads,
    const ProgressFn& progress, const std::atomic<bool>& cancelled);

// Creates a directory and every missing parent. Returns success if it already
// exists, which is the common case on a warm target.
[[nodiscard]] std::expected<void, Error> createDirRecursive(
    const std::wstring& extendedPath);

struct PruneResult {
    std::size_t filesDeleted = 0;
    std::size_t dirsDeleted = 0;
};

// Deletes everything in the target that the payload does not contain.
//
// NOT part of a normal run, and deliberately so: files the archive does not know
// about are usually the application's own state -- profiles, settings, save data,
// logs -- and deleting them on every launch would make a portable app amnesiac.
// This runs only under an explicit --hnx-clean.
//
// The bookkeeping directory (.hnx) is always preserved.
[[nodiscard]] std::expected<PruneResult, Error> pruneOrphans(std::wstring_view targetDir,
                                                             const Toc& toc);

// Deletes the target tree and everything in it, .hnx included. Used by
// waitMode wait-and-clean once the application has exited.
//
// Best effort by design: the application may still hold a handle to something
// under the tree, and failing to remove a cache directory is not a failure worth
// surfacing to the user.
void removeTree(std::wstring_view targetDir);

}  // namespace hnx
