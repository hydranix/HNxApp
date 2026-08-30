#include "codec.hpp"

#include <cstring>

#include <lz4.h>
#include <zstd.h>

namespace hnx::codec {

bool isSupported(u8 codecId) noexcept {
    switch (static_cast<Codec>(codecId)) {
        case Codec::Store:
        case Codec::Lz4:
        case Codec::Zstd:
            return true;
        default:
            return false;
    }
}

const char* name(u8 codecId) noexcept {
    switch (static_cast<Codec>(codecId)) {
        case Codec::Store: return "store";
        case Codec::Lz4: return "lz4";
        case Codec::Zstd: return "zstd";
        case Codec::Lzma: return "lzma";
        case Codec::Brotli: return "brotli";
    }
    return "unknown";
}

std::expected<std::size_t, Error> decompress(u8 codecId,
                                             std::span<const std::byte> src,
                                             std::span<std::byte> dst) {
    switch (static_cast<Codec>(codecId)) {
        case Codec::Store: {
            if (src.size() != dst.size())
                return std::unexpected(Error{Ec::SizeMismatch, "stored block"});
            std::memcpy(dst.data(), src.data(), src.size());
            return dst.size();
        }

        case Codec::Lz4: {
            // LZ4_decompress_safe is the only correct choice against untrusted
            // input: the _fast variant trusts the compressed stream to be well
            // formed and will happily run off the end of the buffer.
            const int n = LZ4_decompress_safe(
                reinterpret_cast<const char*>(src.data()),
                reinterpret_cast<char*>(dst.data()),
                static_cast<int>(src.size()),
                static_cast<int>(dst.size()));
            if (n < 0) return std::unexpected(Error{Ec::DecompressFailed, "lz4"});
            return static_cast<std::size_t>(n);
        }

        case Codec::Zstd: {
            const std::size_t n = ZSTD_decompress(dst.data(), dst.size(),
                                                  src.data(), src.size());
            if (ZSTD_isError(n))
                return std::unexpected(
                    Error{Ec::DecompressFailed, ZSTD_getErrorName(n)});
            return n;
        }

        default:
            return std::unexpected(
                Error{Ec::UnknownCodec, name(codecId)});
    }
}

}  // namespace hnx::codec
