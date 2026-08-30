// TOC parsing against deliberately malformed archives.
//
// These cases cannot be produced through the filesystem: the OS resolves ".."
// when a file is created, and Windows will not create a file called CON or one
// ending in a dot. A malicious archive is just appended bytes, though, so the
// stub must reject one whether or not any packer ever validated it.
//
// The TOC is therefore built here by hand, byte for byte, and fed straight to
// the parser -- which is the same thing an attacker with a hex editor would do.
#include "test.hpp"

#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <zstd.h>

#include "format.hpp"
#include "toc.hpp"

using namespace hnx;

namespace {

// Assembles a TOC image and the footer describing it, mirroring what the packer
// emits so that a rejection can only come from the validation under test.
struct SyntheticArchive {
    std::vector<std::byte> archive;
    Footer footer{};
};

SyntheticArchive makeArchive(const std::vector<BlockDesc>& blocks,
                             const std::vector<EntryDesc>& entries,
                             const std::string& pathPool,
                             u64 blockPayloadBytes = 64) {
    TocHeader hdr{};
    hdr.entryCount = static_cast<u32>(entries.size());
    hdr.blockCount = static_cast<u32>(blocks.size());
    hdr.pathPoolSize = static_cast<u32>(pathPool.size());
    hdr.reserved = 0;
    hdr.totalRawBytes = 0;

    std::vector<std::byte> toc;
    auto push = [&toc](const void* p, std::size_t n) {
        const auto* b = static_cast<const std::byte*>(p);
        toc.insert(toc.end(), b, b + n);
    };
    push(&hdr, sizeof(hdr));
    if (!blocks.empty()) push(blocks.data(), blocks.size() * sizeof(BlockDesc));
    if (!entries.empty()) push(entries.data(), entries.size() * sizeof(EntryDesc));
    if (!pathPool.empty()) push(pathPool.data(), pathPool.size());

    std::vector<std::byte> packed(ZSTD_compressBound(toc.size()));
    const std::size_t n =
        ZSTD_compress(packed.data(), packed.size(), toc.data(), toc.size(), 3);
    packed.resize(n);

    SyntheticArchive out;
    // Block payload region first, then the TOC, matching the real layout.
    out.archive.resize(static_cast<std::size_t>(blockPayloadBytes));
    const u64 tocOffset = out.archive.size();
    out.archive.insert(out.archive.end(), packed.begin(), packed.end());

    Footer& f = out.footer;
    std::memcpy(f.magic, FOOTER_MAGIC, sizeof(FOOTER_MAGIC));
    f.formatVersion = FORMAT_VERSION;
    f.archiveOffset = 0;
    f.archiveSize = out.archive.size();
    f.tocOffset = tocOffset;
    f.tocCompSize = static_cast<u32>(packed.size());
    f.tocRawSize = static_cast<u32>(toc.size());
    f.tocCodec = static_cast<u8>(Codec::Zstd);
    f.buildId = 1;
    f.footerCrc32 = footerCrc(f);
    return out;
}

BlockDesc simpleBlock(u32 rawSize = 64) {
    BlockDesc b{};
    b.compOffset = 0;
    b.compSize = rawSize;
    b.rawSize = rawSize;
    b.codec = static_cast<u8>(Codec::Store);
    return b;
}

EntryDesc fileEntry(u32 pathOffset, u32 blockIndex = 0, u64 offset = 0, u64 size = 8) {
    EntryDesc e{};
    e.pathOffset = pathOffset;
    e.blockIndex = blockIndex;
    e.offsetInBlock = offset;
    e.size = size;
    e.type = static_cast<u8>(EntryType::File);
    return e;
}

// Builds a single-entry archive whose one path is `path`, and reports whether the
// parser accepted it.
bool parsesWithPath(const std::string& path) {
    std::string pool = path;
    pool.push_back('\0');
    auto a = makeArchive({simpleBlock()}, {fileEntry(0)}, pool);
    return Toc::parse(a.archive, a.footer).has_value();
}

}  // namespace

HNX_TEST(toc_accepts_a_well_formed_archive) {
    std::string pool = "bin/app.exe";
    pool.push_back('\0');
    auto a = makeArchive({simpleBlock()}, {fileEntry(0)}, pool);

    auto toc = Toc::parse(a.archive, a.footer);
    HNX_CHECK(toc.has_value());
    if (toc) {
        HNX_CHECK_EQ(toc->fileCount(), 1u);
        HNX_CHECK(toc->path(toc->entries()[0]) == "bin/app.exe");
    }
}

HNX_TEST(toc_rejects_traversal_paths) {
    // The cases that cannot exist on a real filesystem, which is exactly why
    // stub-side validation cannot be skipped as redundant.
    HNX_CHECK(!parsesWithPath("../evil.exe"));
    HNX_CHECK(!parsesWithPath("sub/../../evil.exe"));
    HNX_CHECK(!parsesWithPath(".."));
}

HNX_TEST(toc_rejects_absolute_paths) {
    HNX_CHECK(!parsesWithPath("C:/Windows/System32/evil.dll"));
    HNX_CHECK(!parsesWithPath("/etc/passwd"));
    HNX_CHECK(!parsesWithPath("\\\\server\\share\\evil"));
}

HNX_TEST(toc_rejects_reserved_names_and_streams) {
    HNX_CHECK(!parsesWithPath("CON"));
    HNX_CHECK(!parsesWithPath("bin/NUL"));
    HNX_CHECK(!parsesWithPath("readme.txt:hidden"));
    HNX_CHECK(!parsesWithPath("trailing."));
    HNX_CHECK(!parsesWithPath("trailing "));
}

HNX_TEST(toc_rejects_symlink_entries) {
    std::string pool = "link";
    pool.push_back('\0');
    auto e = fileEntry(0);
    e.type = static_cast<u8>(EntryType::Symlink);
    auto a = makeArchive({simpleBlock()}, {e}, pool);
    HNX_CHECK(!Toc::parse(a.archive, a.footer).has_value());
}

HNX_TEST(toc_rejects_unterminated_path_pool) {
    // No NUL terminator: a parser that trusted the offset would read past the
    // end of the pool and into whatever follows.
    auto a = makeArchive({simpleBlock()}, {fileEntry(0)}, std::string("no_nul_here"));
    HNX_CHECK(!Toc::parse(a.archive, a.footer).has_value());
}

HNX_TEST(toc_rejects_path_offset_outside_pool) {
    std::string pool = "ok";
    pool.push_back('\0');
    auto a = makeArchive({simpleBlock()}, {fileEntry(9999)}, pool);
    HNX_CHECK(!Toc::parse(a.archive, a.footer).has_value());
}

HNX_TEST(toc_rejects_block_index_out_of_range) {
    std::string pool = "a.txt";
    pool.push_back('\0');
    auto a = makeArchive({simpleBlock()}, {fileEntry(0, /*blockIndex=*/5)}, pool);
    HNX_CHECK(!Toc::parse(a.archive, a.footer).has_value());
}

HNX_TEST(toc_rejects_entry_extending_past_its_block) {
    std::string pool = "a.txt";
    pool.push_back('\0');
    // Block holds 64 bytes; the entry claims 100 starting at 0.
    auto a = makeArchive({simpleBlock(64)}, {fileEntry(0, 0, 0, 100)}, pool);
    HNX_CHECK(!Toc::parse(a.archive, a.footer).has_value());
}

HNX_TEST(toc_rejects_offset_plus_size_overflow) {
    std::string pool = "a.txt";
    pool.push_back('\0');
    // offsetInBlock + size wraps u64. Unchecked addition would produce a small
    // sum that passes a naive range test, which is the classic route from a
    // malformed header to an out-of-bounds write.
    auto e = fileEntry(0, 0, UINT64_MAX - 4, 100);
    auto a = makeArchive({simpleBlock(64)}, {e}, pool);
    HNX_CHECK(!Toc::parse(a.archive, a.footer).has_value());
}

HNX_TEST(toc_rejects_block_outside_container) {
    std::string pool = "a.txt";
    pool.push_back('\0');
    auto b = simpleBlock(64);
    b.compOffset = 1'000'000;  // far past the end of the archive
    auto a = makeArchive({b}, {fileEntry(0)}, pool);
    HNX_CHECK(!Toc::parse(a.archive, a.footer).has_value());
}

HNX_TEST(toc_rejects_stored_block_size_mismatch) {
    std::string pool = "a.txt";
    pool.push_back('\0');
    auto b = simpleBlock(64);
    b.compSize = 32;  // Store means compSize must equal rawSize
    auto a = makeArchive({b}, {fileEntry(0)}, pool);
    HNX_CHECK(!Toc::parse(a.archive, a.footer).has_value());
}

HNX_TEST(toc_rejects_oversized_block) {
    std::string pool = "a.txt";
    pool.push_back('\0');
    auto b = simpleBlock();
    b.rawSize = MAX_BLOCK_RAW + 1;
    auto a = makeArchive({b}, {fileEntry(0)}, pool);
    HNX_CHECK(!Toc::parse(a.archive, a.footer).has_value());
}

HNX_TEST(toc_rejects_unknown_codec) {
    std::string pool = "a.txt";
    pool.push_back('\0');
    auto b = simpleBlock();
    b.codec = 99;
    auto a = makeArchive({b}, {fileEntry(0)}, pool);
    HNX_CHECK(!Toc::parse(a.archive, a.footer).has_value());
}

HNX_TEST(toc_rejects_reserved_dictionary_field) {
    // dictId is reserved in v1. Refusing rather than ignoring means a future v2
    // archive fails loudly here instead of decompressing to garbage.
    std::string pool = "a.txt";
    pool.push_back('\0');
    auto b = simpleBlock();
    b.dictId = 1;
    auto a = makeArchive({b}, {fileEntry(0)}, pool);
    HNX_CHECK(!Toc::parse(a.archive, a.footer).has_value());
}

HNX_TEST(toc_rejects_section_sizes_that_do_not_fill_the_image) {
    std::string pool = "a.txt";
    pool.push_back('\0');
    auto a = makeArchive({simpleBlock()}, {fileEntry(0)}, pool);

    // Claim more entries than the image actually carries.
    auto bad = a;
    bad.footer.tocRawSize = a.footer.tocRawSize;  // unchanged
    // Rebuild with a header that lies about entryCount.
    TocHeader hdr{};
    hdr.entryCount = 50;
    hdr.blockCount = 1;
    hdr.pathPoolSize = static_cast<u32>(pool.size());
    hdr.totalRawBytes = 0;

    std::vector<std::byte> toc;
    auto push = [&toc](const void* p, std::size_t n) {
        const auto* b = static_cast<const std::byte*>(p);
        toc.insert(toc.end(), b, b + n);
    };
    const auto block = simpleBlock();
    const auto entry = fileEntry(0);
    push(&hdr, sizeof(hdr));
    push(&block, sizeof(block));
    push(&entry, sizeof(entry));
    push(pool.data(), pool.size());

    std::vector<std::byte> packed(ZSTD_compressBound(toc.size()));
    const std::size_t n =
        ZSTD_compress(packed.data(), packed.size(), toc.data(), toc.size(), 3);
    packed.resize(n);

    std::vector<std::byte> archive(64);
    const u64 tocOffset = archive.size();
    archive.insert(archive.end(), packed.begin(), packed.end());

    Footer f{};
    std::memcpy(f.magic, FOOTER_MAGIC, sizeof(FOOTER_MAGIC));
    f.formatVersion = FORMAT_VERSION;
    f.archiveSize = archive.size();
    f.tocOffset = tocOffset;
    f.tocCompSize = static_cast<u32>(packed.size());
    f.tocRawSize = static_cast<u32>(toc.size());
    f.tocCodec = static_cast<u8>(Codec::Zstd);
    f.footerCrc32 = footerCrc(f);

    HNX_CHECK(!Toc::parse(archive, f).has_value());
}

HNX_TEST(toc_rejects_directory_carrying_content) {
    std::string pool = "adir";
    pool.push_back('\0');
    auto e = fileEntry(0);
    e.type = static_cast<u8>(EntryType::Directory);
    e.size = 100;  // a directory must have no content
    auto a = makeArchive({simpleBlock()}, {e}, pool);
    HNX_CHECK(!Toc::parse(a.archive, a.footer).has_value());
}

HNX_TEST(toc_rejects_toc_larger_than_the_limit) {
    std::string pool = "a.txt";
    pool.push_back('\0');
    auto a = makeArchive({simpleBlock()}, {fileEntry(0)}, pool);
    a.footer.tocRawSize = MAX_TOC_RAW + 1;
    a.footer.footerCrc32 = footerCrc(a.footer);
    HNX_CHECK(!Toc::parse(a.archive, a.footer).has_value());
}
