#include "packer.hpp"

#include <algorithm>
#include <string_view>

namespace hnx::pack {
namespace {

[[nodiscard]] std::string_view extensionOf(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    const std::string_view name =
        slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0) return {};
    return name.substr(dot + 1);
}

[[nodiscard]] std::string_view parentOf(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
}

}  // namespace

std::vector<PlannedBlock> planBlocks(std::vector<SourceEntry>& entries, u32 blockSize) {
    // Collect the file indices; directories carry no content and take no block.
    std::vector<std::size_t> files;
    files.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i)
        if (!entries[i].isDir) files.push_back(i);

    // Sort by (extension, parent, name).
    //
    // This is the single cheapest compression win available. A solid block of 200
    // DLLs shares an enormous amount of structure; the same 200 DLLs interleaved
    // with icons and JSON share almost none. Grouping by extension first, then by
    // directory, puts the most similar content adjacent inside each block.
    std::sort(files.begin(), files.end(), [&](std::size_t a, std::size_t b) {
        const auto& ea = entries[a];
        const auto& eb = entries[b];

        const auto xa = extensionOf(ea.relPath);
        const auto xb = extensionOf(eb.relPath);
        if (xa != xb) return xa < xb;

        const auto pa = parentOf(ea.relPath);
        const auto pb = parentOf(eb.relPath);
        if (pa != pb) return pa < pb;

        return ea.relPath < eb.relPath;
    });

    std::vector<PlannedBlock> blocks;
    PlannedBlock current;

    auto flush = [&] {
        if (!current.members.empty()) {
            blocks.push_back(std::move(current));
            current = PlannedBlock{};
        }
    };

    for (const std::size_t idx : files) {
        const SourceEntry& e = entries[idx];

        // A file too large for one block gets its own run of exclusive blocks.
        // Nothing else is placed in them, so the extractor can stream the file to
        // disk one block at a time and never needs a buffer larger than one block
        // regardless of how large the file is.
        if (e.size > blockSize) {
            flush();

            u64 remaining = e.size;
            bool first = true;
            while (remaining > 0) {
                const u64 take = std::min<u64>(remaining, blockSize);
                PlannedBlock b;
                b.exclusive = true;
                b.spanningEntry = idx;
                b.rawSize = take;
                if (first) {
                    b.members.push_back(idx);
                    first = false;
                }
                blocks.push_back(std::move(b));
                remaining -= take;
            }
            continue;
        }

        if (current.rawSize + e.size > blockSize) flush();

        current.members.push_back(idx);
        current.rawSize += e.size;
    }

    flush();

    // A zero-byte file still needs a home so it gets an entry with a valid block
    // index; it contributes no bytes, so it rides along in whatever block it
    // landed in.
    return blocks;
}

}  // namespace hnx::pack
