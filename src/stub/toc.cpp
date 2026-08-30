#include "toc.hpp"

#include <cstring>

#include "codec/codec.hpp"
#include "paths.hpp"

namespace hnx {
namespace {

// Checked a + b for u64, so an overflow cannot wrap into a small value that then
// passes a range test. This is the arithmetic that turns a malformed TOC into an
// out-of-bounds write, so it gets its own helper rather than being open-coded.
[[nodiscard]] constexpr bool addOverflows(u64 a, u64 b, u64& out) noexcept {
    if (a > UINT64_MAX - b) return true;
    out = a + b;
    return false;
}

}  // namespace

std::expected<Toc, Error> Toc::parse(std::span<const std::byte> archive,
                                     const Footer& footer) {
    if (footer.tocRawSize > MAX_TOC_RAW)
        return std::unexpected(Error{Ec::TocTooLarge});
    if (footer.tocRawSize < sizeof(TocHeader))
        return std::unexpected(Error{Ec::TocMalformed, "TOC smaller than its header"});

    if (!codec::isSupported(footer.tocCodec))
        return std::unexpected(Error{Ec::UnknownCodec, codec::name(footer.tocCodec)});

    u64 tocEnd = 0;
    if (addOverflows(footer.tocOffset, footer.tocCompSize, tocEnd) ||
        tocEnd > archive.size())
        return std::unexpected(Error{Ec::TruncatedArchive, "TOC outside container"});

    const auto src = archive.subspan(static_cast<std::size_t>(footer.tocOffset),
                                     footer.tocCompSize);

    Toc toc;
    toc.raw_.resize(footer.tocRawSize);

    auto n = codec::decompress(footer.tocCodec, src, toc.raw_);
    if (!n) return std::unexpected(n.error());
    if (*n != footer.tocRawSize)
        return std::unexpected(Error{Ec::TocDecompressFailed, "size disagrees with footer"});

    if (auto v = toc.validate(footer.archiveSize); !v)
        return std::unexpected(v.error());

    return toc;
}

std::expected<void, Error> Toc::validate(u64 archiveSize) {
    const std::byte* base = raw_.data();
    const std::size_t total = raw_.size();

    TocHeader hdr{};
    std::memcpy(&hdr, base, sizeof(hdr));

    if (hdr.entryCount > MAX_ENTRY_COUNT)
        return std::unexpected(Error{Ec::TocMalformed, "implausible entry count"});

    // Lay out the three arrays and confirm they fit, using checked arithmetic so
    // a crafted count cannot wrap the total back into range.
    u64 need = sizeof(TocHeader);
    u64 blocksBytes = static_cast<u64>(hdr.blockCount) * sizeof(BlockDesc);
    u64 entriesBytes = static_cast<u64>(hdr.entryCount) * sizeof(EntryDesc);

    if (addOverflows(need, blocksBytes, need))
        return std::unexpected(Error{Ec::TocMalformed, "block table overflows"});
    const u64 blocksOff = sizeof(TocHeader);

    const u64 entriesOff = need;
    if (addOverflows(need, entriesBytes, need))
        return std::unexpected(Error{Ec::TocMalformed, "entry table overflows"});

    const u64 poolOff = need;
    if (addOverflows(need, hdr.pathPoolSize, need))
        return std::unexpected(Error{Ec::TocMalformed, "path pool overflows"});

    if (need != total)
        return std::unexpected(Error{Ec::TocMalformed, "section sizes do not fill the TOC"});

    blocks_ = {reinterpret_cast<const BlockDesc*>(base + blocksOff), hdr.blockCount};
    entries_ = {reinterpret_cast<const EntryDesc*>(base + entriesOff), hdr.entryCount};
    pathPool_ = {reinterpret_cast<const char*>(base + poolOff), hdr.pathPoolSize};
    totalRawBytes_ = hdr.totalRawBytes;

    // --- blocks -------------------------------------------------------------
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        const BlockDesc& b = blocks_[i];

        if (b.rawSize > MAX_BLOCK_RAW)
            return std::unexpected(Error{Ec::TocMalformed, "block exceeds the size ceiling"});
        if (!codec::isSupported(b.codec))
            return std::unexpected(Error{Ec::UnknownCodec, codec::name(b.codec)});

        u64 end = 0;
        if (addOverflows(b.compOffset, b.compSize, end) || end > archiveSize)
            return std::unexpected(Error{Ec::BlockOutOfRange, "block outside container"});

        // A stored block must be exactly as long as its content; anything else
        // means the two disagree about how many bytes to copy.
        if (static_cast<Codec>(b.codec) == Codec::Store && b.compSize != b.rawSize)
            return std::unexpected(Error{Ec::TocMalformed, "stored block size mismatch"});

        // dictId is reserved in v1. Refuse rather than ignore, so a v2 archive
        // does not silently decompress to garbage here.
        if (b.dictId != 0)
            return std::unexpected(Error{Ec::UnsupportedVersion, "block uses a dictionary"});
    }

    // --- entries ------------------------------------------------------------
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const EntryDesc& e = entries_[i];

        // Path must be NUL-terminated inside the pool before it can be read.
        if (e.pathOffset >= pathPool_.size())
            return std::unexpected(Error{Ec::TocMalformed, "path offset outside pool"});
        const std::size_t nul = pathPool_.find('\0', e.pathOffset);
        if (nul == std::string_view::npos)
            return std::unexpected(Error{Ec::TocMalformed, "unterminated path"});

        const std::string_view p = pathPool_.substr(e.pathOffset, nul - e.pathOffset);
        if (auto v = paths::validateArchivePath(p); !v)
            return std::unexpected(v.error());

        switch (static_cast<EntryType>(e.type)) {
            case EntryType::Directory: {
                if (e.blockIndex != NO_BLOCK || e.size != 0)
                    return std::unexpected(Error{Ec::TocMalformed, "directory carries content"});
                ++dirCount_;
                continue;
            }
            case EntryType::Symlink:
                return std::unexpected(Error{Ec::SymlinkRejected, std::string(p)});
            case EntryType::File:
                break;
            default:
                return std::unexpected(Error{Ec::TocMalformed, "unknown entry type"});
        }

        ++fileCount_;

        if (e.blockIndex >= blocks_.size())
            return std::unexpected(Error{Ec::BlockOutOfRange, std::string(p)});

        if (e.flags & ENTRY_FLAG_SPANS_BLOCKS) {
            // A spanning file owns a run of consecutive exclusive blocks. Verify
            // the run exists and that its capacity actually covers the file, or
            // the extractor would read past the last block while streaming.
            if (e.blockSpan == 0)
                return std::unexpected(Error{Ec::TocMalformed, "spanning entry with no blocks"});
            if (e.offsetInBlock != 0)
                return std::unexpected(Error{Ec::TocMalformed, "spanning entry is not block-aligned"});

            u64 last = 0;
            if (addOverflows(e.blockIndex, e.blockSpan, last) || last > blocks_.size())
                return std::unexpected(Error{Ec::BlockOutOfRange, std::string(p)});

            u64 capacity = 0;
            for (u64 b = e.blockIndex; b < last; ++b) {
                if (!(blocks_[static_cast<std::size_t>(b)].flags & BLOCK_FLAG_EXCLUSIVE))
                    return std::unexpected(
                        Error{Ec::TocMalformed, "spanning run includes a shared block"});
                if (addOverflows(capacity, blocks_[static_cast<std::size_t>(b)].rawSize,
                                 capacity))
                    return std::unexpected(Error{Ec::TocMalformed, "run capacity overflows"});
            }
            if (capacity != e.size)
                return std::unexpected(
                    Error{Ec::EntryOutOfRange, "run capacity does not match file size"});
        } else {
            const BlockDesc& b = blocks_[e.blockIndex];
            u64 end = 0;
            if (addOverflows(e.offsetInBlock, e.size, end) || end > b.rawSize)
                return std::unexpected(Error{Ec::EntryOutOfRange, std::string(p)});
        }
    }

    return {};
}

std::string_view Toc::path(const EntryDesc& e) const noexcept {
    // validate() proved every offset is in range and NUL-terminated.
    const char* s = pathPool_.data() + e.pathOffset;
    return std::string_view{s};
}

std::string_view Toc::path(std::size_t entryIndex) const noexcept {
    return path(entries_[entryIndex]);
}

}  // namespace hnx
