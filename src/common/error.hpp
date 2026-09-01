// Error type used across parse and extract paths.
//
// The stub parses attacker-controllable data (an appended archive can be edited
// by anyone with a hex editor), so those paths return std::expected rather than
// throwing. Exceptions stay enabled because std::filesystem and std::thread need
// them, but nothing in the hot path unwinds.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace hnx {

enum class Ec {
    Ok = 0,

    // Self / container discovery
    SelfOpenFailed,
    SelfMapFailed,
    NotAPeFile,
    NoFooter,
    FooterCrcMismatch,
    UnsupportedVersion,
    TruncatedArchive,

    // TOC
    TocTooLarge,
    TocDecompressFailed,
    TocMalformed,
    UnknownCodec,
    BlockOutOfRange,
    EntryOutOfRange,

    // Paths
    PathAbsolute,
    PathTraversal,
    PathHasStream,
    PathReservedName,
    PathTrailingDotOrSpace,
    PathEmptyComponent,
    PathNotUtf8,
    PathTooLong,
    PathEscapesTarget,
    SymlinkRejected,

    // Metadata resource
    MetaResourceMissing,
    MetaParseFailed,
    MetaFieldMissing,
    MetaFieldInvalid,

    // Extraction
    CreateDirFailed,
    CreateFileFailed,
    WriteFailed,
    SetTimeFailed,
    DecompressFailed,
    SizeMismatch,
    HashMismatch,
    LockFailed,
    TargetReadOnly,
    InsufficientSpace,

    // Launch
    ExecMissing,
    LaunchFailed,

    // Packer-side
    SourceWalkFailed,
    CompressFailed,
    ResourcePatchFailed,
    OutputWriteFailed,
    StubMissing,
};

[[nodiscard]] constexpr std::string_view describe(Ec e) noexcept {
    switch (e) {
        case Ec::Ok: return "ok";
        case Ec::SelfOpenFailed: return "cannot open own executable";
        case Ec::SelfMapFailed: return "cannot memory-map own executable";
        case Ec::NotAPeFile: return "not a valid PE file";
        case Ec::NoFooter: return "no HNXPKG payload found in this executable";
        case Ec::FooterCrcMismatch: return "payload footer failed its checksum";
        case Ec::UnsupportedVersion: return "payload was built by a newer packer";
        case Ec::TruncatedArchive: return "payload is truncated";
        case Ec::TocTooLarge: return "table of contents exceeds the size limit";
        case Ec::TocDecompressFailed: return "table of contents failed to decompress";
        case Ec::TocMalformed: return "table of contents is malformed";
        case Ec::UnknownCodec: return "payload uses a codec this build cannot decode";
        case Ec::BlockOutOfRange: return "block index out of range";
        case Ec::EntryOutOfRange: return "entry extends past its block";
        case Ec::PathAbsolute: return "archive contains an absolute path";
        case Ec::PathTraversal: return "archive contains a .. path component";
        case Ec::PathHasStream: return "archive path contains a stream separator";
        case Ec::PathReservedName: return "archive path uses a reserved device name";
        case Ec::PathTrailingDotOrSpace: return "archive path component ends in a dot or space";
        case Ec::PathEmptyComponent: return "archive path has an empty component";
        case Ec::PathNotUtf8: return "archive path is not valid UTF-8";
        case Ec::PathTooLong: return "archive path is too long";
        case Ec::PathEscapesTarget: return "archive path resolves outside the target directory";
        case Ec::SymlinkRejected: return "archive contains a symlink, which is not supported";
        case Ec::MetaResourceMissing: return "HNXMETA resource is missing";
        case Ec::MetaParseFailed: return "HNXMETA resource is not valid JSON";
        case Ec::MetaFieldMissing: return "HNXMETA is missing a required field";
        case Ec::MetaFieldInvalid: return "HNXMETA field has an invalid value";
        case Ec::CreateDirFailed: return "cannot create target directory";
        case Ec::CreateFileFailed: return "cannot create output file";
        case Ec::WriteFailed: return "write failed";
        case Ec::SetTimeFailed: return "cannot set file timestamp";
        case Ec::DecompressFailed: return "block failed to decompress";
        case Ec::SizeMismatch: return "decompressed size does not match the table of contents";
        case Ec::HashMismatch: return "content hash does not match the table of contents";
        case Ec::LockFailed: return "cannot acquire the extraction lock";
        case Ec::TargetReadOnly: return "target directory is not writable";
        case Ec::InsufficientSpace: return "not enough free space to unpack";
        case Ec::ExecMissing: return "primary executable is missing after extraction";
        case Ec::LaunchFailed: return "failed to launch the application";
        case Ec::SourceWalkFailed: return "cannot read the source directory";
        case Ec::CompressFailed: return "compression failed";
        case Ec::ResourcePatchFailed: return "cannot write the HNXMETA resource";
        case Ec::OutputWriteFailed: return "cannot write the output executable";
        case Ec::StubMissing: return "stub executable not found";
    }
    return "unknown error";
}

// Error carries an optional context string (a path, a field name, a Win32 code).
// The stub surfaces this in its error dialog, so it should read as something a
// user can act on rather than an internal code.
struct Error {
    Ec code = Ec::Ok;
    std::string context;
    std::uint32_t win32 = 0;

    Error() = default;
    explicit Error(Ec c) : code(c) {}
    Error(Ec c, std::string ctx) : code(c), context(std::move(ctx)) {}
    Error(Ec c, std::string ctx, std::uint32_t w32)
        : code(c), context(std::move(ctx)), win32(w32) {}

    [[nodiscard]] std::string message() const {
        std::string s(describe(code));
        if (!context.empty()) {
            s += ": ";
            s += context;
        }
        if (win32 != 0) {
            s += " (win32 ";
            s += std::to_string(win32);
            s += ")";
        }
        return s;
    }
};

}  // namespace hnx
