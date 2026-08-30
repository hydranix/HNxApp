// HNXPKG container format v1 - binary contract shared by hnxstub and hnxpack.
// Prose rationale lives in docs/format.md. When the two disagree, this file wins.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace hnx {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// ---------------------------------------------------------------------------
// Limits
//
// These are validated against untrusted input before any allocation, so they
// double as a denial-of-service bound. Keep them generous enough for real apps
// and tight enough that a corrupt header cannot ask for gigabytes.
// ---------------------------------------------------------------------------

inline constexpr u32 DEFAULT_BLOCK_RAW = 8u * 1024 * 1024;   // 8 MiB target
inline constexpr u32 MAX_BLOCK_RAW = 64u * 1024 * 1024;      // hard ceiling
inline constexpr u32 MAX_TOC_RAW = 256u * 1024 * 1024;       // ~2.7M entries
inline constexpr u32 MAX_PATH_BYTES = 32u * 1024;            // per entry, UTF-8
inline constexpr u64 MAX_ENTRY_COUNT = 8u * 1000 * 1000;
inline constexpr u64 FOOTER_SCAN_LIMIT = 64ull * 1024 * 1024;

// ---------------------------------------------------------------------------
// Codecs
// ---------------------------------------------------------------------------

enum class Codec : u8 {
    Store = 0,
    Lz4 = 1,
    Zstd = 2,
    Lzma = 3,    // reserved
    Brotli = 4,  // reserved
};

inline constexpr u8 CODEC_MAX_KNOWN = 2;  // highest id the v1 stub can decode

// ---------------------------------------------------------------------------
// Footer
// ---------------------------------------------------------------------------

inline constexpr u8 FOOTER_MAGIC[8] = {'H', 'N', 'X', 'P', 'K', 'G', 0x1A, 0x00};

enum FooterFlags : u32 {
    FOOTER_FLAG_HAS_DIRS = 1u << 0,
    FOOTER_FLAG_RESERVED_DICT = 1u << 1,
};

#pragma pack(push, 1)

struct Footer {
    u8 magic[8];
    u32 formatVersion;
    u32 flags;
    u64 archiveOffset;
    u64 archiveSize;
    u64 tocOffset;
    u32 tocCompSize;
    u32 tocRawSize;
    u8 tocCodec;
    u8 reserved[3];
    u64 buildId;
    u32 footerCrc32;
};
static_assert(sizeof(Footer) == 64, "Footer must be exactly 64 bytes");
static_assert(offsetof(Footer, footerCrc32) == 60, "CRC must be the last field");

inline constexpr u32 FORMAT_VERSION = 1;
inline constexpr u32 FOOTER_SIZE = sizeof(Footer);

// ---------------------------------------------------------------------------
// TOC
// ---------------------------------------------------------------------------

struct TocHeader {
    u32 entryCount;
    u32 blockCount;
    u32 pathPoolSize;
    u32 reserved;  // keeps totalRawBytes and BlockDesc[] 8-byte aligned
    u64 totalRawBytes;
};
static_assert(sizeof(TocHeader) == 24, "TocHeader must be 24 bytes");

enum BlockFlags : u8 {
    BLOCK_FLAG_EXCLUSIVE = 1u << 0,  // part of a SPANS_BLOCKS run
};

struct BlockDesc {
    u64 compOffset;  // relative to Footer::archiveOffset
    u32 compSize;
    u32 rawSize;
    u8 codec;
    u8 flags;
    u16 dictId;  // 0 = none; reserved so dictionaries are not a format break
    u32 reserved;
};
static_assert(sizeof(BlockDesc) == 24, "BlockDesc must be 24 bytes");

enum class EntryType : u8 {
    File = 0,
    Directory = 1,
    Symlink = 2,  // rejected in v1
};

enum EntryFlags : u8 {
    ENTRY_FLAG_SPANS_BLOCKS = 1u << 0,
    ENTRY_FLAG_EXECUTABLE = 1u << 1,
};

inline constexpr u32 NO_BLOCK = 0xFFFFFFFFu;

struct EntryDesc {
    u32 pathOffset;
    u32 blockIndex;  // NO_BLOCK for directories
    u64 offsetInBlock;
    u64 size;
    u64 mtime;  // FILETIME: 100ns ticks since 1601-01-01 UTC
    u64 hash;   // XXH3-64 of contents; 0 for directories
    u32 attrs;
    u8 type;
    u8 flags;
    u16 blockSpan;  // block count when SPANS_BLOCKS, else 0
};
static_assert(sizeof(EntryDesc) == 48, "EntryDesc must be 48 bytes");

// ---------------------------------------------------------------------------
// State file
// ---------------------------------------------------------------------------

inline constexpr u8 STATE_MAGIC[8] = {'H', 'N', 'X', 'S', 'T', 'A', 'T', 'E'};

struct StateHeader {
    u8 magic[8];
    u32 version;
    u32 reserved;
    u64 buildId;
    u32 entryCount;
    u32 reserved2;
};
static_assert(sizeof(StateHeader) == 32, "StateHeader must be 32 bytes");

struct StateRecord {
    u64 size;
    u64 mtime;
    u64 hash;
};
static_assert(sizeof(StateRecord) == 24, "StateRecord must be 24 bytes");

inline constexpr u32 STATE_VERSION = 1;

#pragma pack(pop)

// ---------------------------------------------------------------------------
// Attributes
//
// Only these round-trip. DIRECTORY is implied by EntryType; REPARSE_POINT and
// ENCRYPTED would fail or misbehave on restore; the rest carry no meaning for a
// freshly extracted tree.
// ---------------------------------------------------------------------------

inline constexpr u32 ATTR_READONLY = 0x00000001;
inline constexpr u32 ATTR_HIDDEN = 0x00000002;
inline constexpr u32 ATTR_SYSTEM = 0x00000004;
inline constexpr u32 ATTR_ARCHIVE = 0x00000020;
inline constexpr u32 ATTR_MASK =
    ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_ARCHIVE;

// ---------------------------------------------------------------------------
// CRC-32 (IEEE), used only for the footer.
//
// Constexpr table so the stub carries no initialisation cost and no static
// constructor.
// ---------------------------------------------------------------------------

namespace detail {
struct Crc32Table {
    u32 v[256]{};
    constexpr Crc32Table() {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            v[i] = c;
        }
    }
};
inline constexpr Crc32Table CRC32_TABLE{};
}  // namespace detail

[[nodiscard]] inline constexpr u32 crc32(std::span<const std::byte> data,
                                         u32 seed = 0) noexcept {
    u32 c = seed ^ 0xFFFFFFFFu;
    for (std::byte b : data)
        c = detail::CRC32_TABLE.v[(c ^ static_cast<u8>(b)) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

[[nodiscard]] inline u32 footerCrc(const Footer& f) noexcept {
    return crc32({reinterpret_cast<const std::byte*>(&f), offsetof(Footer, footerCrc32)});
}

[[nodiscard]] inline bool footerMagicOk(const Footer& f) noexcept {
    for (int i = 0; i < 8; ++i)
        if (f.magic[i] != FOOTER_MAGIC[i]) return false;
    return true;
}

}  // namespace hnx
