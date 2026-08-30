// The extraction state file: <targetDir>\.hnx\state.bin
//
// Records which payload wrote this directory and what each file looked like when
// it was written. Its whole purpose is to turn "is this directory already
// correct?" into one sequential read instead of thousands of stat calls.
//
// Two invariants make it safe to trust:
//   - it is written LAST, after every file is on disk, so its presence means the
//     extraction completed;
//   - it is written to a temp name and renamed, so it is never observed
//     half-written.
//
// A crash therefore leaves no state file at all, and the next run falls back to
// full verification rather than trusting a partially extracted tree.
#pragma once

#include <expected>
#include <span>
#include <string>
#include <vector>

#include "error.hpp"
#include "format.hpp"

namespace hnx {

class State {
public:
    // Loads the state file for `targetDir`. A missing file is NOT an error: it is
    // the normal first-run case, and the caller distinguishes it via valid().
    [[nodiscard]] static State load(std::wstring_view targetDir);

    // True when a well-formed state file was read whose buildId matches and whose
    // record count matches the TOC.
    [[nodiscard]] bool matches(u64 buildId, std::size_t entryCount) const noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] u64 buildId() const noexcept { return buildId_; }
    [[nodiscard]] std::span<const StateRecord> records() const noexcept { return records_; }

    // Writes the state file atomically. `records` must be in TOC entry order.
    [[nodiscard]] static std::expected<void, Error> store(
        std::wstring_view targetDir, u64 buildId, std::span<const StateRecord> records);

    // Removes the state file, forcing a full verification next run. Used when an
    // extraction fails part-way so the tree is never mistaken for complete.
    static void invalidate(std::wstring_view targetDir) noexcept;

    // <targetDir>\.hnx
    [[nodiscard]] static std::wstring metaDir(std::wstring_view targetDir);

private:
    bool valid_ = false;
    u64 buildId_ = 0;
    std::vector<StateRecord> records_;
};

}  // namespace hnx
