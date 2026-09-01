#include "winpe.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace hnx::winpe {
namespace {

// Reads an environment variable, returning empty when it is not set. The two
// this module wants are short, so a fixed buffer avoids the size-then-read
// round trip.
[[nodiscard]] std::wstring environmentValue(const wchar_t* name) {
    constexpr DWORD kCapacity = MAX_PATH;
    wchar_t buf[kCapacity];
    const DWORD n = ::GetEnvironmentVariableW(name, buf, kCapacity);
    if (n == 0 || n >= kCapacity) return {};
    return std::wstring(buf, n);
}

[[nodiscard]] std::wstring withoutTrailingSeparator(std::wstring_view p) {
    while (p.size() > 1 && (p.back() == L'\\' || p.back() == L'/')) p.remove_suffix(1);
    return std::wstring(p);
}

}  // namespace

bool active() {
    static const bool result = [] {
        HKEY key = nullptr;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                            LR"(SYSTEM\CurrentControlSet\Control\MiniNT)", 0, KEY_READ,
                            &key) != ERROR_SUCCESS)
            return false;
        ::RegCloseKey(key);
        return true;
    }();
    return result;
}

bool isAbsolute(std::wstring_view path) noexcept {
    // Covers UNC (\\server\share) and the extended forms (\\?\, \\.\) alike.
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') return true;

    if (path.size() < 3 || path[1] != L':') return false;
    if (path[2] != L'\\' && path[2] != L'/') return false;

    const wchar_t d = path[0];
    return (d >= L'A' && d <= L'Z') || (d >= L'a' && d <= L'z');
}

std::wstring chooseScratchRoot(std::wstring_view temp, std::wstring_view systemRoot) {
    if (isAbsolute(temp)) return withoutTrailingSeparator(temp);

    if (isAbsolute(systemRoot)) {
        std::wstring out = withoutTrailingSeparator(systemRoot);
        out.append(L"\\Temp");
        return out;
    }

    // Both unset or unusable. WinPE always boots its RAM disk as X:, so this is
    // a real location rather than a guess -- but it is the last resort, because
    // an image whose scratch space was relocated will have said so in TEMP.
    return LR"(X:\Windows\Temp)";
}

std::wstring scratchRoot() {
    return chooseScratchRoot(environmentValue(L"TEMP"), environmentValue(L"SystemRoot"));
}

}  // namespace hnx::winpe
