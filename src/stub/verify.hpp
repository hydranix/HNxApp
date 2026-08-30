// The skip ladder: decide, as cheaply as possible, which entries actually need
// to be written.
//
// This is the module the whole design exists to serve. A portable app is
// launched over and over against the same target directory, so the common case
// is that nothing needs doing at all, and the cost of proving that is the cost
// of launching the app.
//
// The ladder, cheapest rung first:
//   1. state.bin buildId matches   -> parallel size+mtime sweep, no hashing
//   2. no state file / different id -> per entry:
//        missing               -> need
//        size differs          -> need
//        mtime equal (+-2s)    -> skip
//        otherwise             -> hash and compare
//
// A block is decompressed only if at least one of its entries is needed, which
// is where the time is actually saved: an unchanged block is never even read
// from the mapped file.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "error.hpp"
#include "format.hpp"
#include "meta.hpp"
#include "state.hpp"
#include "toc.hpp"

namespace hnx {

// Deliberately uint8_t and not bool: std::vector<bool> is a bit-packed proxy
// type, so two threads writing different indices in the same word are a data
// race. The sweep below writes these from every worker at once.
using Flags = std::vector<std::uint8_t>;

struct VerifyPlan {
    // Parallel to Toc::entries(). Non-zero means the entry must be written.
    Flags entryNeeded;
    // Parallel to Toc::blocks(). Non-zero means the block must be decompressed.
    Flags blockNeeded;

    std::size_t neededFiles = 0;
    std::size_t neededDirs = 0;
    std::size_t neededBlocks = 0;
    u64 neededBytes = 0;

    // True when there is nothing at all to do, which is the fast path that must
    // stay in the low milliseconds.
    [[nodiscard]] bool nothingToDo() const noexcept {
        return neededFiles == 0 && neededDirs == 0;
    }
};

// Builds the plan. `state` may be invalid (first run).
[[nodiscard]] std::expected<VerifyPlan, Error> buildPlan(const Toc& toc,
                                                         const State& state,
                                                         std::wstring_view targetDir,
                                                         VerifyMode mode,
                                                         u64 buildId,
                                                         unsigned threads);

// FILETIME comparison with a tolerance.
//
// Two seconds is not arbitrary: FAT stores timestamps at 2-second granularity,
// so a tree copied to a USB stick and back comes home with mtimes up to 2s off.
// Without the tolerance every such file would hash on every launch.
[[nodiscard]] bool mtimeEqual(u64 a, u64 b) noexcept;

// Hashes a file on disk with XXH3-64, streaming so a large file does not need to
// be held in memory.
[[nodiscard]] std::expected<u64, Error> hashFile(const std::wstring& extendedPath);

}  // namespace hnx
