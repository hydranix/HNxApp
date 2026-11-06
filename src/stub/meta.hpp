// Application metadata, carried as an RT_RCDATA/HNXMETA PE resource.
//
// JSON rather than a packed struct: the packer emits it trivially, it survives
// inspection and hand-patching in Resource Hacker, and unknown keys are ignorable
// so a newer packer can add fields without breaking an older stub.
//
// The parser is hand-written (see meta.cpp) instead of using nlohmann or glaze.
// The document is about 1 KB, and either library's headers would cost more
// binary size than the whole parser does -- which matters because this code
// ships inside every packaged application.
#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "error.hpp"

namespace hnx {

enum class WaitMode {
    None,          // launch and exit immediately
    Wait,          // wait for the child, propagate its exit code
    WaitAndClean,  // wait, then delete the target tree
};

enum class VerifyMode {
    Fast,      // trust the state file; stat only the exec target
    Normal,    // default: parallel size+mtime sweep, hash on mismatch
    Paranoid,  // hash everything, ignore the state file
};

enum class UiMode {
    Silent,    // no window ever; errors still surface in a dialog
    Progress,  // progress window, but only if extraction runs long
    Console,   // write progress to an attached console
};

struct EnvVar {
    std::wstring name;
    std::wstring value;
};

// Temporary Windows firewall rules applied around the launched application.
//
// The rules are added through a WFP dynamic session the stub holds open for the
// child's lifetime, so the OS removes them automatically when the stub exits --
// cleanly, on a crash, or if it is killed. That is what makes them temporary
// without any persistent cleanup to get wrong. Applying them needs an elevated
// process, so a package that uses them relaunches itself elevated; see main.cpp.
struct FirewallConfig {
    bool blockInbound = false;
    bool blockOutbound = false;

    // Extra executables inside the package to cover beyond the entry point -- an
    // updater or helper the application spawns. Paths are relative to the
    // extracted tree and validated like `exec`. The entry point is always
    // included when any blocking is on.
    std::vector<std::wstring> programs;

    // True when the config asks for anything that needs rules applied.
    [[nodiscard]] bool active() const { return blockInbound || blockOutbound; }
};

struct Metadata {
    int schemaVersion = 1;

    std::wstring appId;
    std::wstring appName;
    std::wstring appVersion;

    std::wstring targetDir;  // template, expanded via paths::expandTokens
    std::wstring exec;       // relative to the target directory
    std::wstring args;
    std::wstring workDir;

    std::vector<EnvVar> env;

    bool argPassthrough = true;
    bool requireAdmin = false;
    bool singleInstance = false;
    bool allowStubFlags = true;

    WaitMode waitMode = WaitMode::None;
    VerifyMode verify = VerifyMode::Normal;
    UiMode ui = UiMode::Progress;
    int showWindow = 1;  // SW_SHOWNORMAL

    FirewallConfig firewall;

    // Parses the UTF-8 JSON document in `json`.
    [[nodiscard]] static std::expected<Metadata, Error> parse(std::string_view json);

    // Loads the HNXMETA resource from the running module and parses it.
    [[nodiscard]] static std::expected<Metadata, Error> loadFromResource();
};

// Resource identity. RT_RCDATA keeps the blob visible as "RCDATA / HNXMETA" in
// standard resource editors rather than as an opaque custom type.
inline constexpr wchar_t META_RESOURCE_NAME[] = L"HNXMETA";

}  // namespace hnx
