// Shared types for hnxpack.
//
// The packer is a build-time developer tool, so unlike the stub it is free to
// use <print>, <format> and std::filesystem: binary size does not matter here.
#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "error.hpp"
#include "format.hpp"

namespace hnx::pack {

// One thing the user added to the package: a folder whose contents are included,
// or a single file.
//
// A package is built from a list of these rather than one root directory, because
// applications routinely need something that does not live in their own folder --
// a DLL from elsewhere, a config file, a licence.
struct SourceItem {
    std::filesystem::path hostPath;  // file or directory on disk

    // Where its contents land inside the package. Empty puts a folder's contents
    // at the package root, which is what a single-folder package wants. A second
    // folder normally gets its own name here so the two cannot collide.
    std::string archivePrefix;

    [[nodiscard]] bool isDirectory() const;
};

struct SourceEntry {
    std::string relPath;    // UTF-8, forward slashes, relative, validated
    std::wstring absPath;   // extended form, for reading
    u64 size = 0;
    u64 mtime = 0;
    u32 attrs = 0;
    u64 hash = 0;
    bool isDir = false;
    // Marks the primary entry point, so the stub fast verify mode knows which
    // single file to check.
    bool isExec = false;

    // Which SourceItem contributed this entry, so the UI can show and remove a
    // whole source at once.
    std::size_t sourceIndex = 0;

    // Cleared by the UI to leave a file out of the package.
    //
    // Excluding is not deleting: the file stays exactly where it is on disk. The
    // whole point of the tree is to choose what goes in, not to edit the source.
    bool included = true;
};

// One block of the output container, before compression.
struct PlannedBlock {
    std::vector<std::size_t> members;  // indices into the entry list
    u64 rawSize = 0;
    bool exclusive = false;            // part of a spanning-file run
    std::size_t spanningEntry = SIZE_MAX;
};

// Which embedded stub to build the package around.
//
// Normally matched to the entry point: a 32-bit application gets a 32-bit stub,
// so the package runs anywhere the application itself would, including 32-bit
// Windows. x64 is the fallback when the entry point is not a PE at all -- a .bat
// or a .lnk says nothing about architecture.
//
// A 32-bit stub maps the whole payload into a 32-bit address space, so it cannot
// carry an archive near 2 GB (3 GB with /LARGEADDRESSAWARE, which the stub sets).
// That is the cost of matching, and it only bites on very large 32-bit packages.
enum class StubArch { X64, X86 };

[[nodiscard]] const char* stubArchName(StubArch arch);

// Reads the PE machine type of `exe` and maps it to the stub that matches.
//
// Returns nothing when the file is not a PE image, which is a normal case rather
// than an error: .bat, .cmd and .lnk entry points are all supported and none of
// them carries an architecture.
[[nodiscard]] std::optional<StubArch> detectTargetArch(const std::filesystem::path& exe);

// Progress and cancellation, so a GUI can drive a build without scraping stdout.
//
// Every callback may be empty. `cancelled` is polled from worker threads, so it
// must be safe to call concurrently; the others are invoked from the calling
// thread only.
struct BuildCallbacks {
    std::function<void(std::string_view line)> log;
    std::function<void(std::string_view phase, u64 done, u64 total)> progress;
    std::function<bool()> cancelled;

    [[nodiscard]] bool isCancelled() const { return cancelled && cancelled(); }
    void note(std::string_view line) const {
        if (log) log(line);
    }
    void step(std::string_view phase, u64 done, u64 total) const {
        if (progress) progress(phase, done, total);
    }
};

struct BuildOptions {
    // What goes into the package. A single entry with an empty prefix reproduces
    // the old single-directory behaviour exactly.
    std::vector<SourceItem> sources;

    // Entries to leave out, by archive path. Populated from the GUI tree.
    // Matched case-insensitively, since the paths it excludes are Windows paths.
    std::vector<std::string> excluded;

    std::filesystem::path output;
    // An explicit stub file. Empty means use the one embedded in hnxpack, which
    // is the normal case.
    std::filesystem::path stub;

    // Only consulted when archExplicit is set. Otherwise the architecture is
    // taken from the entry point, falling back to X64.
    StubArch arch = StubArch::X64;
    bool archExplicit = false;

    std::filesystem::path configFile;

    // Metadata supplied directly rather than read from configFile. The
    // interactive flow derives its metadata from what the user picked in the
    // dialogs, so it never has a file to point at.
    std::string inlineConfig;

    Codec codec = Codec::Zstd;
    int level = 19;
    u32 blockSize = DEFAULT_BLOCK_RAW;
    unsigned threads = 0;  // 0 = hardware concurrency
    bool verbose = false;

    // Icons are taken from the resolved entry point unless overridden here, so a
    // packaged application looks like itself in Explorer instead of showing a
    // blank stub icon.
    std::filesystem::path iconSource;  // empty = use the entry point
    bool noIcon = false;

    // Report failures in a dialog as well as on stderr.
    //
    // Set by the interactive flow and by --no-console: in both cases stderr goes
    // somewhere the user cannot see, and a build tool that fails with no visible
    // output at all is indistinguishable from one that did nothing.
    bool reportInDialogs = false;

    // Test-only. Skips pack-time path validation so the safety tests can build
    // deliberately malformed archives and confirm the stub refuses them.
    bool allowUnsafePaths = false;
};

// Formats a byte count for human output. Lives with the build pipeline because
// every command reports sizes.
[[nodiscard]] std::string humanBytes(u64 n);

// --- build.cpp --------------------------------------------------------------

// Runs the whole build pipeline. Returns a process exit code.
[[nodiscard]] int buildPackage(BuildOptions opt, const BuildCallbacks& cb = {});

// --- embedded_stub.cpp ------------------------------------------------------

// True when this hnxpack carries a stub for `arch`.
//
// Worth checking early. The interactive flow asks three questions before it
// builds anything, and discovering a missing stub at the end means the user
// answered all of them for nothing.
[[nodiscard]] bool hasEmbeddedStub(StubArch arch);

// Writes the embedded stub for `arch` to `output`, replacing whatever is there.
[[nodiscard]] std::expected<void, Error> writeEmbeddedStub(
    StubArch arch, const std::filesystem::path& output);

// --- walk.cpp ---------------------------------------------------------------

// Enumerates every source item into one archive namespace, validating each path
// and hashing each file. Returns entries with directories before the files they
// contain, which is what lets the stub create the tree in a single pass.
//
// Two items producing the same archive path is an error rather than a silent
// overwrite: which one won would depend on enumeration order.
[[nodiscard]] std::expected<std::vector<SourceEntry>, Error> walkSources(
    std::span<const SourceItem> items, unsigned threads, bool allowUnsafePaths,
    const BuildCallbacks& cb = {});

// Convenience wrapper for a single directory at the package root.
[[nodiscard]] std::expected<std::vector<SourceEntry>, Error> walkSource(
    const std::filesystem::path& root, unsigned threads, bool allowUnsafePaths);

// Suggests where a newly added item should land, given what is already there.
//
// The first folder contributes its contents at the root, which is what a
// conventional single-folder package expects. Anything added afterwards gets its
// own name as a prefix, so unrelated sources cannot collide by accident.
[[nodiscard]] std::string suggestArchivePrefix(const std::filesystem::path& hostPath,
                                               std::span<const SourceItem> existing);

// --- blockize.cpp -----------------------------------------------------------

// Groups files into blocks.
//
// Sorted by (extension, parent, name) first: clustering similar files inside one
// solid block is worth a large amount of compression ratio, and costs nothing
// but the sort.
[[nodiscard]] std::vector<PlannedBlock> planBlocks(std::vector<SourceEntry>& entries,
                                                   u32 blockSize);

// --- compress.cpp -----------------------------------------------------------

struct BuiltArchive {
    std::vector<std::byte> bytes;  // blocks followed by the compressed TOC
    Footer footer{};               // archiveOffset filled in by the caller
};

[[nodiscard]] std::expected<BuiltArchive, Error> buildArchive(
    const std::vector<SourceEntry>& entries, const std::vector<PlannedBlock>& blocks,
    const BuildOptions& opt, u64 buildId);

// Compresses one buffer, falling back to Store when compression does not
// actually help. A block of already-compressed content (PNG, ZIP, MP4) otherwise
// costs CPU on both sides to end up slightly larger.
[[nodiscard]] std::expected<std::vector<std::byte>, Error> compressBlock(
    std::span<const std::byte> raw, Codec codec, int level, Codec& usedCodec);

// --- patch_rsrc.cpp ---------------------------------------------------------

// Writes the HNXMETA resource into `exePath`, and optionally transplants the
// icon resources from `iconSource`.
//
// Both happen in one BeginUpdateResource/EndUpdateResource session on purpose.
// EndUpdateResourceW rewrites the entire file, so doing them separately would
// rewrite a multi-megabyte executable twice for no reason.
//
// MUST run before the archive is appended: that same rewrite silently discards
// any overlay already present.
[[nodiscard]] std::expected<void, Error> writeResources(
    const std::filesystem::path& exePath, std::string_view metaJson,
    const std::filesystem::path& iconSource);

// --- icons.cpp --------------------------------------------------------------

// Copies every RT_GROUP_ICON and RT_ICON from `source` into an open resource
// update handle, preserving ids and languages.
//
// Ids are preserved rather than renumbered because Explorer picks an
// executable's displayed icon by taking the lowest-numbered icon group;
// renumbering could silently change which icon a packaged app shows.
//
// Returns the number of icon groups copied. A source with no icons is not an
// error -- plenty of valid entry points (a .bat, a console tool) have none.
[[nodiscard]] std::expected<int, Error> copyIconResources(
    void* updateHandle, const std::filesystem::path& source);

// Copies the RT_VERSION resource from `source`, so a packaged executable reports
// the application's own name and version in its Properties dialog rather than
// the stub's placeholder. Copying the icon without this produces a package that
// looks right in Explorer and wrong the moment anyone opens Properties.
[[nodiscard]] std::expected<int, Error> copyVersionResource(
    void* updateHandle, const std::filesystem::path& source);

struct VersionStrings {
    std::wstring productName;
    std::wstring fileDescription;
    std::wstring fileVersion;
    std::wstring companyName;
};

// Reads the VERSIONINFO strings from an executable. Every field may be empty:
// most executables carry some of these and few carry all.
[[nodiscard]] VersionStrings readVersionStrings(const std::filesystem::path& exe);

// --- console.cpp ------------------------------------------------------------

// True when Windows created this process's console rather than it being
// inherited from a shell.
//
// The distinction decides whether a console window is ours to close: one created
// for us has exactly this process attached, while one inherited from cmd.exe has
// the shell attached too. Hiding the latter would hide the user's own terminal.
[[nodiscard]] bool ownsConsole();

// Hides the console window, but only if this process owns it. Returns whether it
// actually hid anything.
bool hideOwnConsole();

// True once hideOwnConsole has hidden a window. Errors then have to reach the
// user some other way, since nothing written to stdout or stderr is visible.
[[nodiscard]] bool consoleHidden();

// --- cmdline.cpp ------------------------------------------------------------

// Recognises `hnxpack --src DIR`: the interactive flow with its first question
// already answered.
//
// Returns the directory only when --src and its value are the whole command
// line. --no-console is tolerated, since this form implies it anyway.
//
// Deliberately strict, and the strictness is the point. Anything else present
// means a scripted invocation, and dropping one of those into a modal dialog
// would hang a build server with nothing written anywhere to say why.
[[nodiscard]] std::optional<std::filesystem::path> presetSourceFromArgs(
    int argc, const wchar_t* const* argv);

// --- interactive.cpp --------------------------------------------------------

// The interactive flow: pick a source folder, pick the entry point inside it,
// then pick where to save. Returns a process exit code.
//
// `presetSource` answers the first question up front, which is what
// `hnxpack --src DIR` passes; the flow then starts at the entry point. Sharing
// one function rather than growing a second beside it is what keeps the
// containment rule, the derived metadata and the save dialog identical across
// both entry points.
[[nodiscard]] int runInteractive(
    const std::optional<std::filesystem::path>& presetSource = std::nullopt);

// True when this process can actually show a dialog. A packer invoked from CI
// with no arguments must print usage rather than block forever on a window
// nobody can see.
[[nodiscard]] bool canShowDialogs();

// Derives an appId from a folder name.
//
// Exposed for testing because the result has to satisfy a contract that is easy
// to break: appId becomes a directory name under %LOCALAPPDATA% and part of the
// lock identity, so it must pass validateArchivePath as a single safe component.
// Folder names routinely contain spaces, dots, and reserved words that do not.
[[nodiscard]] std::string sanitizeAppId(std::string_view folderName);

}  // namespace hnx::pack
