#include "dialogs.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace hnx::pack {
namespace {

namespace fs = std::filesystem;

constexpr wchar_t kRegKey[] = LR"(Software\HNx\hnxpack)";

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() {
        if (p_) p_->Release();
    }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T** put() { return &p_; }
    T* operator->() const { return p_; }
    [[nodiscard]] T* get() const { return p_; }

private:
    T* p_ = nullptr;
};

// SetFolder forces the location; SetDefaultFolder only applies when the dialog
// has no remembered state of its own. Picking an entry point must force (it has
// to come from the chosen folder); a save location only suggests.
bool setStartFolder(IFileDialog* dlg, const std::wstring& path, bool force) {
    if (path.empty()) return false;

    ComPtr<IShellItem> item;
    if (FAILED(::SHCreateItemFromParsingName(path.c_str(), nullptr,
                                             IID_PPV_ARGS(item.put()))))
        return false;

    return SUCCEEDED(force ? dlg->SetFolder(item.get())
                           : dlg->SetDefaultFolder(item.get()));
}

std::wstring itemPath(IShellItem* item) {
    PWSTR raw = nullptr;
    if (!item || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) return {};
    std::wstring out(raw);
    ::CoTaskMemFree(raw);
    return out;
}

std::wstring resultPath(IFileDialog* dlg) {
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(item.put()))) return {};
    return itemPath(item.get());
}

}  // namespace

ComScope::ComScope() {
    const HRESULT hr =
        ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    // RPC_E_CHANGED_MODE means COM is already up on this thread in another
    // apartment model. That is fine for the dialogs, but this scope must not
    // uninitialise something it did not initialise.
    ok_ = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    owned_ = SUCCEEDED(hr);
}

ComScope::~ComScope() {
    if (owned_) ::CoUninitialize();
}

std::optional<fs::path> pickFolder(void* owner, const wchar_t* title,
                                   const std::wstring& startDir, bool forceStartDir) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(dlg.put()))))
        return std::nullopt;

    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(title);
    setStartFolder(dlg.get(), startDir, forceStartDir);

    if (FAILED(dlg->Show(static_cast<HWND>(owner)))) return std::nullopt;

    std::wstring p = resultPath(dlg.get());
    if (p.empty()) return std::nullopt;
    return fs::path(p);
}

std::optional<fs::path> pickProgramFile(void* owner, const wchar_t* title,
                                        const std::wstring& startDir,
                                        bool forceStartDir) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(dlg.put()))))
        return std::nullopt;

    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    dlg->SetTitle(title);

    // Executables first, but not exclusively: an entry point can legitimately be
    // a .bat or a .lnk, and the launcher handles both.
    const COMDLG_FILTERSPEC filters[] = {
        {L"Programs and scripts", L"*.exe;*.bat;*.cmd;*.lnk;*.com"},
        {L"Executables", L"*.exe"},
        {L"All files", L"*.*"},
    };
    dlg->SetFileTypes(ARRAYSIZE(filters), filters);
    dlg->SetFileTypeIndex(1);
    setStartFolder(dlg.get(), startDir, forceStartDir);

    if (FAILED(dlg->Show(static_cast<HWND>(owner)))) return std::nullopt;

    std::wstring p = resultPath(dlg.get());
    if (p.empty()) return std::nullopt;
    return fs::path(p);
}

std::vector<fs::path> pickAnyFiles(void* owner, const wchar_t* title,
                                   const std::wstring& startDir) {
    std::vector<fs::path> out;

    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(dlg.put()))))
        return out;

    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT);
    dlg->SetTitle(title);

    const COMDLG_FILTERSPEC filters[] = {{L"All files", L"*.*"}};
    dlg->SetFileTypes(ARRAYSIZE(filters), filters);
    setStartFolder(dlg.get(), startDir, false);

    if (FAILED(dlg->Show(static_cast<HWND>(owner)))) return out;

    ComPtr<IShellItemArray> items;
    if (FAILED(dlg->GetResults(items.put()))) return out;

    DWORD count = 0;
    if (FAILED(items->GetCount(&count))) return out;

    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        if (FAILED(items->GetItemAt(i, item.put()))) continue;
        std::wstring p = itemPath(item.get());
        if (!p.empty()) out.emplace_back(p);
    }
    return out;
}

std::optional<fs::path> pickSaveFile(void* owner, const wchar_t* title,
                                     const std::wstring& startDir,
                                     const std::wstring& defaultName) {
    ComPtr<IFileSaveDialog> dlg;
    if (FAILED(::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(dlg.put()))))
        return std::nullopt;

    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
    dlg->SetTitle(title);

    const COMDLG_FILTERSPEC filters[] = {{L"Application", L"*.exe"}};
    dlg->SetFileTypes(ARRAYSIZE(filters), filters);
    dlg->SetDefaultExtension(L"exe");
    dlg->SetFileName(defaultName.c_str());

    if (!startDir.empty()) setStartFolder(dlg.get(), startDir, true);

    if (FAILED(dlg->Show(static_cast<HWND>(owner)))) return std::nullopt;

    std::wstring p = resultPath(dlg.get());
    if (p.empty()) return std::nullopt;
    return fs::path(p);
}

std::wstring loadSetting(const wchar_t* name) {
    HKEY key{};
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS)
        return {};

    DWORD type = 0;
    DWORD bytes = 0;
    std::wstring out;
    if (::RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
        type == REG_SZ && bytes >= sizeof(wchar_t)) {
        out.resize(bytes / sizeof(wchar_t));
        if (::RegQueryValueExW(key, name, nullptr, nullptr,
                               reinterpret_cast<BYTE*>(out.data()), &bytes) != ERROR_SUCCESS)
            out.clear();
        while (!out.empty() && out.back() == L'\0') out.pop_back();
    }
    ::RegCloseKey(key);
    return out;
}

void saveSetting(const wchar_t* name, const std::wstring& value) {
    HKEY key{};
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                          &key, nullptr) != ERROR_SUCCESS)
        return;
    ::RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                     static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(key);
}

}  // namespace hnx::pack
