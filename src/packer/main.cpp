// hnxpack: builds, inspects and verifies HNx packaged executables.
//
//   hnxpack                  interactive: pick a folder, an entry point, a destination
//   hnxpack --src DIR        the same, starting at the entry point
//   hnxpack build   --src DIR --out EXE [--stub EXE] [--config JSON] ...
//   hnxpack inspect EXE
//   hnxpack verify  EXE
//
// inspect and verify are not conveniences. A packaged executable is opaque, and
// without them the only way to diagnose a bad payload is to run it and watch it
// fail.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <vector>

#include "codec/codec.hpp"
#include "hash.hpp"
#include "meta.hpp"
#include "packer.hpp"
#include "paths.hpp"
#include "selfmap.hpp"
#include "toc.hpp"

namespace fs = std::filesystem;
using namespace hnx;
using namespace hnx::pack;

namespace {

// Maps a whole file into memory for reading. Used by inspect and verify, which
// need the same view of a packaged exe that the stub gets.
class MappedFile {
public:
    ~MappedFile() {
        if (base_) ::UnmapViewOfFile(base_);
        if (map_) ::CloseHandle(map_);
        if (file_ != INVALID_HANDLE_VALUE) ::CloseHandle(file_);
    }

    [[nodiscard]] bool open(const fs::path& p) {
        file_ = ::CreateFileW(p.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return false;

        LARGE_INTEGER li{};
        if (!::GetFileSizeEx(file_, &li) || li.QuadPart <= 0) return false;
        size_ = static_cast<std::size_t>(li.QuadPart);

        map_ = ::CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!map_) return false;

        base_ = static_cast<const std::byte*>(::MapViewOfFile(map_, FILE_MAP_READ, 0, 0, 0));
        return base_ != nullptr;
    }

    [[nodiscard]] std::span<const std::byte> bytes() const { return {base_, size_}; }

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE map_ = nullptr;
    const std::byte* base_ = nullptr;
    std::size_t size_ = 0;
};

int fail(const Error& e) {
    std::println(stderr, "hnxpack: {}", e.message());
    return 1;
}

// Shared with the stub on purpose. An earlier version reimplemented the lookup
// here without the alignment sweep and the recovery scan, so a signed package
// reported "no payload" from hnxpack while the stub ran it perfectly -- meaning
// verify was not checking what the stub would actually load.
[[nodiscard]] std::expected<Footer, Error> footerOf(std::span<const std::byte> image) {
    return findFooter(image);
}

int cmdInspect(const fs::path& exe) {
    MappedFile mf;
    if (!mf.open(exe)) return fail(Error{Ec::SelfOpenFailed, exe.string()});

    auto f = footerOf(mf.bytes());
    if (!f) return fail(f.error());

    const auto archive = mf.bytes().subspan(static_cast<std::size_t>(f->archiveOffset),
                                            static_cast<std::size_t>(f->archiveSize));
    auto toc = Toc::parse(archive, *f);
    if (!toc) return fail(toc.error());

    std::println("file            : {}", exe.string());
    std::println("format version  : {}", f->formatVersion);
    std::println("buildId         : {:016x}", f->buildId);
    std::println("stub size       : {}", humanBytes(f->archiveOffset));
    std::println("archive size    : {}", humanBytes(f->archiveSize));
    std::println("toc             : {} compressed, {} raw, codec {}",
                 humanBytes(f->tocCompSize), humanBytes(f->tocRawSize),
                 codec::name(f->tocCodec));
    std::println("entries         : {} files, {} directories", toc->fileCount(),
                 toc->dirCount());
    std::println("blocks          : {}", toc->blocks().size());
    std::println("raw content     : {}", humanBytes(toc->totalRawBytes()));

    std::size_t byCodec[8] = {};
    for (const auto& b : toc->blocks())
        if (b.codec < 8) ++byCodec[b.codec];
    std::print("block codecs    :");
    for (u8 c = 0; c < 8; ++c)
        if (byCodec[c]) std::print(" {}={}", codec::name(c), byCodec[c]);
    std::println("");

    return 0;
}

int cmdVerify(const fs::path& exe) {
    MappedFile mf;
    if (!mf.open(exe)) return fail(Error{Ec::SelfOpenFailed, exe.string()});

    auto f = footerOf(mf.bytes());
    if (!f) return fail(f.error());

    const auto archive = mf.bytes().subspan(static_cast<std::size_t>(f->archiveOffset),
                                            static_cast<std::size_t>(f->archiveSize));
    auto toc = Toc::parse(archive, *f);
    if (!toc) return fail(toc.error());

    // Decompress every block and check each file's content hash against the TOC.
    // This is the check that would catch a corrupt build before it ships.
    std::vector<std::vector<std::byte>> decoded(toc->blocks().size());

    for (std::size_t bi = 0; bi < toc->blocks().size(); ++bi) {
        const BlockDesc& b = toc->blocks()[bi];
        const auto src = archive.subspan(static_cast<std::size_t>(b.compOffset), b.compSize);
        decoded[bi].resize(b.rawSize);
        auto n = codec::decompress(b.codec, src, decoded[bi]);
        if (!n) return fail(n.error());
        if (*n != b.rawSize)
            return fail(Error{Ec::SizeMismatch, "block " + std::to_string(bi)});
    }

    std::size_t checked = 0, bad = 0;
    for (std::size_t i = 0; i < toc->entries().size(); ++i) {
        const EntryDesc& e = toc->entries()[i];
        if (static_cast<EntryType>(e.type) != EntryType::File) continue;

        Hasher hasher;
        if (e.flags & ENTRY_FLAG_SPANS_BLOCKS) {
            for (u32 k = e.blockIndex; k < e.blockIndex + e.blockSpan; ++k)
                hasher.update(decoded[k]);
        } else {
            hasher.update(std::span{decoded[e.blockIndex].data() + e.offsetInBlock,
                                    static_cast<std::size_t>(e.size)});
        }

        ++checked;
        if (hasher.digest() != e.hash) {
            ++bad;
            std::println(stderr, "  hash mismatch: {}", toc->path(e));
        }
    }

    if (bad > 0) {
        std::println(stderr, "verify FAILED: {} of {} files corrupt", bad, checked);
        return 1;
    }
    std::println("verify OK: {} files, {} blocks, all hashes match", checked,
                 toc->blocks().size());
    return 0;
}

void usage() {
    std::println(
        "hnxpack - build HNx portable applications\n"
        "\n"
        "  hnxpack               with no arguments, asks for a folder, an entry\n"
        "                        point and a destination\n"
        "\n"
        "  hnxpack --src DIR     the same three questions with the first one\n"
        "                        already answered: asks only for the entry point\n"
        "                        and the destination\n"
        "\n"
        "  hnxpack build --src DIR --out EXE [options]\n"
        "      --arch NAME       x64 | x86  (default: matched to the entry\n"
        "                        point, x64 if it is not a PE image)\n"
        "      --stub EXE        use this stub file instead of an embedded one\n"
        "      --config JSON     metadata file (default: generated)\n"
        "      --codec NAME      store | lz4 | zstd     (default: zstd)\n"
        "      --level N         codec level            (default: 19)\n"
        "      --block-size MB   solid block target     (default: 8)\n"
        "      --threads N       (default: hardware concurrency)\n"
        "      --icon-from FILE  take icons and version info from FILE\n"
        "                        (default: the entry point)\n"
        "      --exclude PATH    leave a file or folder out (repeatable).\n"
        "                        Excluding a folder excludes everything in it.\n"
        "      --no-icon         copy neither icons nor version info\n"
        "      --allow-unsafe-paths   TEST ONLY: skip path validation\n"
        "\n"
        "  --no-console            hide the console window, if this process owns\n"
        "                          it. A terminal you launched hnxpack from is\n"
        "                          never hidden. Implied when run with no\n"
        "                          arguments. Errors then appear as dialogs.\n"
        "\n"
        "  hnxpack inspect EXE   show payload details\n"
        "  hnxpack verify  EXE   decompress everything and check hashes");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // Console handling comes first, before any output, so the window is hidden as
    // early as possible. Windows creates it before main runs, so a brief flash is
    // unavoidable in a console-subsystem program -- this keeps it to a few
    // milliseconds rather than leaving the window up for the whole run.
    bool noConsole = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view a{argv[i]};
        if (a == L"--no-console" || a == L"--noconsole") {
            noConsole = true;
            break;
        }
    }

    // No arguments means the interactive flow: launched from Explorer or a
    // shortcut, where the console is ours and carries nothing the user wants.
    // Hiding it automatically matters because a double-click cannot pass a flag.
    const bool interactiveFlow = (argc < 2);

    // `hnxpack --src DIR` is the same flow with its first question answered.
    // Recognised here rather than under `build`, because `build` is the scripted
    // path where blocking on a dialog would be exactly the wrong thing to do.
    const std::optional<fs::path> presetSource = presetSourceFromArgs(argc, argv);

    // The shorthand gets the same console treatment as the no-argument flow, and
    // for the same reason: it is what a shell verb or a "send to" shortcut would
    // pass, neither of which can add a flag. hideOwnConsole only ever hides a
    // console this process owns, so a terminal it was typed into is left alone.
    if (noConsole || interactiveFlow || presetSource) hideOwnConsole();

    if (interactiveFlow || presetSource) {
        // Guarded on there actually being a desktop: a packer run from CI with no
        // arguments must print usage rather than block forever on a dialog nobody
        // can see.
        if (canShowDialogs()) return runInteractive(presetSource);
        usage();
        return 2;
    }

    const std::wstring cmd = argv[1];

    if (cmd == L"--help" || cmd == L"-h" || cmd == L"/?") {
        usage();
        return 0;
    }

    auto next = [&](int& i) -> std::wstring {
        if (i + 1 >= argc) return {};
        return argv[++i];
    };

    if (cmd == L"build") {
        BuildOptions opt;
        for (int i = 2; i < argc; ++i) {
            const std::wstring a = argv[i];
            if (a == L"--src") {
                // Repeatable: several unrelated folders or files can go into one
                // package. The first folder lands at the root, later ones under
                // their own name, matching what the GUI suggests.
                const std::filesystem::path p = next(i);
                opt.sources.push_back({p, suggestArchivePrefix(p, opt.sources)});
            }
            else if (a == L"--out") opt.output = next(i);
            else if (a == L"--stub") opt.stub = next(i);
            else if (a == L"--arch") {
                const std::wstring v = next(i);
                // Setting this suppresses matching against the entry point.
                opt.archExplicit = true;
                if (v == L"x64" || v == L"amd64" || v == L"x86_64") {
                    opt.arch = StubArch::X64;
                } else if (v == L"x86" || v == L"win32" || v == L"i386") {
                    opt.arch = StubArch::X86;
                } else {
                    std::println(stderr, "unknown architecture: {} (expected x64 or x86)",
                                 paths::narrow(v));
                    return 2;
                }
            }
            else if (a == L"--config") opt.configFile = next(i);
            else if (a == L"--exclude") {
                // Repeatable. Excluding a folder excludes everything under it,
                // so the GUI tree does not have to enumerate descendants -- and
                // neither does a script.
                opt.excluded.push_back(paths::narrow(next(i)));
            }
            else if (a == L"--icon-from") opt.iconSource = next(i);
            else if (a == L"--no-icon") opt.noIcon = true;
            else if (a == L"--threads") opt.threads = std::stoul(next(i));
            else if (a == L"--level") opt.level = std::stoi(next(i));
            else if (a == L"--block-size") {
                const auto mb = std::stoul(next(i));
                opt.blockSize = static_cast<u32>(
                    std::clamp<unsigned long>(mb, 1, MAX_BLOCK_RAW / (1024 * 1024)) *
                    1024 * 1024);
            } else if (a == L"--allow-unsafe-paths") {
                opt.allowUnsafePaths = true;
            } else if (a == L"--no-console" || a == L"--noconsole") {
                // Already acted on before argument parsing; accepted here so it
                // is not reported as unknown.
            } else if (a == L"--codec") {
                const std::wstring c = next(i);
                if (c == L"store") opt.codec = Codec::Store;
                else if (c == L"lz4") opt.codec = Codec::Lz4;
                else if (c == L"zstd") opt.codec = Codec::Zstd;
                else {
                    std::println(stderr, "unknown codec");
                    return 2;
                }
            } else if (a == L"--verbose") {
                opt.verbose = true;
            } else {
                std::println(stderr, "unknown option: {}", paths::narrow(a));
                return 2;
            }
        }
        // With the console hidden there is nowhere for a message to land, so a
        // failure has to reach the user as a dialog or not at all.
        if (consoleHidden()) opt.reportInDialogs = true;

        return buildPackage(std::move(opt));
    }

    if (cmd == L"inspect") {
        if (argc < 3) { usage(); return 2; }
        return cmdInspect(fs::path(argv[2]));
    }

    if (cmd == L"verify") {
        if (argc < 3) { usage(); return 2; }
        return cmdVerify(fs::path(argv[2]));
    }

    usage();
    return 2;
}
