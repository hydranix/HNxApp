// Path validation and target-path construction.
//
// The archive controls where bytes land on disk, so this is the one module where
// a bug is a code-execution vector rather than a crash. Validation runs at pack
// time and again at unpack time, and a violation rejects the whole archive: a
// partially applied archive is worse than a refused one.
//
// The pure validation half deliberately avoids Windows headers so it can be unit
// tested directly.
#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "error.hpp"

namespace hnx::paths {

// Validates one relative archive path (UTF-8, forward slashes).
// See docs/format.md section 6 for the full rule list and rationale.
[[nodiscard]] std::expected<void, Error> validateArchivePath(std::string_view p);

// True when the component is a reserved DOS device name, with or without an
// extension, case-insensitive. "CON", "con.txt" and "COM9.tar.gz" all match;
// "CONSOLE" and "COM0" do not.
[[nodiscard]] bool isReservedDeviceName(std::string_view component) noexcept;

// Strict UTF-8 check. Rejects overlong encodings, surrogates, out-of-range code
// points and embedded NULs, all of which are ways to smuggle a different string
// past a naive validator.
[[nodiscard]] bool isValidUtf8(std::string_view s) noexcept;

// UTF-8 <-> UTF-16 conversion using the Win32 code page APIs.
[[nodiscard]] std::wstring widen(std::string_view utf8);
[[nodiscard]] std::string narrow(std::wstring_view utf16);

// Returns an absolute, backslash-separated, \\?\-prefixed path, which is what
// every filesystem call in the stub should use. The prefix is what lifts the
// 260-character MAX_PATH limit; without it, deep app trees fail to extract on
// machines where long-path support is not enabled system-wide.
//
// `base` must already be absolute. `relative` must have passed
// validateArchivePath.
[[nodiscard]] std::expected<std::wstring, Error> joinExtended(
    std::wstring_view base, std::string_view relative);

// Adds the \\?\ prefix to an absolute path if it is not already present.
// Handles the UNC form (\\server\share -> \\?\UNC\server\share).
[[nodiscard]] std::wstring toExtendedForm(std::wstring_view absolutePath);

// Removes a \\?\ or \\?\UNC\ prefix, for display in error messages. Users should
// never see the prefix.
[[nodiscard]] std::wstring toDisplayForm(std::wstring_view path);

// Final containment backstop. Canonicalises `candidate` and confirms it is still
// inside `base`, catching anything the component rules missed. Compares
// case-insensitively because NTFS does.
[[nodiscard]] bool isContainedIn(std::wstring_view base, std::wstring_view candidate);

// Expands %VAR% environment references plus the HNx-specific tokens
// %TARGET%, %APPDIR%, %APPID%, %VERSION%.
struct TokenValues {
    std::wstring target;
    std::wstring appDir;
    std::wstring appId;
    std::wstring version;
};
[[nodiscard]] std::wstring expandTokens(std::wstring_view input, const TokenValues& tv);

}  // namespace hnx::paths
