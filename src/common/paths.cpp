#include "paths.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "format.hpp"

namespace hnx::paths {
namespace {

constexpr bool asciiEqualFold(char a, char b) noexcept {
    if (a >= 'a' && a <= 'z') a = static_cast<char>(a - 'a' + 'A');
    if (b >= 'a' && b <= 'z') b = static_cast<char>(b - 'a' + 'A');
    return a == b;
}

constexpr bool equalsFold(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!asciiEqualFold(a[i], b[i])) return false;
    return true;
}

// The base name is everything before the FIRST dot, not the last. Windows
// resolves "COM1.tar.gz" as the COM1 device, so checking only the final stem
// would let it through.
constexpr std::string_view deviceStem(std::string_view component) noexcept {
    const auto dot = component.find('.');
    return dot == std::string_view::npos ? component : component.substr(0, dot);
}

}  // namespace

bool isReservedDeviceName(std::string_view component) noexcept {
    const std::string_view stem = deviceStem(component);
    if (stem.size() != 3 && stem.size() != 4) return false;

    static constexpr std::string_view kThree[] = {"CON", "PRN", "AUX", "NUL"};
    for (auto n : kThree)
        if (equalsFold(stem, n)) return true;

    if (stem.size() == 4) {
        const bool com = asciiEqualFold(stem[0], 'C') && asciiEqualFold(stem[1], 'O') &&
                         asciiEqualFold(stem[2], 'M');
        const bool lpt = asciiEqualFold(stem[0], 'L') && asciiEqualFold(stem[1], 'P') &&
                         asciiEqualFold(stem[2], 'T');
        // COM0 and LPT0 are not devices; only 1 through 9 are.
        if ((com || lpt) && stem[3] >= '1' && stem[3] <= '9') return true;
    }
    return false;
}

bool isValidUtf8(std::string_view s) noexcept {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const auto* end = p + s.size();

    while (p < end) {
        const unsigned c = *p;

        if (c == 0x00) return false;  // embedded NUL

        if (c < 0x80) {
            ++p;
            continue;
        }

        int extra;
        unsigned cp;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07u;
        } else {
            return false;  // continuation byte or 5/6-byte form
        }

        if (p + extra >= end) return false;
        for (int i = 1; i <= extra; ++i) {
            const unsigned cc = p[i];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }

        // Overlong encodings map a short code point onto a longer sequence, which
        // is a classic way to sneak a "/" or "." past a byte-wise filter.
        if (extra == 1 && cp < 0x80) return false;
        if (extra == 2 && cp < 0x800) return false;
        if (extra == 3 && cp < 0x10000) return false;

        if (cp > 0x10FFFF) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;  // lone surrogate

        p += extra + 1;
    }
    return true;
}

std::expected<void, Error> validateArchivePath(std::string_view p) {
    const std::string ctx(p.substr(0, std::min<std::size_t>(p.size(), 256)));

    if (p.empty()) return std::unexpected(Error{Ec::PathEmptyComponent, ctx});
    if (p.size() > MAX_PATH_BYTES) return std::unexpected(Error{Ec::PathTooLong, ctx});
    if (!isValidUtf8(p)) return std::unexpected(Error{Ec::PathNotUtf8, ctx});

    // Absolute forms: leading separator, or a drive letter. Backslashes should
    // never appear at all (the pool is normalised to forward slashes), so treat
    // one as an attempt to bypass forward-slash-only component splitting.
    if (p.front() == '/' || p.front() == '\\')
        return std::unexpected(Error{Ec::PathAbsolute, ctx});
    if (p.find('\\') != std::string_view::npos)
        return std::unexpected(Error{Ec::PathAbsolute, ctx});
    if (p.size() >= 2 && p[1] == ':') return std::unexpected(Error{Ec::PathAbsolute, ctx});

    // A colon anywhere else opens an NTFS alternate data stream: "readme.txt:evil"
    // writes a hidden stream that the file listing never shows.
    if (p.find(':') != std::string_view::npos)
        return std::unexpected(Error{Ec::PathHasStream, ctx});

    if (p.back() == '/') return std::unexpected(Error{Ec::PathEmptyComponent, ctx});

    std::size_t start = 0;
    while (start <= p.size()) {
        std::size_t slash = p.find('/', start);
        if (slash == std::string_view::npos) slash = p.size();
        const std::string_view comp = p.substr(start, slash - start);

        if (comp.empty()) return std::unexpected(Error{Ec::PathEmptyComponent, ctx});

        // Compared as a whole component, so a file legitimately named "..foo"
        // still packs while a real traversal is caught.
        if (comp == "..") return std::unexpected(Error{Ec::PathTraversal, ctx});
        if (comp == ".") return std::unexpected(Error{Ec::PathEmptyComponent, ctx});

        // Windows silently strips trailing dots and spaces, so "evil. " and
        // "evil" would resolve to the same file: two archive entries could
        // collide, and the second would overwrite the first.
        if (comp.back() == '.' || comp.back() == ' ')
            return std::unexpected(Error{Ec::PathTrailingDotOrSpace, ctx});

        if (isReservedDeviceName(comp))
            return std::unexpected(Error{Ec::PathReservedName, ctx});

        // Control characters are not legal in NTFS names and are a display-spoof
        // vector in any UI that prints the path.
        for (char ch : comp)
            if (static_cast<unsigned char>(ch) < 0x20)
                return std::unexpected(Error{Ec::PathNotUtf8, ctx});

        if (slash == p.size()) break;
        start = slash + 1;
    }

    return {};
}

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                        static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                          static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view utf16) {
    if (utf16.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, utf16.data(),
                                        static_cast<int>(utf16.size()), nullptr, 0,
                                        nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()),
                          out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring toExtendedForm(std::wstring_view absolutePath) {
    if (absolutePath.starts_with(LR"(\\?\)")) return std::wstring(absolutePath);

    if (absolutePath.starts_with(LR"(\\)")) {
        std::wstring out = LR"(\\?\UNC\)";
        out.append(absolutePath.substr(2));
        return out;
    }

    std::wstring out = LR"(\\?\)";
    out.append(absolutePath);
    return out;
}

std::wstring toDisplayForm(std::wstring_view path) {
    if (path.starts_with(LR"(\\?\UNC\)")) {
        std::wstring out = LR"(\\)";
        out.append(path.substr(8));
        return out;
    }
    if (path.starts_with(LR"(\\?\)")) return std::wstring(path.substr(4));
    return std::wstring(path);
}

std::expected<std::wstring, Error> joinExtended(std::wstring_view base,
                                                std::string_view relative) {
    if (auto v = validateArchivePath(relative); !v) return std::unexpected(v.error());

    std::wstring wrel = widen(relative);
    if (wrel.empty() && !relative.empty())
        return std::unexpected(Error{Ec::PathNotUtf8, std::string(relative)});

    std::replace(wrel.begin(), wrel.end(), L'/', L'\\');

    std::wstring out = toExtendedForm(base);
    if (!out.empty() && out.back() != L'\\') out.push_back(L'\\');
    out.append(wrel);

    if (!isContainedIn(base, out))
        return std::unexpected(Error{Ec::PathEscapesTarget, std::string(relative)});

    return out;
}

bool isContainedIn(std::wstring_view base, std::wstring_view candidate) {
    // Both sides are reduced to display form before canonicalising, for two
    // independent reasons:
    //
    //  1. Callers pass a mix of forms -- joinExtended hands in a \\?\ candidate
    //     against a plain base. Comparing "\\?\C:\app" against "C:\app\x" as
    //     strings fails on the prefix alone and would report a legitimate child
    //     as an escape.
    //
    //  2. GetFullPathNameW deliberately does NOT resolve ".." inside a \\?\ path
    //     -- that is the whole point of the prefix. Canonicalising an extended
    //     path would therefore leave a traversal intact, and this function is the
    //     backstop that is supposed to catch exactly that.
    auto canonical = [](std::wstring_view p) -> std::wstring {
        std::wstring in = toDisplayForm(p);
        if (in.empty()) return {};

        DWORD n = ::GetFullPathNameW(in.c_str(), 0, nullptr, nullptr);
        if (n == 0) return {};
        std::wstring out(n, L'\0');
        DWORD w = ::GetFullPathNameW(in.c_str(), n, out.data(), nullptr);
        if (w == 0 || w >= n) return {};
        out.resize(w);
        return out;
    };

    std::wstring b = canonical(base);
    std::wstring c = canonical(candidate);
    if (b.empty() || c.empty()) return false;

    if (!b.empty() && b.back() == L'\\') b.pop_back();

    if (c.size() < b.size()) return false;

    // NTFS compares case-insensitively, so a check that did not fold case would
    // be trivially bypassed by changing the case of the target directory.
    if (::CompareStringOrdinal(c.data(), static_cast<int>(b.size()), b.data(),
                               static_cast<int>(b.size()), TRUE) != CSTR_EQUAL)
        return false;

    // Must be the same path or a true child, so that C:\app does not "contain"
    // C:\application.
    return c.size() == b.size() || c[b.size()] == L'\\';
}

std::wstring expandTokens(std::wstring_view input, const TokenValues& tv) {
    std::wstring work(input);

    auto replaceAll = [](std::wstring& s, std::wstring_view needle,
                         std::wstring_view value) {
        if (needle.empty()) return;
        std::size_t pos = 0;
        while ((pos = s.find(needle, pos)) != std::wstring::npos) {
            s.replace(pos, needle.size(), value);
            pos += value.size();
        }
    };

    // HNx tokens resolve before environment expansion so that a value pulled from
    // the environment can never itself be reinterpreted as a token.
    replaceAll(work, L"%TARGET%", tv.target);
    replaceAll(work, L"%APPDIR%", tv.appDir);
    replaceAll(work, L"%APPID%", tv.appId);
    replaceAll(work, L"%VERSION%", tv.version);

    DWORD n = ::ExpandEnvironmentStringsW(work.c_str(), nullptr, 0);
    if (n == 0) return work;
    std::wstring out(n, L'\0');
    DWORD w = ::ExpandEnvironmentStringsW(work.c_str(), out.data(), n);
    if (w == 0 || w > n) return work;
    out.resize(w > 0 ? w - 1 : 0);  // count includes the terminator
    return out;
}

}  // namespace hnx::paths
