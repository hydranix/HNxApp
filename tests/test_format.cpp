// Format-level tests: struct layout, CRC, and the JSON metadata parser.
//
// The layout assertions matter because the packer and stub are separate binaries
// that agree only by virtue of sharing format.hpp. A silent padding change would
// produce archives that parse as garbage rather than failing loudly.
#include "test.hpp"

#include <cstring>
#include <string>
#include <vector>

#include "format.hpp"
#include "meta.hpp"

using namespace hnx;

HNX_TEST(struct_sizes_are_pinned) {
    HNX_CHECK_EQ(sizeof(Footer), 64u);
    HNX_CHECK_EQ(sizeof(TocHeader), 24u);
    HNX_CHECK_EQ(sizeof(BlockDesc), 24u);
    HNX_CHECK_EQ(sizeof(EntryDesc), 48u);
    HNX_CHECK_EQ(sizeof(StateHeader), 32u);
    HNX_CHECK_EQ(sizeof(StateRecord), 24u);
}

HNX_TEST(footer_field_offsets) {
    HNX_CHECK_EQ(offsetof(Footer, magic), 0u);
    HNX_CHECK_EQ(offsetof(Footer, formatVersion), 8u);
    HNX_CHECK_EQ(offsetof(Footer, archiveOffset), 16u);
    HNX_CHECK_EQ(offsetof(Footer, archiveSize), 24u);
    HNX_CHECK_EQ(offsetof(Footer, tocOffset), 32u);
    HNX_CHECK_EQ(offsetof(Footer, buildId), 52u);
    HNX_CHECK_EQ(offsetof(Footer, footerCrc32), 60u);
}

HNX_TEST(crc32_matches_known_vectors) {
    auto crcOf = [](std::string_view s) {
        return crc32({reinterpret_cast<const std::byte*>(s.data()), s.size()});
    };
    HNX_CHECK_EQ(crcOf(""), 0x00000000u);
    HNX_CHECK_EQ(crcOf("a"), 0xE8B7BE43u);
    HNX_CHECK_EQ(crcOf("123456789"), 0xCBF43926u);
    HNX_CHECK_EQ(crcOf("The quick brown fox jumps over the lazy dog"), 0x414FA339u);
}

HNX_TEST(footer_crc_covers_every_field_but_itself) {
    Footer f{};
    std::memcpy(f.magic, FOOTER_MAGIC, sizeof(FOOTER_MAGIC));
    f.formatVersion = FORMAT_VERSION;
    f.archiveOffset = 12345;
    f.archiveSize = 999;
    f.buildId = 0xDEADBEEFCAFEF00Dull;
    f.footerCrc32 = footerCrc(f);

    HNX_CHECK(footerMagicOk(f));
    HNX_CHECK_EQ(f.footerCrc32, footerCrc(f));

    // Any change to a covered field must invalidate the CRC.
    Footer g = f;
    g.archiveOffset = 12346;
    HNX_CHECK(g.footerCrc32 != footerCrc(g));

    // Changing only the CRC field itself must not change the computed value,
    // which is what proves the CRC does not cover its own storage.
    Footer h = f;
    h.footerCrc32 = 0;
    HNX_CHECK_EQ(footerCrc(h), footerCrc(f));
}

HNX_TEST(footer_magic_detects_corruption) {
    Footer f{};
    std::memcpy(f.magic, FOOTER_MAGIC, sizeof(FOOTER_MAGIC));
    HNX_CHECK(footerMagicOk(f));
    f.magic[7] = 0xFF;
    HNX_CHECK(!footerMagicOk(f));
}

// ---------------------------------------------------------------------------
// Metadata JSON
// ---------------------------------------------------------------------------

HNX_TEST(metadata_parses_a_full_document) {
    const char* json = R"({
        "schemaVersion": 1,
        "appId": "my-app",
        "appName": "My App",
        "appVersion": "2.3.4",
        "targetDir": "%LOCALAPPDATA%\\HNx\\%APPID%",
        "exec": "bin\\app.exe",
        "args": "--flag",
        "workDir": "%TARGET%",
        "env": { "A": "1", "B": "2" },
        "argPassthrough": false,
        "requireAdmin": true,
        "waitMode": "wait",
        "verify": "paranoid",
        "ui": "console",
        "showWindow": "maximized",
        "allowStubFlags": false
    })";

    auto m = Metadata::parse(json);
    HNX_CHECK(m.has_value());
    if (!m) return;

    HNX_CHECK(m->appId == L"my-app");
    HNX_CHECK(m->appName == L"My App");
    HNX_CHECK(m->appVersion == L"2.3.4");
    HNX_CHECK(m->exec == LR"(bin\app.exe)");
    HNX_CHECK(m->args == L"--flag");
    HNX_CHECK_EQ(m->env.size(), 2u);
    HNX_CHECK(!m->argPassthrough);
    HNX_CHECK(m->requireAdmin);
    HNX_CHECK(m->waitMode == WaitMode::Wait);
    HNX_CHECK(m->verify == VerifyMode::Paranoid);
    HNX_CHECK(m->ui == UiMode::Console);
    HNX_CHECK(!m->allowStubFlags);
}

HNX_TEST(metadata_applies_defaults) {
    auto m = Metadata::parse(R"({"appId":"a","exec":"a.exe"})");
    HNX_CHECK(m.has_value());
    if (!m) return;
    HNX_CHECK(m->verify == VerifyMode::Normal);
    HNX_CHECK(m->ui == UiMode::Progress);
    HNX_CHECK(m->waitMode == WaitMode::None);
    HNX_CHECK(m->argPassthrough);
    HNX_CHECK(!m->targetDir.empty());
    HNX_CHECK(m->workDir == L"%TARGET%");
}

HNX_TEST(metadata_requires_appid_and_exec) {
    HNX_CHECK(!Metadata::parse(R"({"exec":"a.exe"})").has_value());
    HNX_CHECK(!Metadata::parse(R"({"appId":"a"})").has_value());
    HNX_CHECK(!Metadata::parse(R"({"appId":"","exec":"a.exe"})").has_value());
}

HNX_TEST(metadata_ignores_unknown_keys) {
    // Forward compatibility: a newer packer must be able to add fields without
    // breaking stubs already in the wild.
    auto m = Metadata::parse(
        R"({"appId":"a","exec":"a.exe","futureField":"x","anotherOne":42})");
    HNX_CHECK(m.has_value());
}

HNX_TEST(metadata_rejects_malformed_json) {
    HNX_CHECK(!Metadata::parse("").has_value());
    HNX_CHECK(!Metadata::parse("{").has_value());
    HNX_CHECK(!Metadata::parse(R"({"appId":})").has_value());
    HNX_CHECK(!Metadata::parse(R"({"appId":"a" "exec":"b"})").has_value());  // missing comma
    HNX_CHECK(!Metadata::parse(R"({appId:"a"})").has_value());               // unquoted key
    // Invalid escape. This is the one that caught a real mistake during
    // development: a config written with single backslashes produced \H, and
    // accepting it silently would have yielded a wrong target directory.
    HNX_CHECK(!Metadata::parse(R"({"appId":"a","exec":"b","targetDir":"C:\Windows"})")
                   .has_value());
}

HNX_TEST(metadata_rejects_bad_enum_values) {
    HNX_CHECK(!Metadata::parse(R"({"appId":"a","exec":"b","verify":"turbo"})").has_value());
    HNX_CHECK(!Metadata::parse(R"({"appId":"a","exec":"b","waitMode":"maybe"})").has_value());
    HNX_CHECK(!Metadata::parse(R"({"appId":"a","exec":"b","ui":"fancy"})").has_value());
}

HNX_TEST(metadata_rejects_wrong_types) {
    HNX_CHECK(!Metadata::parse(R"({"appId":123,"exec":"b"})").has_value());
    HNX_CHECK(!Metadata::parse(R"({"appId":"a","exec":"b","argPassthrough":"yes"})")
                   .has_value());
    HNX_CHECK(!Metadata::parse(R"({"appId":"a","exec":"b","env":"notanobject"})").has_value());
}

HNX_TEST(metadata_rejects_unsafe_exec_and_appid) {
    // exec is joined against the target directory, so it is exactly as dangerous
    // as any archive path and gets the same validation.
    HNX_CHECK(!Metadata::parse(R"({"appId":"a","exec":"../escape.exe"})").has_value());
    HNX_CHECK(!Metadata::parse(R"({"appId":"a","exec":"C:/Windows/evil.exe"})").has_value());
    // appId becomes a directory name and part of the lock identity.
    HNX_CHECK(!Metadata::parse(R"({"appId":"../evil","exec":"a.exe"})").has_value());
    HNX_CHECK(!Metadata::parse(R"({"appId":"sub/dir","exec":"a.exe"})").has_value());
    HNX_CHECK(!Metadata::parse(R"({"appId":"CON","exec":"a.exe"})").has_value());
}

HNX_TEST(metadata_accepts_backslash_exec) {
    // Backslashes are the natural way to write a Windows relative path in the
    // config, and are normalised internally.
    auto m = Metadata::parse(R"({"appId":"a","exec":"bin\\sub\\app.exe"})");
    HNX_CHECK(m.has_value());
}

HNX_TEST(metadata_handles_unicode_escapes) {
    auto m = Metadata::parse(R"({"appId":"a","exec":"a.exe","appName":"caf\u00e9"})");
    HNX_CHECK(m.has_value());
    if (m) HNX_CHECK(m->appName == L"caf\u00e9");

    // Surrogate pair for U+1F680.
    auto m2 = Metadata::parse(R"({"appId":"a","exec":"a.exe","appName":"\ud83d\ude80"})");
    HNX_CHECK(m2.has_value());

    // An unpaired surrogate is not valid and must not silently become a
    // replacement character.
    HNX_CHECK(!Metadata::parse(R"({"appId":"a","exec":"a.exe","appName":"\ud83d"})")
                   .has_value());
}

HNX_TEST(metadata_tolerates_utf8_bom) {
    // Easy to introduce by editing the resource by hand in a Windows editor.
    std::string doc = "\xEF\xBB\xBF";
    doc += R"({"appId":"a","exec":"a.exe"})";
    HNX_CHECK(Metadata::parse(doc).has_value());
}
