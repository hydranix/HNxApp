// XXH3-64 wrapper.
//
// Used for the "is this file different?" check, not as a security boundary: the
// question is whether a file on disk still matches the one in the archive, and a
// non-cryptographic hash answers that far faster than a cryptographic one. An
// attacker who can write into the target directory has already won by other
// means.
//
// The include path is deliberately hnx_xxhash/: zstd and lz4 both vendor older
// xxhash.h files whose directories are also on the include path, and a bare
// <xxhash.h> silently resolves to whichever comes first. See the xxhash section
// of the root CMakeLists.
#pragma once

#include <hnx_xxhash/xxhash.h>

#include <cstddef>
#include <span>

#include "format.hpp"

namespace hnx {

[[nodiscard]] inline u64 hashBytes(std::span<const std::byte> data) noexcept {
    return XXH3_64bits(data.data(), data.size());
}

// Streaming form, for files read in chunks rather than mapped whole. Keeping the
// state in a small RAII wrapper avoids an XXH3_freeState leak on every early
// return in the verify path.
class Hasher {
public:
    Hasher() noexcept { XXH3_64bits_reset(&state_); }

    void update(std::span<const std::byte> data) noexcept {
        XXH3_64bits_update(&state_, data.data(), data.size());
    }

    [[nodiscard]] u64 digest() const noexcept { return XXH3_64bits_digest(&state_); }

    void reset() noexcept { XXH3_64bits_reset(&state_); }

private:
    // XXH_INLINE_ALL makes the state type available directly, so there is no heap
    // allocation and no matching free to forget.
    XXH3_state_t state_{};
};

}  // namespace hnx
