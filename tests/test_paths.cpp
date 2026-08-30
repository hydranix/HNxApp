// Path validation tests.
//
// This is the module where a bug becomes a code-execution vector, since the
// archive controls where bytes land on disk. Every rule in docs/format.md
// section 6 gets both a rejection case and a "this legitimate thing still works"
// case -- an over-eager validator that refuses valid filenames is its own kind of
// bug.
#include "test.hpp"

#include "paths.hpp"

using namespace hnx;
using namespace hnx::paths;

namespace {

bool accepts(std::string_view p) { return validateArchivePath(p).has_value(); }

Ec rejectionOf(std::string_view p) {
    auto r = validateArchivePath(p);
    return r ? Ec::Ok : r.error().code;
}

}  // namespace

HNX_TEST(accepts_ordinary_paths) {
    HNX_CHECK(accepts("file.txt"));
    HNX_CHECK(accepts("bin/app.exe"));
    HNX_CHECK(accepts("a/b/c/d/e/deep.dat"));
    HNX_CHECK(accepts("no-extension"));
    HNX_CHECK(accepts("dots.in.the.name.tar.gz"));
    HNX_CHECK(accepts("space in name.txt"));
    HNX_CHECK(accepts("UPPER/MiXeD/lower.TXT"));
}

HNX_TEST(accepts_unicode) {
    HNX_CHECK(accepts("\xC3\xA9t\xC3\xA9.txt"));                  // été.txt
    HNX_CHECK(accepts("\xE6\x97\xA5\xE6\x9C\xAC/file.txt"));      // 日本/
    HNX_CHECK(accepts("emoji_\xF0\x9F\x9A\x80.dat"));             // 🚀
}

HNX_TEST(rejects_absolute_paths) {
    HNX_CHECK_EQ(rejectionOf("/etc/passwd"), Ec::PathAbsolute);
    HNX_CHECK_EQ(rejectionOf("\\windows\\system32"), Ec::PathAbsolute);
    HNX_CHECK_EQ(rejectionOf("C:/Windows/System32/evil.dll"), Ec::PathAbsolute);
    HNX_CHECK_EQ(rejectionOf("C:evil.dll"), Ec::PathAbsolute);
    // A backslash anywhere means the producer did not normalise, and treating it
    // as an ordinary character would defeat forward-slash-only splitting.
    HNX_CHECK_EQ(rejectionOf("bin\\app.exe"), Ec::PathAbsolute);
}

HNX_TEST(rejects_traversal) {
    HNX_CHECK_EQ(rejectionOf(".."), Ec::PathTraversal);
    HNX_CHECK_EQ(rejectionOf("../evil"), Ec::PathTraversal);
    HNX_CHECK_EQ(rejectionOf("a/../../evil"), Ec::PathTraversal);
    HNX_CHECK_EQ(rejectionOf("a/b/.."), Ec::PathTraversal);
}

HNX_TEST(traversal_check_is_per_component_not_substring) {
    // These contain ".." as a substring but are perfectly legal filenames. A
    // naive s.find("..") check would reject all of them.
    HNX_CHECK(accepts("..foo"));
    HNX_CHECK(accepts("a..b/c..d"));
    HNX_CHECK(accepts("weird...name"));

    // "foo.." is NOT in that list. It is caught by the trailing-dot rule rather
    // than the traversal rule, and that is correct: Windows strips trailing dots,
    // so "foo.." and "foo" resolve to the same file on disk.
    HNX_CHECK_EQ(rejectionOf("foo.."), Ec::PathTrailingDotOrSpace);
}

HNX_TEST(rejects_alternate_data_streams) {
    HNX_CHECK_EQ(rejectionOf("readme.txt:hidden"), Ec::PathHasStream);
    HNX_CHECK_EQ(rejectionOf("a/b:stream:$DATA"), Ec::PathHasStream);
}

HNX_TEST(rejects_reserved_device_names) {
    HNX_CHECK_EQ(rejectionOf("CON"), Ec::PathReservedName);
    HNX_CHECK_EQ(rejectionOf("con"), Ec::PathReservedName);
    HNX_CHECK_EQ(rejectionOf("PRN"), Ec::PathReservedName);
    HNX_CHECK_EQ(rejectionOf("AUX"), Ec::PathReservedName);
    HNX_CHECK_EQ(rejectionOf("NUL"), Ec::PathReservedName);
    HNX_CHECK_EQ(rejectionOf("COM1"), Ec::PathReservedName);
    HNX_CHECK_EQ(rejectionOf("LPT9"), Ec::PathReservedName);
    HNX_CHECK_EQ(rejectionOf("dir/NUL"), Ec::PathReservedName);

    // With an extension still resolves to the device: Windows takes the name
    // before the FIRST dot, so checking only the final stem would let these
    // through.
    HNX_CHECK_EQ(rejectionOf("CON.txt"), Ec::PathReservedName);
    HNX_CHECK_EQ(rejectionOf("COM1.tar.gz"), Ec::PathReservedName);
}

HNX_TEST(reserved_name_check_does_not_overreach) {
    HNX_CHECK(accepts("CONSOLE"));
    HNX_CHECK(accepts("CONFIG.SYS"));
    HNX_CHECK(accepts("PRINTER"));
    HNX_CHECK(accepts("COM0"));    // only 1-9 are devices
    HNX_CHECK(accepts("COM10"));
    HNX_CHECK(accepts("LPT0"));
    HNX_CHECK(accepts("AUXILIARY"));
    HNX_CHECK(accepts("NULL.txt"));
}

HNX_TEST(rejects_trailing_dot_or_space) {
    // Windows silently strips these, so "evil. " and "evil" would collide on one
    // real file and the second entry would overwrite the first.
    HNX_CHECK_EQ(rejectionOf("evil."), Ec::PathTrailingDotOrSpace);
    HNX_CHECK_EQ(rejectionOf("evil "), Ec::PathTrailingDotOrSpace);
    HNX_CHECK_EQ(rejectionOf("dir./file"), Ec::PathTrailingDotOrSpace);
    HNX_CHECK_EQ(rejectionOf("dir /file"), Ec::PathTrailingDotOrSpace);
}

HNX_TEST(rejects_empty_and_degenerate_components) {
    HNX_CHECK_EQ(rejectionOf(""), Ec::PathEmptyComponent);
    HNX_CHECK_EQ(rejectionOf("a//b"), Ec::PathEmptyComponent);
    HNX_CHECK_EQ(rejectionOf("a/"), Ec::PathEmptyComponent);
    HNX_CHECK_EQ(rejectionOf("."), Ec::PathEmptyComponent);
    HNX_CHECK_EQ(rejectionOf("a/./b"), Ec::PathEmptyComponent);
}

HNX_TEST(rejects_control_characters) {
    HNX_CHECK(!accepts(std::string_view("a\x01\x62", 3)));
    HNX_CHECK(!accepts(std::string_view("tab\there", 8)));
    HNX_CHECK(!accepts(std::string_view("nl\nhere", 7)));
}

HNX_TEST(rejects_invalid_utf8) {
    HNX_CHECK_EQ(rejectionOf("\xFF\xFE"), Ec::PathNotUtf8);
    HNX_CHECK_EQ(rejectionOf("\x80\x80"), Ec::PathNotUtf8);          // lone continuation
    HNX_CHECK_EQ(rejectionOf("\xC3"), Ec::PathNotUtf8);              // truncated
    // Overlong encoding of '/', a classic way to smuggle a separator past a
    // byte-wise filter.
    HNX_CHECK_EQ(rejectionOf("\xC0\xAF"), Ec::PathNotUtf8);
    // Overlong encoding of '.', which would rebuild ".." past the component check.
    HNX_CHECK_EQ(rejectionOf("\xC0\xAE\xC0\xAE"), Ec::PathNotUtf8);
    // Lone surrogate, not legal in UTF-8.
    HNX_CHECK_EQ(rejectionOf("\xED\xA0\x80"), Ec::PathNotUtf8);
}

HNX_TEST(utf8_validator_accepts_valid_sequences) {
    HNX_CHECK(isValidUtf8("plain ascii"));
    HNX_CHECK(isValidUtf8("\xC3\xA9"));                  // 2-byte
    HNX_CHECK(isValidUtf8("\xE6\x97\xA5"));              // 3-byte
    HNX_CHECK(isValidUtf8("\xF0\x9F\x9A\x80"));          // 4-byte
    HNX_CHECK(!isValidUtf8(std::string_view("a\0b", 3)));  // embedded NUL
}

HNX_TEST(containment_rejects_sibling_prefix) {
    // C:\app must not be considered to contain C:\application: a pure prefix
    // comparison would say yes, which is how a containment check that forgets the
    // separator lets a write escape.
    HNX_CHECK(!isContainedIn(LR"(C:\app)", LR"(C:\application\x.txt)"));
    HNX_CHECK(isContainedIn(LR"(C:\app)", LR"(C:\app\x.txt)"));
    HNX_CHECK(isContainedIn(LR"(C:\app)", LR"(C:\app)"));
}

HNX_TEST(containment_is_case_insensitive) {
    // NTFS compares case-insensitively, so a case-sensitive check would be
    // trivially bypassed by changing the case of the target directory.
    HNX_CHECK(isContainedIn(LR"(C:\App)", LR"(C:\app\x.txt)"));
    HNX_CHECK(isContainedIn(LR"(C:\APP)", LR"(C:\aPp\sub\y.txt)"));
}

HNX_TEST(containment_resolves_dot_dot) {
    HNX_CHECK(!isContainedIn(LR"(C:\app)", LR"(C:\app\..\evil.txt)"));
    HNX_CHECK(isContainedIn(LR"(C:\app)", LR"(C:\app\sub\..\ok.txt)"));
}

HNX_TEST(join_produces_extended_paths) {
    auto r = joinExtended(LR"(C:\target)", "bin/app.exe");
    HNX_CHECK(r.has_value());
    if (r) HNX_CHECK(*r == LR"(\\?\C:\target\bin\app.exe)");
}

HNX_TEST(join_rejects_unsafe_relative_paths) {
    HNX_CHECK(!joinExtended(LR"(C:\target)", "../escape.txt").has_value());
    HNX_CHECK(!joinExtended(LR"(C:\target)", "C:/abs.txt").has_value());
    HNX_CHECK(!joinExtended(LR"(C:\target)", "CON").has_value());
}

HNX_TEST(extended_form_handles_unc) {
    HNX_CHECK(toExtendedForm(LR"(C:\a)") == LR"(\\?\C:\a)");
    HNX_CHECK(toExtendedForm(LR"(\\server\share\a)") == LR"(\\?\UNC\server\share\a)");
    // Already-prefixed paths must not be double-prefixed.
    HNX_CHECK(toExtendedForm(LR"(\\?\C:\a)") == LR"(\\?\C:\a)");
}

HNX_TEST(display_form_round_trips) {
    HNX_CHECK(toDisplayForm(toExtendedForm(LR"(C:\a\b)")) == LR"(C:\a\b)");
    HNX_CHECK(toDisplayForm(toExtendedForm(LR"(\\srv\share\x)")) == LR"(\\srv\share\x)");
}

HNX_TEST(token_expansion) {
    TokenValues tv;
    tv.target = LR"(C:\t)";
    tv.appDir = LR"(D:\apps)";
    tv.appId = L"myapp";
    tv.version = L"2.1";

    HNX_CHECK(expandTokens(L"%TARGET%\\data", tv) == LR"(C:\t\data)");
    HNX_CHECK(expandTokens(L"%APPDIR%\\x", tv) == LR"(D:\apps\x)");
    HNX_CHECK(expandTokens(L"%APPID%-%VERSION%", tv) == L"myapp-2.1");
    HNX_CHECK(expandTokens(L"no tokens here", tv) == L"no tokens here");
}

HNX_TEST(token_expansion_precedes_environment_expansion) {
    // HNx tokens resolve first so that a value pulled out of the environment can
    // never itself be reinterpreted as a token.
    TokenValues tv;
    tv.appId = L"%TARGET%";
    tv.target = LR"(C:\real)";
    const std::wstring out = expandTokens(L"%APPID%", tv);
    HNX_CHECK(out == L"%TARGET%");
}
