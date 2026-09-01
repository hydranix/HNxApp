// Which command lines mean "the interactive flow, folder already chosen".
//
// The dialogs themselves cannot be driven headlessly, but the decision to open
// them can be, and that decision is the part with teeth. Recognising too much
// is the expensive failure: a scripted `build --src ... --out ...` mistaken for
// the shorthand would sit on a modal dialog on a machine with nobody at it,
// which from the outside looks exactly like a hang.
#include "test.hpp"

#include <initializer_list>
#include <vector>

#include "packer.hpp"

using namespace hnx::pack;

namespace {

// argv as wmain receives it: argv[0] is the program name, and the scan must skip
// it rather than treat it as an unrecognised option.
std::optional<std::filesystem::path> parse(std::initializer_list<const wchar_t*> args) {
    std::vector<const wchar_t*> argv{L"hnxpack.exe"};
    argv.insert(argv.end(), args);
    return presetSourceFromArgs(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

HNX_TEST(preset_source_recognises_the_shorthand) {
    auto p = parse({L"--src", L"C:\\Apps\\MyApp"});
    HNX_CHECK(p.has_value());
    HNX_CHECK(*p == std::filesystem::path(L"C:\\Apps\\MyApp"));
}

HNX_TEST(preset_source_keeps_the_path_verbatim) {
    // Relative paths, trailing slashes and spaces are all things a user types.
    // Resolving them is the interactive flow's job, not this scan's -- it only
    // decides which flow runs.
    HNX_CHECK(*parse({L"--src", L".\\MyApp"}) == std::filesystem::path(L".\\MyApp"));
    HNX_CHECK(*parse({L"--src", L"C:\\Program Files\\App\\"}) ==
              std::filesystem::path(L"C:\\Program Files\\App\\"));
    HNX_CHECK(*parse({L"--src", L"\\\\server\\share\\app"}) ==
              std::filesystem::path(L"\\\\server\\share\\app"));
}

HNX_TEST(preset_source_tolerates_no_console) {
    // The shorthand hides an owned console anyway, so an explicit --no-console
    // is redundant rather than disqualifying, on either side of --src.
    HNX_CHECK(parse({L"--no-console", L"--src", L"C:\\Apps\\MyApp"}).has_value());
    HNX_CHECK(parse({L"--src", L"C:\\Apps\\MyApp", L"--no-console"}).has_value());
    HNX_CHECK(parse({L"--src", L"C:\\Apps\\MyApp", L"--noconsole"}).has_value());
}

HNX_TEST(preset_source_ignores_a_bare_command_line) {
    // No arguments is the plain interactive flow, which the caller handles
    // separately; this scan must not claim it.
    HNX_CHECK(!parse({}).has_value());
    HNX_CHECK(!parse({L"--no-console"}).has_value());
}

HNX_TEST(preset_source_rejects_scripted_builds) {
    // The case that matters. Every one of these is a scripted invocation that
    // must reach the normal parser, not a dialog.
    HNX_CHECK(!parse({L"build", L"--src", L"C:\\Apps\\MyApp", L"--out", L"a.exe"}).has_value());
    HNX_CHECK(!parse({L"build", L"--src", L"C:\\Apps\\MyApp"}).has_value());
    HNX_CHECK(!parse({L"--src", L"C:\\Apps\\MyApp", L"--out", L"a.exe"}).has_value());
    HNX_CHECK(!parse({L"--src", L"C:\\Apps\\MyApp", L"--arch", L"x86"}).has_value());
    HNX_CHECK(!parse({L"--out", L"a.exe", L"--src", L"C:\\Apps\\MyApp"}).has_value());
}

HNX_TEST(preset_source_rejects_other_commands) {
    HNX_CHECK(!parse({L"inspect", L"a.exe"}).has_value());
    HNX_CHECK(!parse({L"verify", L"a.exe"}).has_value());
    HNX_CHECK(!parse({L"--help"}).has_value());
    HNX_CHECK(!parse({L"-h"}).has_value());
}

HNX_TEST(preset_source_rejects_a_malformed_src) {
    // --src with nothing after it, or with an empty value, is a typo. Falling
    // through to usage says so; opening a dialog would not.
    HNX_CHECK(!parse({L"--src"}).has_value());
    HNX_CHECK(!parse({L"--src", L""}).has_value());
    HNX_CHECK(!parse({L"--no-console", L"--src"}).has_value());
}

HNX_TEST(preset_source_rejects_a_repeated_src) {
    // The shorthand packages one folder. Several --src values is the build
    // subcommand's repeatable form, and silently taking the first would package
    // something other than what was asked for.
    HNX_CHECK(!parse({L"--src", L"C:\\A", L"--src", L"C:\\B"}).has_value());
}

HNX_TEST(preset_source_does_not_treat_a_value_as_an_option) {
    // A folder genuinely named "--no-console" is absurd, but the value slot must
    // be consumed positionally regardless, or a path could silently change the
    // meaning of the scan.
    auto p = parse({L"--src", L"--no-console"});
    HNX_CHECK(p.has_value());
    HNX_CHECK(*p == std::filesystem::path(L"--no-console"));
}
