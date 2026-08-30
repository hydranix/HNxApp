// Codec dispatch.
//
// One entry point, so adding a codec touches exactly this file plus the id table
// in docs/format.md. The stub links decompress-only builds; the packer links the
// full libraries and adds its own compress side.
#pragma once

#include <cstddef>
#include <expected>
#include <span>

#include "error.hpp"
#include "format.hpp"

namespace hnx::codec {

// Decompresses `src` into `dst`. `dst` must already be sized to the exact
// expected output length from the TOC: every codec here is told the output size
// rather than discovering it, which means a corrupt block cannot cause an
// unbounded allocation.
//
// Returns the number of bytes written, which callers must check equals
// dst.size() -- a short read means the block disagrees with the TOC.
[[nodiscard]] std::expected<std::size_t, Error> decompress(
    u8 codecId, std::span<const std::byte> src, std::span<std::byte> dst);

// True when this build can decode the given id. Checked before extraction starts
// so an archive using a future codec fails with a clear message instead of
// part-way through writing files.
[[nodiscard]] bool isSupported(u8 codecId) noexcept;

[[nodiscard]] const char* name(u8 codecId) noexcept;

}  // namespace hnx::codec
