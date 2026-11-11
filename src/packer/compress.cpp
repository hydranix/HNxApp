#include "packer.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <lz4.h>
#include <lz4hc.h>
#include <zstd.h>

#include "hash.hpp"
#include "paths.hpp"

namespace hnx::pack {
namespace {

[[nodiscard]] std::expected<std::vector<std::byte>, Error> readWholeFile(
    const std::wstring& extendedPath, u64 expectedSize) {
    std::vector<std::byte> out(static_cast<std::size_t>(expectedSize));
    if (expectedSize == 0) return out;

    HANDLE h = ::CreateFileW(extendedPath.c_str(), GENERIC_READ, SOURCE_SHARE_MODE,
                             nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return std::unexpected(
            Error{Ec::SourceWalkFailed, paths::narrow(extendedPath), ::GetLastError()});

    std::size_t done = 0;
    while (done < out.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(out.size() - done, 64u * 1024 * 1024));
        DWORD got = 0;
        if (!::ReadFile(h, out.data() + done, chunk, &got, nullptr)) {
            const DWORD e = ::GetLastError();
            ::CloseHandle(h);
            return std::unexpected(Error{Ec::SourceWalkFailed, paths::narrow(extendedPath), e});
        }
        if (got == 0) break;
        done += got;
    }
    ::CloseHandle(h);

    if (done != out.size())
        return std::unexpected(Error{Ec::SourceWalkFailed,
                                     "file changed size during packing: " +
                                         paths::narrow(extendedPath)});
    return out;
}

// Reads the slice of a spanning file that belongs to one of its exclusive blocks.
[[nodiscard]] std::expected<std::vector<std::byte>, Error> readFileRange(
    const std::wstring& extendedPath, u64 offset, u32 length) {
    std::vector<std::byte> out(length);
    if (length == 0) return out;

    HANDLE h = ::CreateFileW(extendedPath.c_str(), GENERIC_READ, SOURCE_SHARE_MODE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return std::unexpected(
            Error{Ec::SourceWalkFailed, paths::narrow(extendedPath), ::GetLastError()});

    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
        const DWORD e = ::GetLastError();
        ::CloseHandle(h);
        return std::unexpected(Error{Ec::SourceWalkFailed, paths::narrow(extendedPath), e});
    }

    std::size_t done = 0;
    while (done < out.size()) {
        DWORD got = 0;
        if (!::ReadFile(h, out.data() + done, static_cast<DWORD>(out.size() - done), &got,
                        nullptr)) {
            const DWORD e = ::GetLastError();
            ::CloseHandle(h);
            return std::unexpected(Error{Ec::SourceWalkFailed, paths::narrow(extendedPath), e});
        }
        if (got == 0) break;
        done += got;
    }
    ::CloseHandle(h);

    if (done != out.size())
        return std::unexpected(Error{Ec::SourceWalkFailed, "short read on spanning file"});
    return out;
}

}  // namespace

std::expected<std::vector<std::byte>, Error> compressBlock(std::span<const std::byte> raw,
                                                           Codec codec, int level,
                                                           Codec& usedCodec) {
    std::vector<std::byte> out;

    switch (codec) {
        case Codec::Store:
            usedCodec = Codec::Store;
            out.assign(raw.begin(), raw.end());
            return out;

        case Codec::Lz4: {
            const int bound = LZ4_compressBound(static_cast<int>(raw.size()));
            if (bound <= 0) return std::unexpected(Error{Ec::CompressFailed, "lz4 bound"});
            out.resize(static_cast<std::size_t>(bound));
            // The HC variant is slower to compress but decompresses at the same
            // speed, which is the right trade for a payload compressed once and
            // decompressed many times.
            const int n = LZ4_compress_HC(reinterpret_cast<const char*>(raw.data()),
                                          reinterpret_cast<char*>(out.data()),
                                          static_cast<int>(raw.size()), bound,
                                          std::clamp(level, 1, LZ4HC_CLEVEL_MAX));
            if (n <= 0) return std::unexpected(Error{Ec::CompressFailed, "lz4"});
            out.resize(static_cast<std::size_t>(n));
            usedCodec = Codec::Lz4;
            break;
        }

        case Codec::Zstd: {
            const std::size_t bound = ZSTD_compressBound(raw.size());
            out.resize(bound);
            const std::size_t n = ZSTD_compress(out.data(), out.size(), raw.data(),
                                                raw.size(), level);
            if (ZSTD_isError(n))
                return std::unexpected(Error{Ec::CompressFailed, ZSTD_getErrorName(n)});
            out.resize(n);
            usedCodec = Codec::Zstd;
            break;
        }

        default:
            return std::unexpected(Error{Ec::UnknownCodec, "codec not supported by packer"});
    }

    // Fall back to Store when compression did not pay for itself. Already
    // compressed content (PNG, ZIP, MP4, JAR) routinely comes out slightly larger
    // than it went in, and storing it saves both the size and the decompression
    // work at launch.
    if (out.size() >= raw.size()) {
        usedCodec = Codec::Store;
        out.assign(raw.begin(), raw.end());
    }

    return out;
}

std::expected<BuiltArchive, Error> buildArchive(const std::vector<SourceEntry>& entries,
                                                const std::vector<PlannedBlock>& blocks,
                                                const BuildOptions& opt, u64 buildId,
                                                const BuildCallbacks& cb) {
    // Compress every block in parallel, then lay them out sequentially. The
    // layout has to be deterministic, so compression results are collected into
    // an indexed vector rather than appended as they finish.
    std::vector<std::vector<std::byte>> compressed(blocks.size());
    std::vector<Codec> usedCodec(blocks.size(), Codec::Store);
    std::vector<u32> rawSize(blocks.size(), 0);

    // Where each block's data starts within its spanning file, needed because a
    // spanning run reads successive slices of one file.
    std::vector<u64> spanOffset(blocks.size(), 0);
    {
        u64 acc = 0;
        std::size_t currentEntry = SIZE_MAX;
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            if (!blocks[i].exclusive) continue;
            if (blocks[i].spanningEntry != currentEntry) {
                currentEntry = blocks[i].spanningEntry;
                acc = 0;
            }
            spanOffset[i] = acc;
            acc += blocks[i].rawSize;
        }
    }

    // EntryDesc::blockSpan is 16 bits, so a file may span at most 65535 blocks.
    // Past that the count wrapped and the TOC described the file as living in
    // one block, which the stub rejects at launch. Checked before compressing,
    // because the only files that can hit it are tens of gigabytes.
    {
        std::size_t run = 0;
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            const bool continues = i > 0 && blocks[i].exclusive && blocks[i - 1].exclusive &&
                                   blocks[i].spanningEntry == blocks[i - 1].spanningEntry;
            run = blocks[i].exclusive ? (continues ? run + 1 : 1) : 0;
            if (run > UINT16_MAX)
                return std::unexpected(Error{
                    Ec::CompressFailed,
                    entries[blocks[i].spanningEntry].relPath +
                        " needs more than 65535 blocks; use a larger --block-size"});
        }
    }

    std::atomic<std::size_t> next{0};
    std::atomic<u64> bytesDone{0};
    std::atomic<unsigned> running{0};
    std::atomic<bool> abort{false};
    std::mutex errMutex;
    std::optional<Error> firstError;

    u64 bytesTotal = 0;
    for (const auto& b : blocks) bytesTotal += b.rawSize;

    const unsigned threads =
        std::max(1u, opt.threads ? opt.threads : std::thread::hardware_concurrency());

    auto fail = [&](Error e) {
        std::lock_guard lock(errMutex);
        if (!firstError) firstError = std::move(e);
        abort.store(true);
    };

    auto worker = [&] {
        for (;;) {
            if (abort.load(std::memory_order_relaxed)) break;
            const std::size_t bi = next.fetch_add(1, std::memory_order_relaxed);
            if (bi >= blocks.size()) break;

            const PlannedBlock& pb = blocks[bi];
            std::vector<std::byte> raw;

            if (pb.exclusive) {
                // Not re-hashed here: the slices of one spanning file are spread
                // across workers, so there is no single pass over its content.
                // A verify of the finished package is what catches a large file
                // that changed while it was being packed.
                auto slice = readFileRange(entries[pb.spanningEntry].absPath,
                                           spanOffset[bi],
                                           static_cast<u32>(pb.rawSize));
                if (!slice) {
                    fail(slice.error());
                    break;
                }
                raw = std::move(*slice);
            } else {
                raw.reserve(static_cast<std::size_t>(pb.rawSize));
                bool ok = true;
                for (const std::size_t mi : pb.members) {
                    auto content = readWholeFile(entries[mi].absPath, entries[mi].size);
                    if (!content) {
                        fail(content.error());
                        ok = false;
                        break;
                    }
                    // Sources are read with every sharing mode granted, so a file
                    // can be written to between the walk that hashed it and this
                    // read. Packing it anyway would produce a TOC whose hash does
                    // not match the bytes stored, which verification then reports
                    // as corruption on every launch.
                    if (hashBytes(*content) != entries[mi].hash) {
                        fail(Error{Ec::SourceWalkFailed,
                                   "file changed while it was being packed: " +
                                       entries[mi].relPath});
                        ok = false;
                        break;
                    }
                    raw.insert(raw.end(), content->begin(), content->end());
                }
                if (!ok) break;
            }

            rawSize[bi] = static_cast<u32>(raw.size());

            Codec used = Codec::Store;
            auto packed = compressBlock(raw, opt.codec, opt.level, used);
            if (!packed) {
                fail(packed.error());
                break;
            }
            compressed[bi] = std::move(*packed);
            usedCodec[bi] = used;
            bytesDone.fetch_add(pb.rawSize, std::memory_order_relaxed);
        }
        running.fetch_sub(1);
    };

    {
        std::vector<std::jthread> pool;
        pool.reserve(threads);
        running.store(threads);
        for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker);

        // Compression is the long phase of a build -- minutes at high levels on a
        // large tree -- so it reports progress. Both callbacks are serviced here,
        // on the calling thread, the same way hashAll does it.
        if (cb.progress || cb.cancelled) {
            while (running.load() > 0) {
                if (cb.isCancelled()) abort.store(true);
                cb.step("Compressing", bytesDone.load(), bytesTotal);
                ::Sleep(30);
            }
            cb.step("Compressing", bytesDone.load(), bytesTotal);
        }
    }

    if (firstError) return std::unexpected(*firstError);
    if (abort.load()) return std::unexpected(Error{Ec::CompressFailed, "cancelled"});

    // --- lay out blocks -----------------------------------------------------
    BuiltArchive built;
    std::vector<BlockDesc> blockDescs(blocks.size());

    for (std::size_t i = 0; i < blocks.size(); ++i) {
        BlockDesc& d = blockDescs[i];
        d.compOffset = built.bytes.size();
        d.compSize = static_cast<u32>(compressed[i].size());
        d.rawSize = rawSize[i];
        d.codec = static_cast<u8>(usedCodec[i]);
        d.flags = blocks[i].exclusive ? BLOCK_FLAG_EXCLUSIVE : 0;
        d.dictId = 0;
        d.reserved = 0;
        built.bytes.insert(built.bytes.end(), compressed[i].begin(), compressed[i].end());
    }

    // --- entries and path pool ---------------------------------------------
    //
    // Entry order follows `entries` (directories first, then files by path), which
    // is what lets the stub create the tree in one forward pass.
    std::vector<EntryDesc> entryDescs(entries.size());
    std::string pathPool;

    // Map each file to the block that holds it.
    std::vector<u32> blockOfEntry(entries.size(), NO_BLOCK);
    std::vector<u64> offsetOfEntry(entries.size(), 0);
    std::vector<u16> spanOfEntry(entries.size(), 0);

    for (std::size_t bi = 0; bi < blocks.size(); ++bi) {
        const PlannedBlock& pb = blocks[bi];
        if (pb.exclusive) {
            if (!pb.members.empty()) {
                // First block of the run carries the member; count the run length.
                const std::size_t e = pb.members.front();
                blockOfEntry[e] = static_cast<u32>(bi);
                offsetOfEntry[e] = 0;
                u16 span = 0;
                for (std::size_t k = bi; k < blocks.size(); ++k) {
                    if (!blocks[k].exclusive || blocks[k].spanningEntry != pb.spanningEntry)
                        break;
                    ++span;
                }
                spanOfEntry[e] = span;
            }
            continue;
        }
        u64 off = 0;
        for (const std::size_t mi : pb.members) {
            blockOfEntry[mi] = static_cast<u32>(bi);
            offsetOfEntry[mi] = off;
            off += entries[mi].size;
        }
    }

    u64 totalRaw = 0;

    for (std::size_t i = 0; i < entries.size(); ++i) {
        const SourceEntry& s = entries[i];
        EntryDesc& d = entryDescs[i];

        d.pathOffset = static_cast<u32>(pathPool.size());
        pathPool.append(s.relPath);
        pathPool.push_back('\0');

        d.size = s.size;
        d.mtime = s.mtime;
        d.hash = s.hash;
        d.attrs = s.attrs;
        d.type = static_cast<u8>(s.isDir ? EntryType::Directory : EntryType::File);
        d.flags = s.isExec ? ENTRY_FLAG_EXECUTABLE : 0;
        d.blockSpan = 0;

        if (s.isDir) {
            d.blockIndex = NO_BLOCK;
            d.offsetInBlock = 0;
            continue;
        }

        totalRaw += s.size;
        d.blockIndex = blockOfEntry[i];
        d.offsetInBlock = offsetOfEntry[i];

        if (spanOfEntry[i] > 0) {
            d.flags |= ENTRY_FLAG_SPANS_BLOCKS;
            d.blockSpan = spanOfEntry[i];
            d.offsetInBlock = 0;
        }
    }

    // --- TOC ----------------------------------------------------------------
    TocHeader hdr{};
    hdr.entryCount = static_cast<u32>(entryDescs.size());
    hdr.blockCount = static_cast<u32>(blockDescs.size());
    hdr.pathPoolSize = static_cast<u32>(pathPool.size());
    hdr.reserved = 0;
    hdr.totalRawBytes = totalRaw;

    std::vector<std::byte> toc;
    auto appendBytes = [&toc](const void* p, std::size_t n) {
        const auto* b = static_cast<const std::byte*>(p);
        toc.insert(toc.end(), b, b + n);
    };

    appendBytes(&hdr, sizeof(hdr));
    if (!blockDescs.empty())
        appendBytes(blockDescs.data(), blockDescs.size() * sizeof(BlockDesc));
    if (!entryDescs.empty())
        appendBytes(entryDescs.data(), entryDescs.size() * sizeof(EntryDesc));
    if (!pathPool.empty()) appendBytes(pathPool.data(), pathPool.size());

    if (toc.size() > MAX_TOC_RAW) return std::unexpected(Error{Ec::TocTooLarge});

    // The TOC is always zstd: it is read on every single launch including the
    // warm path, so decompression speed and ratio both matter more here than the
    // flexibility of matching the payload codec.
    Codec tocCodec = Codec::Zstd;
    auto tocPacked = compressBlock(toc, Codec::Zstd, 19, tocCodec);
    if (!tocPacked) return std::unexpected(tocPacked.error());

    const u64 tocOffset = built.bytes.size();
    built.bytes.insert(built.bytes.end(), tocPacked->begin(), tocPacked->end());

    // --- footer -------------------------------------------------------------
    Footer& f = built.footer;
    std::memcpy(f.magic, FOOTER_MAGIC, sizeof(FOOTER_MAGIC));
    f.formatVersion = FORMAT_VERSION;
    f.flags = 0;
    for (const auto& e : entries)
        if (e.isDir) {
            f.flags |= FOOTER_FLAG_HAS_DIRS;
            break;
        }
    f.archiveOffset = 0;  // caller fills this in once it knows the stub size
    f.archiveSize = built.bytes.size();
    f.tocOffset = tocOffset;
    f.tocCompSize = static_cast<u32>(tocPacked->size());
    f.tocRawSize = static_cast<u32>(toc.size());
    f.tocCodec = static_cast<u8>(tocCodec);
    f.buildId = buildId;

    return built;
}

}  // namespace hnx::pack
