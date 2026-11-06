// walkSources honouring the exclusion list during the walk, not after it.
//
// The order matters because of what an excluded entry is allowed to do. A file
// the user left out of the package must not be able to fail the build, so it has
// to be skipped before path validation runs -- and an excluded folder must not be
// entered at all. These tests use a name the validator rejects (a trailing dot,
// which Windows would silently strip) as the stand-in for anything unpackable a
// real tree can hold: a junction, a reserved device name, an unreadable file.
#include "test.hpp"

#include <algorithm>
#include <filesystem>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "packer.hpp"
#include "paths.hpp"

namespace fs = std::filesystem;
using namespace hnx;
using namespace hnx::pack;

namespace {

// A scratch tree that cleans up after itself. Removal goes through the \\?\ form
// because the unpackable names it creates cannot be deleted any other way.
class ScratchTree {
public:
    ScratchTree() {
        root_ = fs::temp_directory_path() /
                ("hnx_walk_" + std::to_string(::GetCurrentProcessId()) + "_" +
                 std::to_string(::GetTickCount64()));
        fs::create_directories(root_);
    }
    ~ScratchTree() { removeTree(paths::toExtendedForm(root_.wstring())); }

    ScratchTree(const ScratchTree&) = delete;
    ScratchTree& operator=(const ScratchTree&) = delete;

    [[nodiscard]] const fs::path& root() const { return root_; }

    // Creates a file at `rel` (backslash-separated), parents included, through
    // the extended form so a trailing dot survives.
    void file(const std::wstring& rel) const {
        const std::wstring full = paths::toExtendedForm((root_ / rel).wstring());
        const std::wstring parent = full.substr(0, full.find_last_of(L'\\'));
        ::CreateDirectoryW(parent.c_str(), nullptr);
        HANDLE h = ::CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD put = 0;
            ::WriteFile(h, "data", 4, &put, nullptr);
            ::CloseHandle(h);
        }
    }

private:
    fs::path root_;

    static void removeTree(const std::wstring& dir) {
        WIN32_FIND_DATAW fd{};
        HANDLE f = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring name = fd.cFileName;
                if (name == L"." || name == L"..") continue;
                const std::wstring child = dir + L"\\" + name;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeTree(child);
                else ::DeleteFileW(child.c_str());
            } while (::FindNextFileW(f, &fd));
            ::FindClose(f);
        }
        ::RemoveDirectoryW(dir.c_str());
    }
};

bool hasEntry(const std::vector<SourceEntry>& entries, std::string_view rel) {
    return std::any_of(entries.begin(), entries.end(),
                       [&](const SourceEntry& e) { return e.relPath == rel; });
}

}  // namespace

HNX_TEST(walk_rejects_an_unpackable_name_that_is_not_excluded) {
    ScratchTree t;
    t.file(L"good.txt");
    t.file(L"bad.");

    const SourceItem src{t.root(), {}};
    auto r = walkSources({&src, 1}, 1, false);
    HNX_CHECK(!r.has_value());
}

HNX_TEST(walk_skips_an_excluded_entry_before_validating_it) {
    ScratchTree t;
    t.file(L"good.txt");
    t.file(L"bad.");

    const SourceItem src{t.root(), {}};
    const std::vector<std::string> excluded{"bad."};
    auto r = walkSources({&src, 1}, 1, false, {}, excluded);
    HNX_CHECK(r.has_value());
    if (!r) return;
    HNX_CHECK(hasEntry(*r, "good.txt"));
    HNX_CHECK(!hasEntry(*r, "bad."));
}

HNX_TEST(walk_does_not_enter_an_excluded_directory) {
    ScratchTree t;
    t.file(L"keep\\a.txt");
    t.file(L"skip\\b.txt");
    t.file(L"skip\\bad.");

    const SourceItem src{t.root(), {}};
    // Case differs from the folder on disk: exclusions match the way Windows
    // compares paths.
    const std::vector<std::string> excluded{"SKIP"};
    auto r = walkSources({&src, 1}, 1, false, {}, excluded);
    HNX_CHECK(r.has_value());
    if (!r) return;
    HNX_CHECK(hasEntry(*r, "keep/a.txt"));
    HNX_CHECK(std::none_of(r->begin(), r->end(), [](const SourceEntry& e) {
        return e.relPath.starts_with("skip");
    }));
}

HNX_TEST(walk_applies_exclusions_under_a_prefix) {
    ScratchTree t;
    t.file(L"x.txt");
    t.file(L"sub\\y.txt");

    const SourceItem src{t.root(), "extra"};
    const std::vector<std::string> excluded{"extra/sub"};
    auto r = walkSources({&src, 1}, 1, false, {}, excluded);
    HNX_CHECK(r.has_value());
    if (!r) return;
    HNX_CHECK(hasEntry(*r, "extra/x.txt"));
    HNX_CHECK(!hasEntry(*r, "extra/sub/y.txt"));
    HNX_CHECK(!hasEntry(*r, "extra/sub"));
}

HNX_TEST(walk_excludes_a_single_file_source) {
    ScratchTree t;
    t.file(L"lone.dll");

    const SourceItem src{t.root() / "lone.dll", {}};
    const std::vector<std::string> excluded{"lone.dll"};
    auto r = walkSources({&src, 1}, 1, false, {}, excluded);
    HNX_CHECK(r.has_value());
    if (!r) return;
    HNX_CHECK(r->empty());
}
