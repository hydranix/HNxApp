// Deriving an appId from a folder name.
//
// Its own translation unit, and not part of interactive.cpp, for two reasons: it
// is pure string logic with nothing to do with dialogs, and the tests can link it
// without dragging in COM and the whole build pipeline behind it.
#include "packer.hpp"

#include "paths.hpp"

namespace hnx::pack {

// Derives an appId from a folder name.
//
// appId becomes a directory name under %LOCALAPPDATA% and part of the lock
// identity, so it has to survive validateArchivePath as a single safe component.
// Folder names routinely contain spaces, dots and other things that do not.
//
// Not in the anonymous namespace so the tests can hold it to that contract
// directly: getting this wrong produces a build failure with a confusing message
// about metadata rather than about the folder name that caused it.
std::string sanitizeAppId(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());

    for (char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        if ((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || c == '-' || c == '_') {
            out.push_back(c);
        } else if (u >= 'A' && u <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else if (u >= 0x80) {
            // Non-ASCII is valid in a path but makes for a poor cache directory
            // name and a poor lock identity. Keep the id ASCII and let appName
            // carry the real title.
            out.push_back('-');
        } else {
            out.push_back('-');
        }
    }

    // Collapse runs and trim, so "My App (v2)" becomes "my-app-v2" rather than
    // "my-app--v2-".
    std::string collapsed;
    for (char c : out) {
        if (c == '-' && (collapsed.empty() || collapsed.back() == '-')) continue;
        collapsed.push_back(c);
    }
    while (!collapsed.empty() && collapsed.back() == '-') collapsed.pop_back();

    if (collapsed.empty()) collapsed = "app";

    // A sanitized name can still land on a reserved device name.
    if (paths::isReservedDeviceName(collapsed)) collapsed += "-app";

    return collapsed;
}

}  // namespace hnx::pack
