// The no-argument flow.
//
//   1. pick the application folder
//   2. pick the entry point, starting inside that folder
//   3. pick where to save, defaulting to the folder's name plus .exe, in
//      whichever directory was used last time
//
// Metadata, icons, version strings and the stub architecture are all derived from
// what the user picked, so a working package comes out without ever writing a
// config file.
//
// This is the quick path. The GUI covers the same ground with every option
// exposed and a file tree; this exists for the common case of "package this
// folder, done".
#include "packer.hpp"

#include <print>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "dialogs.hpp"
#include "paths.hpp"

namespace hnx::pack {
namespace {

namespace fs = std::filesystem;

std::string jsonEscapeLocal(std::string_view s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

void report(const wchar_t* title, const std::wstring& text, UINT icon) {
    ::MessageBoxW(nullptr, text.c_str(), title, MB_OK | icon | MB_SETFOREGROUND);
}

}  // namespace

bool canShowDialogs() {
    // A packer run from CI with no arguments must not block forever on a dialog
    // nobody can see. An interactive window station is the cheapest reliable
    // signal that a desktop exists.
    HWINSTA station = ::GetProcessWindowStation();
    if (!station) return false;

    USEROBJECTFLAGS flags{};
    DWORD needed = 0;
    if (!::GetUserObjectInformationW(station, UOI_FLAGS, &flags, sizeof(flags), &needed))
        return false;

    return (flags.dwFlags & WSF_VISIBLE) != 0;
}

int runInteractive() {
    // Checked before the first dialog, not at the end.
    //
    // The failure this guards against was exactly the opposite: the user answered
    // all three questions and only then got "stub executable not found".
    if (!hasEmbeddedStub(StubArch::X64) && !hasEmbeddedStub(StubArch::X86)) {
        report(L"HNx",
               L"This copy of hnxpack has no launcher stub built into it, so it "
               L"cannot produce a package.\n\n"
               L"Rebuild hnxpack, or use the command line and pass --stub with a "
               L"stub executable.",
               MB_ICONERROR);
        std::println(stderr, "hnxpack: no embedded stub");
        return 1;
    }

    ComScope com;
    if (!com.ok()) {
        std::println(stderr, "hnxpack: cannot initialise COM for the file dialogs");
        return 1;
    }

    // --- 1. the application folder -----------------------------------------
    auto sourceDir = pickFolder(nullptr, L"Select the application folder to package",
                                loadSetting(SETTING_LAST_SOURCE_DIR));
    if (!sourceDir) return 1;  // cancelled

    std::error_code ec;
    if (!fs::is_directory(*sourceDir, ec)) {
        report(L"HNx", L"That is not a folder.", MB_ICONERROR);
        return 1;
    }

    saveSetting(SETTING_LAST_SOURCE_DIR, sourceDir->parent_path().wstring());

    // --- 2. the entry point, inside that folder -----------------------------
    auto execFile = pickProgramFile(
        nullptr, L"Select the program to run when this package starts",
        sourceDir->wstring(), /*forceStartDir=*/true);
    if (!execFile) return 1;

    // The entry point has to live inside the packaged folder: it is stored as a
    // path relative to the extracted tree, so a file from elsewhere would simply
    // not exist at runtime. The dialog opens in the right place but does not stop
    // the user navigating away.
    const fs::path sourceAbs = fs::weakly_canonical(*sourceDir, ec);
    const fs::path execAbs = fs::weakly_canonical(*execFile, ec);

    if (!paths::isContainedIn(sourceAbs.wstring(), execAbs.wstring())) {
        report(L"HNx",
               L"That program is outside the folder being packaged.\n\n"
               L"The entry point has to be inside it, because it is stored as a "
               L"path relative to the packaged files.\n\n"
               L"The GUI can combine several unrelated folders into one package "
               L"if that is what you need.",
               MB_ICONERROR);
        return 1;
    }

    fs::path relExec = fs::relative(execAbs, sourceAbs, ec);
    if (ec || relExec.empty()) {
        report(L"HNx", L"Could not work out the entry point path.", MB_ICONERROR);
        return 1;
    }

    // --- 3. where to save ---------------------------------------------------
    const std::wstring folderName = sourceAbs.filename().wstring();
    const std::wstring defaultName = folderName + L".exe";

    auto output = pickSaveFile(nullptr, L"Save the packaged application",
                               loadSetting(SETTING_LAST_SAVE_DIR), defaultName);
    if (!output) return 1;

    saveSetting(SETTING_LAST_SAVE_DIR, output->parent_path().wstring());

    // --- build --------------------------------------------------------------
    //
    // Version strings come from the chosen executable, so a package inherits the
    // application's own name and version rather than showing a folder name.
    const VersionStrings vs = readVersionStrings(execAbs);

    const std::string appId = sanitizeAppId(paths::narrow(folderName));

    std::string appName = paths::narrow(
        !vs.productName.empty()       ? vs.productName
        : !vs.fileDescription.empty() ? vs.fileDescription
                                      : folderName);
    std::string appVersion =
        vs.fileVersion.empty() ? std::string("1.0") : paths::narrow(vs.fileVersion);

    std::string execRel = paths::narrow(relExec.wstring());
    for (auto& c : execRel)
        if (c == '/') c = '\\';

    std::string metaJson;
    metaJson += "{\n";
    metaJson += "  \"schemaVersion\": 1,\n";
    metaJson += "  \"appId\": \"" + jsonEscapeLocal(appId) + "\",\n";
    metaJson += "  \"appName\": \"" + jsonEscapeLocal(appName) + "\",\n";
    metaJson += "  \"appVersion\": \"" + jsonEscapeLocal(appVersion) + "\",\n";
    metaJson += "  \"targetDir\": \"%LOCALAPPDATA%\\\\HNx\\\\%APPID%\\\\%VERSION%\",\n";
    metaJson += "  \"exec\": \"" + jsonEscapeLocal(execRel) + "\",\n";
    metaJson += "  \"workDir\": \"%TARGET%\",\n";
    metaJson += "  \"argPassthrough\": true,\n";
    metaJson += "  \"waitMode\": \"none\",\n";
    metaJson += "  \"verify\": \"normal\",\n";
    metaJson += "  \"ui\": \"progress\",\n";
    metaJson += "  \"allowStubFlags\": true\n";
    metaJson += "}\n";

    BuildOptions opt;
    opt.sources.push_back({sourceAbs, {}});
    opt.output = *output;
    opt.inlineConfig = metaJson;
    opt.reportInDialogs = true;

    const int rc = buildPackage(std::move(opt));

    if (rc == 0) {
        std::wstring msg = L"Packaged successfully.\n\n";
        msg += output->wstring();
        report(L"HNx", msg, MB_ICONINFORMATION);
    }
    // A failure has already been reported by buildPackage in dialog mode.
    return rc;
}

}  // namespace hnx::pack
