// Temporary per-application firewall rules, applied through a WFP dynamic session.
//
// A dynamic session is the whole point: every filter added under it is removed by
// the Base Filtering Engine when the session handle closes, which happens when
// this process exits -- cleanly, on a crash, or on taskkill. That is what makes
// the rules temporary with no persistent-cleanup bookkeeping to get wrong, the
// same property the extraction lock gets from the OS closing a file handle.
//
// fwpuclnt.dll is loaded at runtime rather than linked, so the stub still loads on
// an image that does not ship it (WinPE), where firewall rules are skipped anyway.
#pragma once

#include <expected>
#include <string>
#include <vector>

#include "error.hpp"
#include "meta.hpp"

namespace hnx {

class FirewallSession {
public:
    FirewallSession() = default;
    ~FirewallSession();

    FirewallSession(const FirewallSession&) = delete;
    FirewallSession& operator=(const FirewallSession&) = delete;

    // Opens a dynamic WFP session and adds block filters per `cfg` for every path
    // in `programPaths` (absolute, display form; the entry point is expected to be
    // the first). Requires an elevated process: FwpmEngineOpen0 returns access
    // denied otherwise.
    //
    // On any failure the session is closed before returning, so a partial set of
    // filters is never left applied. A no-op when `cfg` asks for no blocking.
    [[nodiscard]] std::expected<void, Error> apply(
        const FirewallConfig& cfg, const std::vector<std::wstring>& programPaths);

    // Closes the WFP engine handle, which removes every filter added through it.
    // Called by the destructor; safe to call when nothing was opened.
    void close() noexcept;

private:
    void* engine_ = nullptr;  // HANDLE from FwpmEngineOpen0
};

}  // namespace hnx
