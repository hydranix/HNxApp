#include "firewall.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// initguid.h must precede fwpmu.h so the well-known WFP layer and condition GUIDs
// are *defined* in this translation unit rather than left as unresolved externals.
// This is what lets the stub reference them without linking uuid.lib or fwpuclnt.lib.
#include <initguid.h>
#include <fwpmu.h>
#include <rpc.h>  // RPC_C_AUTHN_WINNT

#include "paths.hpp"

namespace hnx {
namespace {

// The five fwpuclnt.dll entry points this module uses, resolved at runtime.
//
// Linking the import library would add a load-time dependency the stub must not
// have: an image without fwpuclnt.dll (WinPE) would then fail to start at all,
// with nothing left able to report why. This is the same reasoning that keeps
// comctl32 out of the stub's static imports.
using FwpmEngineOpen0_t = DWORD(WINAPI*)(const wchar_t*, UINT32, void*,
                                         const FWPM_SESSION0*, HANDLE*);
using FwpmEngineClose0_t = DWORD(WINAPI*)(HANDLE);
using FwpmFilterAdd0_t = DWORD(WINAPI*)(HANDLE, const FWPM_FILTER0*, void*, UINT64*);
using FwpmGetAppIdFromFileName0_t = DWORD(WINAPI*)(const wchar_t*, FWP_BYTE_BLOB**);
using FwpmFreeMemory0_t = void(WINAPI*)(void**);

template <typename Fn>
[[nodiscard]] Fn resolve(HMODULE mod, const char* name) {
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(::GetProcAddress(mod, name)));
}

struct Wfp {
    HMODULE module = nullptr;
    FwpmEngineOpen0_t EngineOpen = nullptr;
    FwpmFilterAdd0_t FilterAdd = nullptr;
    FwpmGetAppIdFromFileName0_t GetAppId = nullptr;
    FwpmFreeMemory0_t FreeMemory = nullptr;

    [[nodiscard]] bool load() {
        module = ::LoadLibraryW(L"fwpuclnt.dll");
        if (!module) return false;
        EngineOpen = resolve<FwpmEngineOpen0_t>(module, "FwpmEngineOpen0");
        FilterAdd = resolve<FwpmFilterAdd0_t>(module, "FwpmFilterAdd0");
        GetAppId = resolve<FwpmGetAppIdFromFileName0_t>(module, "FwpmGetAppIdFromFileName0");
        FreeMemory = resolve<FwpmFreeMemory0_t>(module, "FwpmFreeMemory0");
        return EngineOpen && FilterAdd && GetAppId && FreeMemory;
    }
};

// Adds one BLOCK filter at `layer` matching the given application id.
[[nodiscard]] DWORD addBlockFilter(const Wfp& wfp, HANDLE engine, const GUID& layer,
                                   FWP_BYTE_BLOB* appId) {
    FWPM_FILTER_CONDITION0 cond{};
    cond.fieldKey = FWPM_CONDITION_ALE_APP_ID;
    cond.matchType = FWP_MATCH_EQUAL;
    cond.conditionValue.type = FWP_BYTE_BLOB_TYPE;
    cond.conditionValue.byteBlob = appId;

    FWPM_FILTER0 filter{};
    filter.displayData.name = const_cast<wchar_t*>(L"HNx temporary rule");
    filter.layerKey = layer;
    filter.numFilterConditions = 1;
    filter.filterCondition = &cond;
    filter.action.type = FWP_ACTION_BLOCK;
    filter.weight.type = FWP_EMPTY;  // let BFE assign a weight
    // subLayerKey left as the zero GUID: the filter goes in the built-in default
    // sublayer, which is where a simple app-id block belongs.

    UINT64 id = 0;
    return wfp.FilterAdd(engine, &filter, nullptr, &id);
}

}  // namespace

FirewallSession::~FirewallSession() { close(); }

void FirewallSession::close() noexcept {
    if (!engine_) return;
    // The module is still loaded (we never FreeLibrary it, matching the stub's
    // comctl32 handling), so resolve the close entry point from it and tear the
    // session -- and with it every filter -- down.
    if (HMODULE m = ::GetModuleHandleW(L"fwpuclnt.dll")) {
        if (auto EngineClose = resolve<FwpmEngineClose0_t>(m, "FwpmEngineClose0"))
            EngineClose(engine_);
    }
    engine_ = nullptr;
}

std::expected<void, Error> FirewallSession::apply(
    const FirewallConfig& cfg, const std::vector<std::wstring>& programPaths) {
    if (!cfg.active() || programPaths.empty()) return {};

    Wfp wfp;
    if (!wfp.load())
        return std::unexpected(Error{Ec::FirewallUnavailable, "fwpuclnt.dll", ::GetLastError()});

    // A dynamic session: the BFE deletes everything added through this handle when
    // it closes, including if the process dies, so the rules cannot outlive us.
    FWPM_SESSION0 session{};
    session.flags = FWPM_SESSION_FLAG_DYNAMIC;

    HANDLE engine = nullptr;
    DWORD rc = wfp.EngineOpen(nullptr, RPC_C_AUTHN_WINNT, nullptr, &session, &engine);
    if (rc != ERROR_SUCCESS)
        return std::unexpected(Error{Ec::FirewallApplyFailed, "FwpmEngineOpen0", rc});
    engine_ = engine;  // owned now; close() removes every filter added below

    for (const auto& path : programPaths) {
        FWP_BYTE_BLOB* appId = nullptr;
        rc = wfp.GetAppId(path.c_str(), &appId);
        if (rc != ERROR_SUCCESS) {
            close();
            return std::unexpected(Error{Ec::FirewallApplyFailed,
                                         "FwpmGetAppIdFromFileName0: " + paths::narrow(path), rc});
        }

        DWORD added = ERROR_SUCCESS;
        if (cfg.blockOutbound) {
            added = addBlockFilter(wfp, engine, FWPM_LAYER_ALE_AUTH_CONNECT_V4, appId);
            if (added == ERROR_SUCCESS)
                added = addBlockFilter(wfp, engine, FWPM_LAYER_ALE_AUTH_CONNECT_V6, appId);
        }
        if (added == ERROR_SUCCESS && cfg.blockInbound) {
            added = addBlockFilter(wfp, engine, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, appId);
            if (added == ERROR_SUCCESS)
                added = addBlockFilter(wfp, engine, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, appId);
        }

        wfp.FreeMemory(reinterpret_cast<void**>(&appId));

        if (added != ERROR_SUCCESS) {
            close();
            return std::unexpected(Error{Ec::FirewallApplyFailed, "FwpmFilterAdd0", added});
        }
    }

    return {};
}

}  // namespace hnx
