// Windows Preinstallation Environment support.
//
// WinPE is a normal Win32 environment with two properties that matter here:
//
//   * None of the profile variables are defined. %LOCALAPPDATA% -- which the
//     default targetDir is built from -- expands to itself, so the target
//     resolves to a relative path and a directory literally named
//     "%LOCALAPPDATA%" appears beside the current directory.
//   * The system drive is X:, a RAM disk. Its scratch space is 32 MB by default
//     and 512 MB at most, so running out of room is the ordinary case there
//     rather than an exotic one.
//
// The first is what this module fixes; the second is why the caller checks free
// space before writing and why --hnx-target= remains the way to reach a real
// disk.
#pragma once

#include <string>
#include <string_view>

namespace hnx::winpe {

// True when this process is running under WinPE.
//
// The MiniNT key exists only there: it is what WinPE's own components test, and
// unlike looking at %SystemDrive% it does not mistake a machine that happens to
// have booted from X: for a preinstallation environment. Cached, since the
// answer cannot change while the process lives.
[[nodiscard]] bool active();

// Where a package unpacks under WinPE. Reads TEMP and SystemRoot from the
// environment and applies chooseScratchRoot to them.
[[nodiscard]] std::wstring scratchRoot();

// The scratch root policy, separated from the environment so it can be tested.
// TEMP first, then SystemRoot\Temp, then the location WinPE always puts it.
// The returned path has no trailing separator.
[[nodiscard]] std::wstring chooseScratchRoot(std::wstring_view temp,
                                             std::wstring_view systemRoot);

// True for a drive-qualified path (C:\...) or a UNC/extended one (\\...).
// "X:foo" is drive-relative, not absolute, and is rejected.
[[nodiscard]] bool isAbsolute(std::wstring_view path) noexcept;

}  // namespace hnx::winpe
