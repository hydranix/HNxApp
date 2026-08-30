// Footer location, including the Authenticode alignment case.
//
// This file exists because of a real bug. Signing a package appended a
// certificate and left the footer four bytes before where the security directory
// pointed, so hnxpack reported "no payload" on a signed build while the stub
// still ran it (the stub survived only via its wide backward-scan recovery path,
// which masked the failure).
//
// The cause is that the Authenticode certificate table must start on an 8-byte
// boundary, so signtool inserts up to 7 bytes of padding between the end of our
// data and the certificate. The security directory offset includes that padding;
// our footer does not.
#include "test.hpp"

#include <cstring>
#include <span>
#include <vector>

#include <windows.h>

#include "format.hpp"
#include "selfmap.hpp"

using namespace hnx;

namespace {

// Builds a minimal but structurally valid PE64 image with an optional security
// directory, so findFooter can be exercised without needing a real signed
// binary on disk.
std::vector<std::byte> makePeImage(std::size_t bodyBytes, u32 securityVa,
                                   u32 securitySize) {
    const std::size_t ntOffset = 0x80;
    const std::size_t headerBytes =
        ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
        sizeof(IMAGE_OPTIONAL_HEADER64);

    std::vector<std::byte> img(headerBytes + bodyBytes, std::byte{0});

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(img.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = static_cast<LONG>(ntOffset);

    auto* sig = reinterpret_cast<DWORD*>(img.data() + ntOffset);
    *sig = IMAGE_NT_SIGNATURE;

    auto* fh = reinterpret_cast<IMAGE_FILE_HEADER*>(img.data() + ntOffset + sizeof(DWORD));
    fh->Machine = IMAGE_FILE_MACHINE_AMD64;
    fh->SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);

    auto* oh = reinterpret_cast<IMAGE_OPTIONAL_HEADER64*>(
        img.data() + ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER));
    oh->Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    oh->NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    oh->DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY].VirtualAddress = securityVa;
    oh->DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY].Size = securitySize;

    return img;
}

Footer makeFooter(u64 buildId = 0x1234) {
    Footer f{};
    std::memcpy(f.magic, FOOTER_MAGIC, sizeof(FOOTER_MAGIC));
    f.formatVersion = FORMAT_VERSION;
    f.archiveOffset = 0;
    f.archiveSize = 0;
    f.tocOffset = 0;
    f.tocCompSize = 0;
    f.tocRawSize = sizeof(TocHeader);
    f.tocCodec = static_cast<u8>(Codec::Zstd);
    f.buildId = buildId;
    f.footerCrc32 = footerCrc(f);
    return f;
}

void placeFooter(std::vector<std::byte>& img, std::size_t at, const Footer& f) {
    std::memcpy(img.data() + at, &f, sizeof(Footer));
}

}  // namespace

HNX_TEST(finds_footer_at_end_of_unsigned_image) {
    auto img = makePeImage(/*bodyBytes=*/256, /*securityVa=*/0, /*securitySize=*/0);
    const auto f = makeFooter(0xAAAA);
    placeFooter(img, img.size() - sizeof(Footer), f);

    auto got = findFooter(img);
    HNX_CHECK(got.has_value());
    if (got) HNX_CHECK_EQ(got->buildId, 0xAAAAull);
}

HNX_TEST(finds_footer_through_authenticode_alignment_padding) {
    // The regression this file exists for. For every legal amount of alignment
    // padding, the security directory points `pad` bytes past our footer.
    for (u64 pad = 0; pad < CERT_ALIGNMENT; ++pad) {
        const std::size_t body = 256;
        const std::size_t certBytes = 1408;

        auto img = makePeImage(body + pad + certBytes, 0, 0);

        // Our data ends here, immediately after the footer.
        const std::size_t footerEnd = img.size() - certBytes - pad;
        placeFooter(img, footerEnd - sizeof(Footer), makeFooter(0xBEEF00 + pad));

        // The certificate starts after the padding, and that is what the security
        // directory records.
        auto* oh = reinterpret_cast<IMAGE_OPTIONAL_HEADER64*>(
            img.data() + 0x80 + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER));
        oh->DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY].VirtualAddress =
            static_cast<DWORD>(footerEnd + pad);
        oh->DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY].Size =
            static_cast<DWORD>(certBytes);

        auto got = findFooter(img);
        HNX_CHECK(got.has_value());
        if (got) HNX_CHECK_EQ(got->buildId, 0xBEEF00ull + pad);
    }
}

HNX_TEST(security_directory_bounds_the_search) {
    // Without honouring the security directory the lookup would read into the
    // certificate blob rather than our footer.
    auto img = makePeImage(256 + 1408, 0, 0);
    const std::size_t footerEnd = img.size() - 1408;
    placeFooter(img, footerEnd - sizeof(Footer), makeFooter(0xCAFE));

    auto* oh = reinterpret_cast<IMAGE_OPTIONAL_HEADER64*>(
        img.data() + 0x80 + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER));
    oh->DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY].VirtualAddress =
        static_cast<DWORD>(footerEnd);
    oh->DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY].Size = 1408;

    auto eof = resolveLogicalEof(img);
    HNX_CHECK(eof.has_value());
    if (eof) HNX_CHECK_EQ(*eof, static_cast<u64>(footerEnd));

    auto got = findFooter(img);
    HNX_CHECK(got.has_value());
    if (got) HNX_CHECK_EQ(got->buildId, 0xCAFEull);
}

HNX_TEST(reports_no_payload_for_a_plain_executable) {
    auto img = makePeImage(4096, 0, 0);
    auto got = findFooter(img);
    HNX_CHECK(!got.has_value());
    if (!got) HNX_CHECK(got.error().code == Ec::NoFooter);
}

HNX_TEST(rejects_a_corrupt_footer_rather_than_searching_past_it) {
    // A bad CRC means a damaged payload. Continuing to search would be more
    // likely to find garbage that happens to validate than to recover anything,
    // and would turn a clear error into a confusing one.
    auto img = makePeImage(256, 0, 0);
    auto f = makeFooter();
    f.archiveOffset = 999;  // invalidates the CRC without touching the magic
    placeFooter(img, img.size() - sizeof(Footer), f);

    auto got = findFooter(img);
    HNX_CHECK(!got.has_value());
    if (!got) HNX_CHECK(got.error().code == Ec::FooterCrcMismatch);
}

HNX_TEST(rejects_a_future_format_version) {
    auto img = makePeImage(256, 0, 0);
    auto f = makeFooter();
    f.formatVersion = FORMAT_VERSION + 1;
    f.footerCrc32 = footerCrc(f);
    placeFooter(img, img.size() - sizeof(Footer), f);

    auto got = findFooter(img);
    HNX_CHECK(!got.has_value());
    if (!got) HNX_CHECK(got.error().code == Ec::UnsupportedVersion);
}

HNX_TEST(rejects_a_container_that_overruns_its_own_footer) {
    auto img = makePeImage(256, 0, 0);
    auto f = makeFooter();
    f.archiveOffset = 0;
    f.archiveSize = 1'000'000;  // far larger than the image
    f.footerCrc32 = footerCrc(f);
    placeFooter(img, img.size() - sizeof(Footer), f);

    auto got = findFooter(img);
    HNX_CHECK(!got.has_value());
    if (!got) HNX_CHECK(got.error().code == Ec::TruncatedArchive);
}

HNX_TEST(rejects_non_pe_input) {
    std::vector<std::byte> notPe(1024, std::byte{0x41});
    auto got = findFooter(notPe);
    HNX_CHECK(!got.has_value());
}
