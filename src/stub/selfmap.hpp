// Maps the running executable and locates the appended HNXPKG container.
//
// The subtle part is finding the footer. It is NOT simply the last 64 bytes of
// the file: signtool appends an Authenticode certificate blob after everything
// else, so a signed build has the footer buried in the middle. Since signing is
// the main defence against antivirus false positives on a self-extracting stub,
// getting this right is what makes signing possible at all.
#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <string>

#include "error.hpp"
#include "format.hpp"

namespace hnx {

class SelfImage {
public:
    SelfImage() = default;
    ~SelfImage();

    SelfImage(const SelfImage&) = delete;
    SelfImage& operator=(const SelfImage&) = delete;
    SelfImage(SelfImage&&) noexcept;
    SelfImage& operator=(SelfImage&&) noexcept;

    // Opens and maps the current executable read-only, then locates and validates
    // the footer.
    [[nodiscard]] static std::expected<SelfImage, Error> open();

    [[nodiscard]] const Footer& footer() const noexcept { return footer_; }
    [[nodiscard]] std::wstring_view exePath() const noexcept { return exePath_; }

    // The whole mapped file.
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {base_, size_};
    }

    // The container region only, i.e. bytes() offset by Footer::archiveOffset.
    // Block offsets in the TOC are relative to this.
    [[nodiscard]] std::span<const std::byte> archive() const noexcept {
        return {base_ + footer_.archiveOffset,
                static_cast<std::size_t>(footer_.archiveSize)};
    }

    // Bounds-checked slice of the container. Returns an error rather than an
    // empty span so a malformed TOC cannot be mistaken for an empty block.
    [[nodiscard]] std::expected<std::span<const std::byte>, Error> slice(
        u64 offset, u64 length) const noexcept;

private:
    void* file_ = nullptr;     // HANDLE
    void* mapping_ = nullptr;  // HANDLE
    const std::byte* base_ = nullptr;
    std::size_t size_ = 0;
    Footer footer_{};
    std::wstring exePath_;

    void reset() noexcept;
};

// Resolves the offset at which our own data ends, skipping any Authenticode
// certificate. Exposed for the packer and for tests.
//
// Note that this is an upper bound, not an exact end: see findFooter.
[[nodiscard]] std::expected<u64, Error> resolveLogicalEof(
    std::span<const std::byte> image);

// Reads and validates a footer sitting immediately before `logicalEof`.
[[nodiscard]] std::expected<Footer, Error> readFooterAt(
    std::span<const std::byte> image, u64 logicalEof);

// Locates and validates the footer in a packaged image.
//
// This is the single entry point both the stub and hnxpack must use. Having the
// packer reimplement the lookup was itself a bug: the packer reported a signed
// package as having no payload while the stub still ran it, so `hnxpack verify`
// was not verifying what the stub would actually see.
//
// Handles the Authenticode alignment gap. The certificate table must start on an
// 8-byte boundary, so signtool inserts up to 7 bytes of padding between our
// footer and the certificate. The security directory therefore points *past* the
// real end of our data, and reading at exactly (securityDir - 64) misses the
// footer by however many bytes of padding were needed.
[[nodiscard]] std::expected<Footer, Error> findFooter(std::span<const std::byte> image);

// Maximum padding Authenticode can insert before the certificate table.
inline constexpr u64 CERT_ALIGNMENT = 8;

}  // namespace hnx
