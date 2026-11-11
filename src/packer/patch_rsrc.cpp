#include "packer.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "paths.hpp"

#include <string>
#include <vector>

namespace hnx::pack {
namespace {

// One RT_VERSION resource, by name and language.
struct VersionKey {
    bool isInt = true;
    WORD id = 0;
    std::wstring name;
    WORD language = 0;

    [[nodiscard]] const wchar_t* ptr() const {
        return isInt ? MAKEINTRESOURCEW(id) : name.c_str();
    }
};

BOOL CALLBACK onVersionLanguage(HMODULE, LPCWSTR, LPCWSTR name, WORD lang, LONG_PTR param) {
    VersionKey k;
    k.isInt = IS_INTRESOURCE(name);
    if (k.isInt) k.id = static_cast<WORD>(reinterpret_cast<ULONG_PTR>(name));
    else k.name = name;
    k.language = lang;
    reinterpret_cast<std::vector<VersionKey>*>(param)->push_back(std::move(k));
    return TRUE;
}

BOOL CALLBACK onVersionName(HMODULE mod, LPCWSTR type, LPWSTR name, LONG_PTR param) {
    ::EnumResourceLanguagesW(mod, type, name, onVersionLanguage, param);
    return TRUE;
}

// Every RT_VERSION resource in a file, read without running any of its code.
[[nodiscard]] std::vector<VersionKey> versionResources(const std::filesystem::path& p) {
    std::vector<VersionKey> out;
    HMODULE m = ::LoadLibraryExW(p.wstring().c_str(), nullptr,
                                 LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) return out;
    ::EnumResourceNamesW(m, RT_VERSION, onVersionName, reinterpret_cast<LONG_PTR>(&out));
    ::FreeLibrary(m);
    return out;
}

}  // namespace

std::expected<void, Error> writeResources(const std::filesystem::path& exePath,
                                          std::string_view metaJson,
                                          const std::filesystem::path& iconSource) {
    const std::wstring path = exePath.wstring();

    // Read before the update session opens. When the application brings its own
    // version info, the stub's placeholder is deleted first: copying replaces it
    // only when the name and language match, and the stub's is 1 / US English,
    // so an application in any other language ended up with both, and Explorer
    // on an English system showed HNx's name and version.
    const std::vector<VersionKey> stubVersions =
        (!iconSource.empty() && !versionResources(iconSource).empty())
            ? versionResources(exePath)
            : std::vector<VersionKey>{};

    // bDeleteExistingResources is false so the manifest and version info compiled
    // into the stub survive. Passing true here would strip the manifest, and the
    // resulting binary would lose longPathAware and its common-controls
    // dependency without any build-time complaint.
    HANDLE h = ::BeginUpdateResourceW(path.c_str(), FALSE);
    if (!h)
        return std::unexpected(
            Error{Ec::ResourcePatchFailed, "BeginUpdateResourceW", ::GetLastError()});

    // A single failure path: any error must discard the pending update, or the
    // next EndUpdateResource call from anywhere would commit a half-built set of
    // resources to the file.
    auto abort = [&](Error e) -> std::expected<void, Error> {
        ::EndUpdateResourceW(h, TRUE);
        return std::unexpected(std::move(e));
    };

    // MAKEINTRESOURCE is not used for the name: a string name keeps the blob
    // visible as "RCDATA / HNXMETA" in resource editors, which is the point of
    // choosing an inspectable format in the first place.
    if (!::UpdateResourceW(h, RT_RCDATA, L"HNXMETA",
                           MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                           const_cast<void*>(static_cast<const void*>(metaJson.data())),
                           static_cast<DWORD>(metaJson.size())))
        return abort(Error{Ec::ResourcePatchFailed, "UpdateResourceW", ::GetLastError()});

    // Branding rides along in the same session. Doing this as a second
    // Begin/End pass would rewrite the whole executable a second time.
    //
    // Icons and version info travel together deliberately: an executable showing
    // the application's icon but the stub's name and version in its Properties
    // dialog looks broken in a way that is hard to explain.
    if (!iconSource.empty()) {
        auto icons = copyIconResources(h, iconSource);
        if (!icons) return abort(icons.error());

        for (const auto& k : stubVersions)
            if (!::UpdateResourceW(h, RT_VERSION, k.ptr(), k.language, nullptr, 0))
                return abort(Error{Ec::ResourcePatchFailed, "UpdateResourceW(RT_VERSION delete)",
                                   ::GetLastError()});

        auto version = copyVersionResource(h, iconSource);
        if (!version) return abort(version.error());
    }

    // This is the call that rewrites the whole file. Anything appended before now
    // would be destroyed, which is exactly why the build pipeline patches
    // resources first and appends the container afterwards.
    if (!::EndUpdateResourceW(h, FALSE))
        return std::unexpected(
            Error{Ec::ResourcePatchFailed, "EndUpdateResourceW", ::GetLastError()});

    return {};
}

}  // namespace hnx::pack
