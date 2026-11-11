// writeResources replacing the stub's version info with the application's.
//
// Copying only replaces a resource with the same name and language, and the
// stub's placeholder is US English. An application whose version info is in any
// other language used to end up with both, and Explorer on an English system then
// showed the stub's name and version for the package.
#include "test.hpp"

#include <filesystem>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "packer.hpp"

namespace fs = std::filesystem;
using namespace hnx::pack;

namespace {

void addVersion(const fs::path& exe, WORD lang) {
    HANDLE h = ::BeginUpdateResourceW(exe.wstring().c_str(), FALSE);
    if (!h) return;
    // The content is never parsed on this path, only copied.
    char blob[64] = "version-blob";
    ::UpdateResourceW(h, RT_VERSION, MAKEINTRESOURCEW(1), lang, blob, sizeof(blob));
    ::EndUpdateResourceW(h, FALSE);
}

BOOL CALLBACK onLang(HMODULE, LPCWSTR, LPCWSTR, WORD lang, LONG_PTR param) {
    reinterpret_cast<std::vector<WORD>*>(param)->push_back(lang);
    return TRUE;
}

std::vector<WORD> versionLanguages(const fs::path& exe) {
    std::vector<WORD> out;
    HMODULE m = ::LoadLibraryExW(exe.wstring().c_str(), nullptr,
                                 LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) return out;
    ::EnumResourceLanguagesW(m, RT_VERSION, MAKEINTRESOURCEW(1), onLang,
                             reinterpret_cast<LONG_PTR>(&out));
    ::FreeLibrary(m);
    return out;
}

}  // namespace

HNX_TEST(resources_replace_stub_version_info_in_another_language) {
    wchar_t self[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);

    const fs::path dir = fs::temp_directory_path() /
                         ("hnx_rsrc_" + std::to_string(::GetCurrentProcessId()));
    fs::create_directories(dir);
    const fs::path stub = dir / "stub.exe";
    const fs::path app = dir / "app.exe";
    fs::copy_file(self, stub, fs::copy_options::overwrite_existing);
    fs::copy_file(self, app, fs::copy_options::overwrite_existing);

    addVersion(stub, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
    addVersion(app, MAKELANGID(LANG_GERMAN, SUBLANG_GERMAN));

    auto r = writeResources(stub, "{}", app);
    HNX_CHECK(r.has_value());

    const auto langs = versionLanguages(stub);
    HNX_CHECK_EQ(langs.size(), std::size_t{1});
    if (!langs.empty()) HNX_CHECK_EQ(langs.front(), MAKELANGID(LANG_GERMAN, SUBLANG_GERMAN));

    std::error_code ec;
    fs::remove_all(dir, ec);
}
