#include "packer.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "paths.hpp"

namespace hnx::pack {

std::expected<void, Error> writeResources(const std::filesystem::path& exePath,
                                          std::string_view metaJson,
                                          const std::filesystem::path& iconSource) {
    const std::wstring path = exePath.wstring();

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
