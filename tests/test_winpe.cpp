// WinPE target-directory policy.
//
// active() is not testable here -- it reports the environment this test process
// happens to be running in -- but the two decisions that follow from it are pure
// functions of their inputs, and they are the ones that would silently put a
// package's files in the wrong place.
#include "test.hpp"

#include "winpe.hpp"

using namespace hnx;

HNX_TEST(absolute_paths_are_recognised) {
    HNX_CHECK(winpe::isAbsolute(LR"(C:\)"));
    HNX_CHECK(winpe::isAbsolute(LR"(X:\Windows\Temp)"));
    HNX_CHECK(winpe::isAbsolute(LR"(c:\lower\drive)"));
    HNX_CHECK(winpe::isAbsolute(L"D:/forward/slashes"));
    HNX_CHECK(winpe::isAbsolute(LR"(\\server\share)"));
    HNX_CHECK(winpe::isAbsolute(LR"(\\?\C:\extended)"));
}

HNX_TEST(non_absolute_paths_are_rejected) {
    HNX_CHECK(!winpe::isAbsolute(L""));
    HNX_CHECK(!winpe::isAbsolute(L"relative\\path"));
    HNX_CHECK(!winpe::isAbsolute(LR"(\rooted-but-driveless)"));

    // The shape the default targetDir takes when %LOCALAPPDATA% does not expand.
    HNX_CHECK(!winpe::isAbsolute(LR"(%LOCALAPPDATA%\HNx\app\1.0)"));

    // Drive-relative: "X:foo" means "foo, relative to the current directory on
    // X:", which is not somewhere we can safely unpack.
    HNX_CHECK(!winpe::isAbsolute(L"X:foo"));
    HNX_CHECK(!winpe::isAbsolute(L"C:"));

    // A colon in the second position is not enough to make a drive letter.
    HNX_CHECK(!winpe::isAbsolute(LR"(1:\digits)"));
}

HNX_TEST(scratch_root_prefers_temp) {
    HNX_CHECK_EQ(winpe::chooseScratchRoot(LR"(X:\Windows\Temp)", LR"(X:\Windows)"),
                 std::wstring(LR"(X:\Windows\Temp)"));

    // An image whose scratch space was relocated says so in TEMP, and that wins
    // over the conventional location.
    HNX_CHECK_EQ(winpe::chooseScratchRoot(LR"(D:\scratch)", LR"(X:\Windows)"),
                 std::wstring(LR"(D:\scratch)"));
}

HNX_TEST(scratch_root_trims_trailing_separators) {
    HNX_CHECK_EQ(winpe::chooseScratchRoot(LR"(D:\scratch\)", L""),
                 std::wstring(LR"(D:\scratch)"));
    HNX_CHECK_EQ(winpe::chooseScratchRoot(L"D:/scratch/", L""),
                 std::wstring(L"D:/scratch"));

    // A drive root reduces to the drive, which is still a usable prefix once a
    // subdirectory is appended to it.
    HNX_CHECK_EQ(winpe::chooseScratchRoot(LR"(D:\)", L""), std::wstring(L"D:"));
}

HNX_TEST(scratch_root_falls_back_to_system_root) {
    HNX_CHECK_EQ(winpe::chooseScratchRoot(L"", LR"(X:\Windows)"),
                 std::wstring(LR"(X:\Windows\Temp)"));
    HNX_CHECK_EQ(winpe::chooseScratchRoot(L"", LR"(Y:\Windows\)"),
                 std::wstring(LR"(Y:\Windows\Temp)"));

    // A TEMP that is set but unusable is ignored rather than joined onto.
    HNX_CHECK_EQ(winpe::chooseScratchRoot(L"%TEMP%", LR"(X:\Windows)"),
                 std::wstring(LR"(X:\Windows\Temp)"));
}

HNX_TEST(scratch_root_falls_back_to_the_winpe_ram_disk) {
    HNX_CHECK_EQ(winpe::chooseScratchRoot(L"", L""),
                 std::wstring(LR"(X:\Windows\Temp)"));
    HNX_CHECK_EQ(winpe::chooseScratchRoot(L"relative", L"also-relative"),
                 std::wstring(LR"(X:\Windows\Temp)"));
}
