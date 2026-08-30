#include "selfmap.hpp"

#include <cstring>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace hnx {
namespace {

// Reads a trivially-copyable struct out of a byte span with bounds checking.
// Everything here parses attacker-controllable data, so no raw reinterpret_cast
// onto an unchecked offset is allowed.
template <typename T>
[[nodiscard]] bool readAt(std::span<const std::byte> s, u64 off, T& out) noexcept {
    if (off > s.size() || s.size() - off < sizeof(T)) return false;
    std::memcpy(&out, s.data() + off, sizeof(T));
    return true;
}

}  // namespace

std::expected<u64, Error> resolveLogicalEof(std::span<const std::byte> image) {
    IMAGE_DOS_HEADER dos{};
    if (!readAt(image, 0, dos)) return std::unexpected(Error{Ec::NotAPeFile, "no DOS header"});
    if (dos.e_magic != IMAGE_DOS_SIGNATURE)
        return std::unexpected(Error{Ec::NotAPeFile, "bad MZ signature"});
    if (dos.e_lfanew < 0)
        return std::unexpected(Error{Ec::NotAPeFile, "negative e_lfanew"});

    const auto ntOff = static_cast<u64>(dos.e_lfanew);

    DWORD sig{};
    if (!readAt(image, ntOff, sig)) return std::unexpected(Error{Ec::NotAPeFile, "no NT header"});
    if (sig != IMAGE_NT_SIGNATURE)
        return std::unexpected(Error{Ec::NotAPeFile, "bad PE signature"});

    IMAGE_FILE_HEADER fh{};
    if (!readAt(image, ntOff + sizeof(DWORD), fh))
        return std::unexpected(Error{Ec::NotAPeFile, "truncated file header"});

    const u64 optOff = ntOff + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);

    // PE32 and PE32+ put the data directory at different offsets, so read the
    // magic first rather than assuming the host architecture matches the file.
    WORD optMagic{};
    if (!readAt(image, optOff, optMagic))
        return std::unexpected(Error{Ec::NotAPeFile, "truncated optional header"});

    u64 dirOff = 0;
    u32 dirCount = 0;
    if (optMagic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        IMAGE_OPTIONAL_HEADER64 oh{};
        if (!readAt(image, optOff, oh))
            return std::unexpected(Error{Ec::NotAPeFile, "truncated PE32+ header"});
        dirCount = oh.NumberOfRvaAndSizes;
        dirOff = optOff + offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory);
    } else if (optMagic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        IMAGE_OPTIONAL_HEADER32 oh{};
        if (!readAt(image, optOff, oh))
            return std::unexpected(Error{Ec::NotAPeFile, "truncated PE32 header"});
        dirCount = oh.NumberOfRvaAndSizes;
        dirOff = optOff + offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory);
    } else {
        return std::unexpected(Error{Ec::NotAPeFile, "unknown optional header magic"});
    }

    if (dirCount > IMAGE_DIRECTORY_ENTRY_SECURITY) {
        IMAGE_DATA_DIRECTORY sec{};
        if (readAt(image,
                   dirOff + sizeof(IMAGE_DATA_DIRECTORY) * IMAGE_DIRECTORY_ENTRY_SECURITY,
                   sec)) {
            // The security directory is the one data directory whose
            // VirtualAddress is a plain file offset rather than an RVA. Anything
            // from there on is certificate, not ours.
            if (sec.VirtualAddress != 0 && sec.Size != 0 &&
                sec.VirtualAddress <= image.size()) {
                return static_cast<u64>(sec.VirtualAddress);
            }
        }
    }

    return static_cast<u64>(image.size());
}

std::expected<Footer, Error> readFooterAt(std::span<const std::byte> image,
                                          u64 logicalEof) {
    if (logicalEof < FOOTER_SIZE)
        return std::unexpected(Error{Ec::NoFooter, "file smaller than a footer"});

    Footer f{};
    if (!readAt(image, logicalEof - FOOTER_SIZE, f))
        return std::unexpected(Error{Ec::NoFooter, "cannot read footer"});

    if (!footerMagicOk(f)) return std::unexpected(Error{Ec::NoFooter});

    if (f.footerCrc32 != footerCrc(f))
        return std::unexpected(Error{Ec::FooterCrcMismatch});

    if (f.formatVersion != FORMAT_VERSION)
        return std::unexpected(Error{
            Ec::UnsupportedVersion,
            "payload format v" + std::to_string(f.formatVersion) + ", this stub speaks v" +
                std::to_string(FORMAT_VERSION)});

    // The container must sit entirely before its own footer.
    const u64 footerStart = logicalEof - FOOTER_SIZE;
    if (f.archiveOffset > footerStart || f.archiveSize > footerStart - f.archiveOffset)
        return std::unexpected(Error{Ec::TruncatedArchive, "container overruns the footer"});

    if (f.tocOffset > f.archiveSize || f.tocCompSize > f.archiveSize - f.tocOffset)
        return std::unexpected(Error{Ec::TruncatedArchive, "TOC overruns the container"});

    if (f.tocRawSize > MAX_TOC_RAW)
        return std::unexpected(Error{Ec::TocTooLarge});

    return f;
}

namespace {

// Recovery path only. A healthy build resolves through the PE headers plus the
// alignment sweep in findFooter; this exists for binaries that some third-party
// post-processor has rewritten in a way that leaves the security directory
// inconsistent.
[[nodiscard]] std::expected<Footer, Error> scanBackwardsForFooter(
    std::span<const std::byte> image) {
    if (image.size() < FOOTER_SIZE) return std::unexpected(Error{Ec::NoFooter});

    const u64 limit = image.size() < FOOTER_SCAN_LIMIT
                          ? image.size()
                          : FOOTER_SCAN_LIMIT;
    const u64 lowest = image.size() - limit;

    for (u64 end = image.size(); end >= lowest + FOOTER_SIZE; --end) {
        Footer f{};
        if (!readAt(image, end - FOOTER_SIZE, f)) continue;
        if (!footerMagicOk(f)) continue;
        if (f.footerCrc32 != footerCrc(f)) continue;
        return readFooterAt(image, end);
    }
    return std::unexpected(Error{Ec::NoFooter});
}

}  // namespace

std::expected<Footer, Error> findFooter(std::span<const std::byte> image) {
    auto eof = resolveLogicalEof(image);
    if (!eof) return std::unexpected(eof.error());

    // Try the exact end first: that is the unsigned case, and the common one.
    auto f = readFooterAt(image, *eof);
    if (f) return f;

    // A footer that was found but failed validation is a corrupt payload, not a
    // misplaced one. Searching further would be more likely to find garbage that
    // happens to checksum than to recover anything real.
    if (f.error().code != Ec::NoFooter) return std::unexpected(f.error());

    // Walk back over the Authenticode alignment padding. The certificate table
    // is 8-byte aligned, so between our footer and the certificate there can be
    // up to 7 bytes of padding that the security directory offset includes but
    // our data does not.
    for (u64 back = 1; back < CERT_ALIGNMENT; ++back) {
        if (*eof < back) break;
        auto g = readFooterAt(image, *eof - back);
        if (g) return g;
        if (g.error().code != Ec::NoFooter) return std::unexpected(g.error());
    }

    return scanBackwardsForFooter(image);
}

SelfImage::~SelfImage() { reset(); }

void SelfImage::reset() noexcept {
    if (base_) ::UnmapViewOfFile(base_);
    if (mapping_) ::CloseHandle(mapping_);
    if (file_ && file_ != INVALID_HANDLE_VALUE) ::CloseHandle(file_);
    base_ = nullptr;
    mapping_ = nullptr;
    file_ = nullptr;
    size_ = 0;
}

SelfImage::SelfImage(SelfImage&& o) noexcept
    : file_(std::exchange(o.file_, nullptr)),
      mapping_(std::exchange(o.mapping_, nullptr)),
      base_(std::exchange(o.base_, nullptr)),
      size_(std::exchange(o.size_, 0)),
      footer_(o.footer_),
      exePath_(std::move(o.exePath_)) {}

SelfImage& SelfImage::operator=(SelfImage&& o) noexcept {
    if (this != &o) {
        reset();
        file_ = std::exchange(o.file_, nullptr);
        mapping_ = std::exchange(o.mapping_, nullptr);
        base_ = std::exchange(o.base_, nullptr);
        size_ = std::exchange(o.size_, 0);
        footer_ = o.footer_;
        exePath_ = std::move(o.exePath_);
    }
    return *this;
}

std::expected<std::span<const std::byte>, Error> SelfImage::slice(
    u64 offset, u64 length) const noexcept {
    const u64 archiveSize = footer_.archiveSize;
    if (offset > archiveSize || archiveSize - offset < length)
        return std::unexpected(Error{Ec::TruncatedArchive, "slice outside container"});
    return std::span<const std::byte>{base_ + footer_.archiveOffset + offset,
                                      static_cast<std::size_t>(length)};
}

std::expected<SelfImage, Error> SelfImage::open() {
    SelfImage img;

    // GetModuleFileNameW has no documented upper bound with long paths enabled,
    // so grow until it fits rather than assuming MAX_PATH.
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, path.data(),
                                             static_cast<DWORD>(path.size()));
        if (n == 0)
            return std::unexpected(Error{Ec::SelfOpenFailed, "GetModuleFileNameW",
                                         ::GetLastError()});
        if (n < path.size()) {
            path.resize(n);
            break;
        }
        if (path.size() > 64 * 1024)
            return std::unexpected(Error{Ec::SelfOpenFailed, "path implausibly long"});
        path.resize(path.size() * 2);
    }
    img.exePath_ = path;

    // FILE_SHARE_READ matters: the file is currently executing, and other stub
    // instances of the same application will be reading it concurrently.
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return std::unexpected(Error{Ec::SelfOpenFailed, "CreateFileW", ::GetLastError()});
    img.file_ = h;

    LARGE_INTEGER li{};
    if (!::GetFileSizeEx(h, &li) || li.QuadPart <= 0)
        return std::unexpected(Error{Ec::SelfOpenFailed, "GetFileSizeEx", ::GetLastError()});

    // A payload larger than the address space cannot be mapped in one view. A
    // 32-bit stub would hit this long before a 64-bit one; fail clearly rather
    // than truncating.
    if (static_cast<unsigned long long>(li.QuadPart) >
        static_cast<unsigned long long>(SIZE_MAX))
        return std::unexpected(Error{Ec::SelfOpenFailed, "payload exceeds address space"});

    img.size_ = static_cast<std::size_t>(li.QuadPart);

    HANDLE m = ::CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!m)
        return std::unexpected(Error{Ec::SelfMapFailed, "CreateFileMappingW", ::GetLastError()});
    img.mapping_ = m;

    void* view = ::MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    if (!view)
        return std::unexpected(Error{Ec::SelfMapFailed, "MapViewOfFile", ::GetLastError()});
    img.base_ = static_cast<const std::byte*>(view);

    auto f = findFooter(img.bytes());
    if (!f) return std::unexpected(f.error());
    img.footer_ = *f;

    return img;
}

}  // namespace hnx
