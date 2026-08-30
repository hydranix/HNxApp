// Table of contents: decompress, validate, and expose as views.
//
// Everything here parses data an attacker can edit with a hex editor, so every
// field is bounds-checked before use and validation happens once, up front,
// rather than being scattered through the extractor. By the time a Toc exists,
// the extractor can trust every index and offset it holds.
#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include "error.hpp"
#include "format.hpp"

namespace hnx {

class Toc {
public:
    // Decompresses and fully validates the TOC described by `footer` out of
    // `archive` (the container region, not the whole file).
    [[nodiscard]] static std::expected<Toc, Error> parse(
        std::span<const std::byte> archive, const Footer& footer);

    [[nodiscard]] std::span<const BlockDesc> blocks() const noexcept { return blocks_; }
    [[nodiscard]] std::span<const EntryDesc> entries() const noexcept { return entries_; }
    [[nodiscard]] u64 totalRawBytes() const noexcept { return totalRawBytes_; }

    // Path of an entry, as a relative UTF-8 string with forward slashes.
    // Validated during parse, so this never returns a dangerous path.
    [[nodiscard]] std::string_view path(const EntryDesc& e) const noexcept;
    [[nodiscard]] std::string_view path(std::size_t entryIndex) const noexcept;

    [[nodiscard]] std::size_t fileCount() const noexcept { return fileCount_; }
    [[nodiscard]] std::size_t dirCount() const noexcept { return dirCount_; }

private:
    std::vector<std::byte> raw_;  // owns the decompressed TOC image
    std::span<const BlockDesc> blocks_;
    std::span<const EntryDesc> entries_;
    std::string_view pathPool_;
    u64 totalRawBytes_ = 0;
    std::size_t fileCount_ = 0;
    std::size_t dirCount_ = 0;

    [[nodiscard]] std::expected<void, Error> validate(u64 archiveSize);
};

}  // namespace hnx
