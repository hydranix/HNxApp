// Transplanting icon resources from the packaged application into the stub.
//
// Without this a packaged executable shows the stub's icon -- which is to say no
// icon at all -- so every app built with HNx would look identical in Explorer and
// on the taskbar. Copying the entry point's icons makes the package look like the
// application it contains.
//
// Icons in a PE are two resource types working together: RT_GROUP_ICON holds a
// directory listing the available sizes and colour depths, and each entry points
// at an RT_ICON holding one actual image. Copying the group without its members,
// or renumbering either, produces an executable that Explorer renders as a blank
// or generic icon.
#include "packer.hpp"

#include <cstring>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "paths.hpp"

namespace hnx::pack {
namespace {

namespace fs = std::filesystem;

// The on-disk form of an icon directory inside a PE. This differs from the .ico
// file layout: a file stores a 4-byte offset to the image, a resource stores a
// 2-byte id of the RT_ICON that holds it. Confusing the two yields a corrupt
// group that renders as a blank icon.
#pragma pack(push, 2)
struct GroupIconDirHeader {
    WORD reserved;
    WORD type;  // 1 = icon, 2 = cursor
    WORD count;
};
struct GroupIconDirEntry {
    BYTE width;
    BYTE height;
    BYTE colorCount;
    BYTE reserved;
    WORD planes;
    WORD bitCount;
    DWORD bytesInRes;
    WORD id;  // id of the RT_ICON resource, not a file offset
};
#pragma pack(pop)

static_assert(sizeof(GroupIconDirHeader) == 6);
static_assert(sizeof(GroupIconDirEntry) == 14);

// A resource name is either an integer id or a string. Carrying both through the
// copy keeps the destination identical to the source.
struct ResName {
    bool isInt = true;
    WORD id = 0;
    std::wstring name;

    [[nodiscard]] const wchar_t* ptr() const {
        return isInt ? MAKEINTRESOURCEW(id) : name.c_str();
    }

    static ResName from(const wchar_t* n) {
        ResName r;
        if (IS_INTRESOURCE(n)) {
            r.isInt = true;
            r.id = static_cast<WORD>(reinterpret_cast<ULONG_PTR>(n));
        } else {
            r.isInt = false;
            r.name = n;
        }
        return r;
    }
};

struct GroupRecord {
    ResName name;
    WORD language = 0;
    std::vector<std::byte> data;
};

struct EnumContext {
    HMODULE module = nullptr;
    std::vector<GroupRecord> groups;
};

BOOL CALLBACK onLanguage(HMODULE mod, LPCWSTR type, LPCWSTR name, WORD lang,
                         LONG_PTR param) {
    auto* ctx = reinterpret_cast<EnumContext*>(param);

    HRSRC res = ::FindResourceExW(mod, type, name, lang);
    if (!res) return TRUE;

    const DWORD size = ::SizeofResource(mod, res);
    HGLOBAL h = ::LoadResource(mod, res);
    if (!h || size == 0) return TRUE;

    const void* p = ::LockResource(h);
    if (!p) return TRUE;

    GroupRecord rec;
    rec.name = ResName::from(name);
    rec.language = lang;
    rec.data.resize(size);
    std::memcpy(rec.data.data(), p, size);
    ctx->groups.push_back(std::move(rec));
    return TRUE;
}

BOOL CALLBACK onGroupName(HMODULE mod, LPCWSTR type, LPWSTR name, LONG_PTR param) {
    ::EnumResourceLanguagesW(mod, type, name, onLanguage, param);
    return TRUE;
}

// Loads a module for resource reading only. The AS_DATAFILE and
// AS_IMAGE_RESOURCE flags together mean no code from the target ever runs and no
// DllMain is called -- the packer must be able to read icons out of an arbitrary
// executable without executing any of it.
class ResourceModule {
public:
    explicit ResourceModule(const fs::path& p) {
        module_ = ::LoadLibraryExW(p.wstring().c_str(), nullptr,
                                   LOAD_LIBRARY_AS_DATAFILE |
                                       LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    }
    ~ResourceModule() {
        if (module_) ::FreeLibrary(module_);
    }
    ResourceModule(const ResourceModule&) = delete;
    ResourceModule& operator=(const ResourceModule&) = delete;

    [[nodiscard]] HMODULE get() const noexcept { return module_; }
    [[nodiscard]] bool valid() const noexcept { return module_ != nullptr; }

private:
    HMODULE module_ = nullptr;
};

}  // namespace

std::expected<int, Error> copyIconResources(void* updateHandle, const fs::path& source) {
    ResourceModule mod(source);
    if (!mod.valid()) {
        // Not fatal on its own: the entry point may be a .bat or a data file with
        // no PE resources at all. The caller decides whether that matters.
        return 0;
    }

    EnumContext ctx;
    ctx.module = mod.get();

    if (!::EnumResourceNamesW(mod.get(), RT_GROUP_ICON, onGroupName,
                              reinterpret_cast<LONG_PTR>(&ctx))) {
        const DWORD e = ::GetLastError();
        // ERROR_RESOURCE_TYPE_NOT_FOUND simply means no icons, which is normal.
        if (e != ERROR_RESOURCE_TYPE_NOT_FOUND && e != ERROR_RESOURCE_DATA_NOT_FOUND &&
            e != ERROR_RESOURCE_ENUM_USER_STOP)
            return std::unexpected(Error{Ec::ResourcePatchFailed, "EnumResourceNamesW", e});
        return 0;
    }

    if (ctx.groups.empty()) return 0;

    int copiedGroups = 0;

    for (const auto& group : ctx.groups) {
        if (group.data.size() < sizeof(GroupIconDirHeader)) continue;

        GroupIconDirHeader hdr{};
        std::memcpy(&hdr, group.data.data(), sizeof(hdr));

        // Validate the directory against the resource's real size before trusting
        // the count: this data comes from an arbitrary executable the user picked,
        // and a malformed group must not walk off the end of the buffer.
        const std::size_t need =
            sizeof(GroupIconDirHeader) +
            static_cast<std::size_t>(hdr.count) * sizeof(GroupIconDirEntry);
        if (hdr.count == 0 || need > group.data.size()) continue;

        // Copy each image the group refers to, keeping its id so the directory
        // entries stay valid.
        for (WORD i = 0; i < hdr.count; ++i) {
            GroupIconDirEntry entry{};
            std::memcpy(&entry,
                        group.data.data() + sizeof(GroupIconDirHeader) +
                            static_cast<std::size_t>(i) * sizeof(GroupIconDirEntry),
                        sizeof(entry));

            HRSRC res = ::FindResourceExW(mod.get(), RT_ICON,
                                          MAKEINTRESOURCEW(entry.id), group.language);
            if (!res) {
                // Some executables list an image the resource table does not
                // actually contain. Skipping is right: the remaining sizes still
                // render, whereas failing the build would reject a package over a
                // cosmetic defect in someone else's binary.
                continue;
            }

            const DWORD size = ::SizeofResource(mod.get(), res);
            HGLOBAL h = ::LoadResource(mod.get(), res);
            if (!h || size == 0) continue;
            const void* p = ::LockResource(h);
            if (!p) continue;

            if (!::UpdateResourceW(updateHandle, RT_ICON, MAKEINTRESOURCEW(entry.id),
                                   group.language, const_cast<void*>(p), size))
                return std::unexpected(Error{Ec::ResourcePatchFailed,
                                             "UpdateResourceW(RT_ICON)", ::GetLastError()});
        }

        if (!::UpdateResourceW(updateHandle, RT_GROUP_ICON, group.name.ptr(),
                               group.language,
                               const_cast<void*>(static_cast<const void*>(group.data.data())),
                               static_cast<DWORD>(group.data.size())))
            return std::unexpected(Error{Ec::ResourcePatchFailed,
                                         "UpdateResourceW(RT_GROUP_ICON)",
                                         ::GetLastError()});

        ++copiedGroups;
    }

    return copiedGroups;
}

std::expected<int, Error> copyVersionResource(void* updateHandle, const fs::path& source) {
    ResourceModule mod(source);
    if (!mod.valid()) return 0;

    EnumContext ctx;
    ctx.module = mod.get();

    // RT_VERSION is a flat resource, so the same enumerate-and-copy the icon
    // groups use works directly; there are no member resources to chase.
    if (!::EnumResourceNamesW(mod.get(), RT_VERSION, onGroupName,
                              reinterpret_cast<LONG_PTR>(&ctx))) {
        const DWORD e = ::GetLastError();
        if (e != ERROR_RESOURCE_TYPE_NOT_FOUND && e != ERROR_RESOURCE_DATA_NOT_FOUND &&
            e != ERROR_RESOURCE_ENUM_USER_STOP)
            return std::unexpected(
                Error{Ec::ResourcePatchFailed, "EnumResourceNamesW(RT_VERSION)", e});
        return 0;
    }

    int copied = 0;
    for (const auto& rec : ctx.groups) {
        if (rec.data.empty()) continue;
        if (!::UpdateResourceW(updateHandle, RT_VERSION, rec.name.ptr(), rec.language,
                               const_cast<void*>(static_cast<const void*>(rec.data.data())),
                               static_cast<DWORD>(rec.data.size())))
            return std::unexpected(Error{Ec::ResourcePatchFailed,
                                         "UpdateResourceW(RT_VERSION)", ::GetLastError()});
        ++copied;
    }
    return copied;
}

VersionStrings readVersionStrings(const fs::path& exe) {
    VersionStrings out;

    const std::wstring path = exe.wstring();

    DWORD handle = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) return out;

    std::vector<std::byte> buf(size);
    if (!::GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return out;

    // The string table is keyed by language and codepage, so the translation
    // table has to be read first; assuming 040904B0 works for most US-English
    // binaries and silently returns nothing for everyone else.
    struct LangCodepage {
        WORD language;
        WORD codePage;
    };
    LangCodepage* translations = nullptr;
    UINT translationBytes = 0;
    if (!::VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
                          reinterpret_cast<void**>(&translations), &translationBytes) ||
        translationBytes < sizeof(LangCodepage))
        return out;

    wchar_t prefix[64];
    ::wsprintfW(prefix, L"\\StringFileInfo\\%04x%04x\\", translations[0].language,
                translations[0].codePage);

    auto query = [&](const wchar_t* field) -> std::wstring {
        std::wstring key = prefix;
        key.append(field);
        wchar_t* value = nullptr;
        UINT valueLen = 0;
        if (::VerQueryValueW(buf.data(), key.c_str(), reinterpret_cast<void**>(&value),
                             &valueLen) &&
            value && valueLen > 0) {
            std::wstring s(value, valueLen);
            while (!s.empty() && s.back() == L'\0') s.pop_back();
            return s;
        }
        return {};
    };

    out.productName = query(L"ProductName");
    out.fileDescription = query(L"FileDescription");
    out.fileVersion = query(L"FileVersion");
    out.companyName = query(L"CompanyName");
    return out;
}

}  // namespace hnx::pack
