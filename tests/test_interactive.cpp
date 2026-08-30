// The pure logic behind the interactive flow.
//
// The dialogs themselves cannot be exercised headlessly, but sanitizeAppId can,
// and it is the piece with a contract worth pinning: whatever a user names a
// folder, the derived appId has to be a legal single path component, because it
// becomes a directory under %LOCALAPPDATA% and part of the extraction lock
// identity. A folder called "My App (v2.0)." must not produce an id that the
// metadata validator then rejects with a message about JSON.
#include "test.hpp"

#include <string>

#include "packer.hpp"
#include "paths.hpp"

using namespace hnx;
using namespace hnx::pack;

namespace {

// The contract: every result must be a legal archive path component AND contain
// no separator, since it is used as one directory name.
bool isUsableAppId(const std::string& id) {
    if (id.empty()) return false;
    if (id.find('/') != std::string::npos) return false;
    if (id.find('\\') != std::string::npos) return false;
    return paths::validateArchivePath(id).has_value();
}

}  // namespace

HNX_TEST(appid_passes_through_simple_names) {
    HNX_CHECK(sanitizeAppId("myapp") == "myapp");
    HNX_CHECK(sanitizeAppId("my-app") == "my-app");
    HNX_CHECK(sanitizeAppId("my_app_2") == "my_app_2");
}

HNX_TEST(appid_lowercases) {
    HNX_CHECK(sanitizeAppId("MyApp") == "myapp");
    HNX_CHECK(sanitizeAppId("VSCODE") == "vscode");
}

HNX_TEST(appid_replaces_and_collapses_separators) {
    // A run of unusable characters becomes one dash, not several, and never a
    // trailing one.
    HNX_CHECK(sanitizeAppId("My App") == "my-app");
    HNX_CHECK(sanitizeAppId("My   App") == "my-app");
    HNX_CHECK(sanitizeAppId("My App (v2.0)") == "my-app-v2-0");
    HNX_CHECK(sanitizeAppId("Trailing...") == "trailing");
    HNX_CHECK(sanitizeAppId("  spaced  ") == "spaced");
}

HNX_TEST(appid_handles_names_that_would_be_illegal) {
    // These are all perfectly ordinary folder names that would be rejected
    // outright if used as an appId unchanged.
    const char* folders[] = {
        "My App",        "app.v1.2",   "CON",         "NUL",        "COM1",
        "trailing. ",    "..",         ".",           "a/b",        "C:",
        "app:stream",    "  ",         "...",         "-----",      "",
        "\xC3\xA9t\xC3\xA9",           "\xF0\x9F\x9A\x80",
    };

    for (const char* f : folders) {
        const std::string id = sanitizeAppId(f);
        HNX_CHECK(isUsableAppId(id));
    }
}

HNX_TEST(appid_never_yields_a_reserved_device_name) {
    // "CON" as a folder name is unusual but legal on some filesystems and
    // reachable over a network share. The derived id must not be a device name,
    // or the cache directory could never be created.
    HNX_CHECK(!paths::isReservedDeviceName(sanitizeAppId("CON")));
    HNX_CHECK(!paths::isReservedDeviceName(sanitizeAppId("nul")));
    HNX_CHECK(!paths::isReservedDeviceName(sanitizeAppId("Com1")));
    HNX_CHECK(!paths::isReservedDeviceName(sanitizeAppId("LPT9")));
}

HNX_TEST(appid_never_yields_empty) {
    // An empty id would produce a cache path with a missing component, silently
    // collapsing two different applications onto one directory.
    HNX_CHECK(!sanitizeAppId("").empty());
    HNX_CHECK(!sanitizeAppId("   ").empty());
    HNX_CHECK(!sanitizeAppId("...").empty());
    HNX_CHECK(!sanitizeAppId("///").empty());
    HNX_CHECK(!sanitizeAppId("\xF0\x9F\x9A\x80").empty());
}

HNX_TEST(appid_is_stable) {
    // The same folder must always yield the same id: it identifies the cache
    // directory, so an unstable result would re-extract on every launch.
    for (int i = 0; i < 3; ++i) {
        HNX_CHECK(sanitizeAppId("My App (v2.0)") == "my-app-v2-0");
        HNX_CHECK(sanitizeAppId("CON") == sanitizeAppId("CON"));
    }
}
