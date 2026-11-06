// Metadata parsing, focused on the firewall fields that drive the temporary
// WFP rules. The rule application itself needs an elevated process and the Base
// Filtering Engine, so it cannot be exercised here; the parsing can.
#include "test.hpp"

#include "meta.hpp"

using namespace hnx;

HNX_TEST(firewall_absent_by_default) {
    auto m = Metadata::parse(R"({ "appId": "demo", "exec": "app.exe" })");
    HNX_CHECK(m.has_value());
    if (m) {
        HNX_CHECK(!m->firewall.active());
        HNX_CHECK(!m->firewall.blockInbound);
        HNX_CHECK(!m->firewall.blockOutbound);
        HNX_CHECK(m->firewall.programs.empty());
    }
}

HNX_TEST(firewall_block_flags_parse) {
    auto m = Metadata::parse(R"({
        "appId": "demo", "exec": "app.exe",
        "firewallBlockInbound": true,
        "firewallBlockOutbound": true
    })");
    HNX_CHECK(m.has_value());
    if (m) {
        HNX_CHECK(m->firewall.blockInbound);
        HNX_CHECK(m->firewall.blockOutbound);
        HNX_CHECK(m->firewall.active());
    }
}

HNX_TEST(firewall_outbound_only_is_active) {
    auto m = Metadata::parse(R"({
        "appId": "demo", "exec": "app.exe",
        "firewallBlockOutbound": true
    })");
    HNX_CHECK(m.has_value());
    if (m) {
        HNX_CHECK(m->firewall.active());
        HNX_CHECK(!m->firewall.blockInbound);
    }
}

HNX_TEST(firewall_programs_split_and_trim) {
    // Semicolon-separated, surrounding whitespace trimmed, a trailing separator
    // and an empty field ignored.
    auto m = Metadata::parse(R"({
        "appId": "demo", "exec": "app.exe",
        "firewallBlockOutbound": true,
        "firewallPrograms": "bin/helper.exe ;  tools/net.exe;"
    })");
    HNX_CHECK(m.has_value());
    if (m) {
        HNX_CHECK_EQ(m->firewall.programs.size(), static_cast<std::size_t>(2));
        if (m->firewall.programs.size() == 2) {
            HNX_CHECK(m->firewall.programs[0] == L"bin/helper.exe");
            HNX_CHECK(m->firewall.programs[1] == L"tools/net.exe");
        }
    }
}

HNX_TEST(firewall_programs_keep_backslashes) {
    // Backslashes are validated as an archive path (normalised to forward slashes
    // for the check) but stored as written, matching how `exec` is kept.
    auto m = Metadata::parse(R"({
        "appId": "demo", "exec": "app.exe",
        "firewallBlockOutbound": true,
        "firewallPrograms": "bin\\helper.exe"
    })");
    HNX_CHECK(m.has_value());
    if (m) {
        HNX_CHECK_EQ(m->firewall.programs.size(), static_cast<std::size_t>(1));
        if (!m->firewall.programs.empty())
            HNX_CHECK(m->firewall.programs[0] == L"bin\\helper.exe");
    }
}

HNX_TEST(firewall_programs_reject_traversal) {
    auto m = Metadata::parse(R"({
        "appId": "demo", "exec": "app.exe",
        "firewallBlockOutbound": true,
        "firewallPrograms": "../escape.exe"
    })");
    HNX_CHECK(!m.has_value());
}

HNX_TEST(firewall_programs_reject_absolute) {
    // A drive letter brings a ':' with it, which path validation rejects outright.
    auto m = Metadata::parse(R"({
        "appId": "demo", "exec": "app.exe",
        "firewallBlockOutbound": true,
        "firewallPrograms": "C:/Windows/System32/cmd.exe"
    })");
    HNX_CHECK(!m.has_value());
}

HNX_TEST(firewall_block_flag_must_be_bool) {
    auto m = Metadata::parse(R"({
        "appId": "demo", "exec": "app.exe",
        "firewallBlockInbound": "yes"
    })");
    HNX_CHECK(!m.has_value());
}
