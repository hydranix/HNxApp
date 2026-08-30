// Common file dialogs.
//
// Extracted from the interactive flow because the GUI needs exactly the same
// pickers. Leaving them private to interactive.cpp would have meant two
// implementations of the same IFileDialog boilerplate drifting apart.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hnx::pack {

// Initialises COM for the calling thread and uninitialises on scope exit. The
// file dialogs are COM objects; a GUI that already runs its own COM apartment
// can skip this.
class ComScope {
public:
    ComScope();
    ~ComScope();
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    bool ok_ = false;
    bool owned_ = false;
};

// Each returns nothing when the user cancels, which is not an error.
//
// `owner` is the window the dialog is modal to; nullptr for no owner.

[[nodiscard]] std::optional<std::filesystem::path> pickFolder(
    void* owner, const wchar_t* title, const std::wstring& startDir,
    bool forceStartDir = false);

[[nodiscard]] std::optional<std::filesystem::path> pickProgramFile(
    void* owner, const wchar_t* title, const std::wstring& startDir,
    bool forceStartDir = false);

// Any file, for adding arbitrary content to a package.
[[nodiscard]] std::vector<std::filesystem::path> pickAnyFiles(
    void* owner, const wchar_t* title, const std::wstring& startDir);

[[nodiscard]] std::optional<std::filesystem::path> pickSaveFile(
    void* owner, const wchar_t* title, const std::wstring& startDir,
    const std::wstring& defaultName);

// Small persistent settings, so the GUI and the interactive flow remember the
// same last-used directories.
[[nodiscard]] std::wstring loadSetting(const wchar_t* name);
void saveSetting(const wchar_t* name, const std::wstring& value);

inline constexpr wchar_t SETTING_LAST_SAVE_DIR[] = L"LastSaveDir";
inline constexpr wchar_t SETTING_LAST_SOURCE_DIR[] = L"LastSourceDir";

}  // namespace hnx::pack
