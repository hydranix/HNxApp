// Command-line forms that are not a subcommand.
//
// Split out of main.cpp so the recognition can be unit tested. It is small, but
// it is the piece where being wrong in the permissive direction is expensive:
// mistaking a scripted `build --src ... --out ...` for the interactive shorthand
// would open a modal dialog on a machine with nobody at it, which from the
// outside is indistinguishable from a hang.
#include "packer.hpp"

#include <string_view>

namespace hnx::pack {

std::optional<std::filesystem::path> presetSourceFromArgs(int argc,
                                                          const wchar_t* const* argv) {
    std::optional<std::filesystem::path> src;

    for (int i = 1; i < argc; ++i) {
        const std::wstring_view a{argv[i]};

        // Already acted on before argument parsing, and implied by this form
        // anyway, so it does not disqualify the shorthand.
        if (a == L"--no-console" || a == L"--noconsole") continue;

        if (a == L"--src" && !src && i + 1 < argc) {
            const std::wstring_view value{argv[i + 1]};
            // An empty value is a typo, not the shorthand. Usage is a better
            // answer to it than a dialog complaining that "" is not a folder.
            if (value.empty()) return std::nullopt;
            src.emplace(value);
            ++i;
            continue;
        }

        // Anything else at all -- a subcommand, --out, a second --src, a --src
        // with nothing after it -- means this is not the shorthand, and the
        // caller should fall through to its normal parsing.
        return std::nullopt;
    }

    return src;
}

}  // namespace hnx::pack
