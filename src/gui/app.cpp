#include "app.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <imgui.h>

#include "dialogs.hpp"
#include "paths.hpp"

namespace hnx::gui {
namespace {


void setText(char* dst, std::size_t cap, std::string_view src) {
    const std::size_t n = std::min(src.size(), cap - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

const char* const kWaitValues[] = {"none", "wait", "wait-and-clean"};

const char* const kVerifyValues[] = {"fast", "normal", "paranoid"};

const char* const kUiValues[] = {"silent", "progress", "console"};

const char* const kShowValues[] = {"hidden", "normal", "minimized", "maximized"};

std::string jsonEscape(std::string_view s) {
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

}  // namespace

void applyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.ChildRounding = 4.0f;
    s.FrameRounding = 4.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f;
    s.PopupRounding = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.WindowPadding = ImVec2(12, 12);
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 6);
    s.CellPadding = ImVec2(6, 4);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.09f, 0.11f, 1.00f);
    c[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
    c[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.17f, 0.21f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.22f, 0.28f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.13f, 0.13f, 0.17f, 1.00f);
    c[ImGuiCol_Button] = ImVec4(0.22f, 0.34f, 0.55f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.43f, 0.68f, 1.00f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.18f, 0.28f, 0.46f, 1.00f);
    c[ImGuiCol_Header] = ImVec4(0.22f, 0.34f, 0.55f, 0.55f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.43f, 0.68f, 0.70f);
    c[ImGuiCol_Tab] = ImVec4(0.15f, 0.15f, 0.19f, 1.00f);
    c[ImGuiCol_TabSelected] = ImVec4(0.22f, 0.34f, 0.55f, 1.00f);
    c[ImGuiCol_CheckMark] = ImVec4(0.45f, 0.70f, 1.00f, 1.00f);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.30f, 0.60f, 0.95f, 1.00f);
}

// ---------------------------------------------------------------------------

App::App() {
    // Sane defaults, present before the user touches anything.
    setText(meta_.targetDir, sizeof(meta_.targetDir),
            R"(%LOCALAPPDATA%\HNx\%APPID%\%VERSION%)");
    setText(meta_.workDir, sizeof(meta_.workDir), "%TARGET%");
    setText(meta_.appVersion, sizeof(meta_.appVersion), "1.0");
}

App::~App() { shutdown(); }

void App::shutdown() {
    if (worker_.joinable()) {
        cancel_.store(true);
        worker_.join();
    }
}

bool confirmClose(void* hwnd) {
    return ::MessageBoxW(static_cast<HWND>(hwnd),
                         L"A package is still being built.\n\nClose anyway and abandon it?",
                         L"HNx Packer", MB_YESNO | MB_ICONWARNING) == IDYES;
}

void App::pushLog(std::string line) {
    std::lock_guard lock(logMutex_);
    log_.push_back(std::move(line));
    logScrollPending_ = true;
}

// ---------------------------------------------------------------------------
// Sources and scanning
// ---------------------------------------------------------------------------

void App::addFolder() {
    ComScope com;
    auto p = pickFolder(hwnd_, L"Add a folder to the package",
                        loadSetting(SETTING_LAST_SOURCE_DIR));
    if (!p) return;

    saveSetting(SETTING_LAST_SOURCE_DIR, p->parent_path().wstring());
    sources_.push_back({*p, suggestArchivePrefix(*p, sources_)});
    scanPending_ = true;
}

void App::addFiles() {
    ComScope com;
    auto files = pickAnyFiles(hwnd_, L"Add files to the package",
                              loadSetting(SETTING_LAST_SOURCE_DIR));
    if (files.empty()) return;

    saveSetting(SETTING_LAST_SOURCE_DIR, files.front().parent_path().wstring());
    for (const auto& f : files)
        sources_.push_back({f, suggestArchivePrefix(f, sources_)});
    scanPending_ = true;
}

void App::removeSource(std::size_t index) {
    if (index >= sources_.size()) return;
    // Removes it from the package only. Nothing is touched on disk -- that
    // distinction is the whole reason the tree exists.
    sources_.erase(sources_.begin() + static_cast<std::ptrdiff_t>(index));
    scanPending_ = true;
}

void App::rescan() {
    scanPending_ = false;
    scanError_.clear();
    selected_ = SIZE_MAX;

    if (sources_.empty()) {
        entries_.clear();
        nodes_.clear();
        root_ = SIZE_MAX;
        return;
    }

    // Remember what was excluded so a rescan does not silently re-include files
    // the user deliberately left out.
    std::vector<std::string> previouslyExcluded;
    for (const auto& e : entries_)
        if (!e.included) previouslyExcluded.push_back(e.relPath);
    std::sort(previouslyExcluded.begin(), previouslyExcluded.end());

    auto scanned = walkSources(sources_, 0, false, {});
    if (!scanned) {
        scanError_ = scanned.error().message();
        entries_.clear();
        nodes_.clear();
        root_ = SIZE_MAX;
        return;
    }

    entries_ = std::move(*scanned);
    for (auto& e : entries_)
        e.included = !std::binary_search(previouslyExcluded.begin(),
                                         previouslyExcluded.end(), e.relPath);

    rebuildTree();

    // Offer an entry point if there is an obvious one and none is set yet.
    if (meta_.exec[0] == '\0') autoFillFromExec();
}

void App::rebuildTree() {
    nodes_.clear();
    root_ = SIZE_MAX;
    if (entries_.empty()) return;

    nodes_.reserve(entries_.size() * 2);

    TreeNode root;
    root.name = "(package root)";
    root.isDir = true;
    nodes_.push_back(std::move(root));
    root_ = 0;

    // Path -> node index, so each component is created once no matter how many
    // entries share it.
    std::vector<std::pair<std::string, std::size_t>> index;
    index.emplace_back("", root_);

    auto findNode = [&](const std::string& path) -> std::size_t {
        auto it = std::lower_bound(
            index.begin(), index.end(), path,
            [](const auto& a, const std::string& b) { return a.first < b; });
        if (it != index.end() && it->first == path) return it->second;
        return SIZE_MAX;
    };
    auto addIndex = [&](std::string path, std::size_t node) {
        auto it = std::lower_bound(
            index.begin(), index.end(), path,
            [](const auto& a, const std::string& b) { return a.first < b; });
        index.insert(it, {std::move(path), node});
    };

    // Entries arrive directories-first then sorted by path, so a parent is always
    // created before anything inside it.
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const SourceEntry& e = entries_[i];

        std::size_t parent = root_;
        std::string accumulated;

        // Create every missing intermediate component.
        std::size_t start = 0;
        while (true) {
            const std::size_t slash = e.relPath.find('/', start);
            const bool last = (slash == std::string::npos);
            const std::string component =
                last ? e.relPath.substr(start) : e.relPath.substr(start, slash - start);

            if (!accumulated.empty()) accumulated.push_back('/');
            accumulated += component;

            std::size_t node = findNode(accumulated);
            if (node == SIZE_MAX) {
                TreeNode n;
                n.name = component;
                n.path = accumulated;
                n.isDir = last ? e.isDir : true;
                n.parent = parent;
                nodes_.push_back(std::move(n));
                node = nodes_.size() - 1;
                nodes_[parent].children.push_back(node);
                addIndex(accumulated, node);
            }

            if (last) {
                nodes_[node].entryIndex = i;
                nodes_[node].isDir = e.isDir;
                break;
            }
            parent = node;
            start = slash + 1;
        }
    }

    // Directories before files, alphabetically within each group, which is what
    // a file browser does and therefore what the eye expects.
    for (auto& n : nodes_) {
        std::sort(n.children.begin(), n.children.end(), [&](std::size_t a, std::size_t b) {
            if (nodes_[a].isDir != nodes_[b].isDir) return nodes_[a].isDir;
            return nodes_[a].name < nodes_[b].name;
        });
    }

    refreshCounts(root_);
}

void App::refreshCounts(std::size_t node) {
    if (node >= nodes_.size()) return;
    TreeNode& n = nodes_[node];

    n.fileCount = 0;
    n.includedCount = 0;
    n.includedBytes = 0;

    if (!n.isDir && n.entryIndex != SIZE_MAX) {
        const SourceEntry& e = entries_[n.entryIndex];
        n.fileCount = 1;
        if (e.included) {
            n.includedCount = 1;
            n.includedBytes = e.size;
        }
    }

    for (std::size_t c : n.children) {
        refreshCounts(c);
        n.fileCount += nodes_[c].fileCount;
        n.includedCount += nodes_[c].includedCount;
        n.includedBytes += nodes_[c].includedBytes;
    }
}

void App::setSubtreeIncluded(std::size_t node, bool included) {
    if (node >= nodes_.size()) return;
    TreeNode& n = nodes_[node];
    if (n.entryIndex != SIZE_MAX) entries_[n.entryIndex].included = included;
    for (std::size_t c : n.children) setSubtreeIncluded(c, included);
}

void App::autoFillFromExec() {
    // Prefer an .exe at the shallowest level, which is nearly always the real
    // entry point rather than a helper buried in a subfolder.
    std::size_t best = SIZE_MAX;
    std::size_t bestDepth = SIZE_MAX;

    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const SourceEntry& e = entries_[i];
        if (e.isDir || !e.included) continue;
        if (e.relPath.size() < 4) continue;
        std::string ext = e.relPath.substr(e.relPath.size() - 4);
        for (auto& c : ext) c = static_cast<char>(::tolower(c));
        if (ext != ".exe") continue;

        const std::size_t depth =
            static_cast<std::size_t>(std::count(e.relPath.begin(), e.relPath.end(), '/'));
        if (depth < bestDepth) {
            bestDepth = depth;
            best = i;
        }
    }
    if (best == SIZE_MAX) return;

    std::string execPath = entries_[best].relPath;
    for (auto& c : execPath)
        if (c == '/') c = '\\';
    setText(meta_.exec, sizeof(meta_.exec), execPath);

    // Name, version and id follow from the executable, exactly as the
    // command-line interactive flow derives them.
    const fs::path host(paths::toDisplayForm(entries_[best].absPath));
    const VersionStrings vs = readVersionStrings(host);

    if (meta_.appName[0] == '\0') {
        std::string name = paths::narrow(!vs.productName.empty() ? vs.productName
                                         : !vs.fileDescription.empty() ? vs.fileDescription
                                                                       : std::wstring());
        if (name.empty() && !sources_.empty())
            name = paths::narrow(sources_.front().hostPath.filename().wstring());
        setText(meta_.appName, sizeof(meta_.appName), name);
    }
    if (!vs.fileVersion.empty())
        setText(meta_.appVersion, sizeof(meta_.appVersion), paths::narrow(vs.fileVersion));

    if (meta_.appId[0] == '\0' && !sources_.empty())
        setText(meta_.appId, sizeof(meta_.appId),
                sanitizeAppId(paths::narrow(sources_.front().hostPath.filename().wstring())));
}

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

std::string App::buildMetaJson() const {
    std::string s = "{\n";
    s += "  \"schemaVersion\": 1,\n";
    s += "  \"appId\": \"" + jsonEscape(meta_.appId) + "\",\n";
    s += "  \"appName\": \"" + jsonEscape(meta_.appName) + "\",\n";
    s += "  \"appVersion\": \"" + jsonEscape(meta_.appVersion) + "\",\n";
    s += "  \"targetDir\": \"" + jsonEscape(meta_.targetDir) + "\",\n";
    s += "  \"exec\": \"" + jsonEscape(meta_.exec) + "\",\n";
    s += "  \"args\": \"" + jsonEscape(meta_.args) + "\",\n";
    s += "  \"workDir\": \"" + jsonEscape(meta_.workDir) + "\",\n";

    s += "  \"env\": {";
    bool first = true;
    for (const auto& [k, v] : meta_.env) {
        if (k.empty()) continue;
        if (!first) s += ",";
        s += "\n    \"" + jsonEscape(k) + "\": \"" + jsonEscape(v) + "\"";
        first = false;
    }
    s += first ? "},\n" : "\n  },\n";

    s += std::string("  \"waitMode\": \"") + kWaitValues[meta_.waitMode] + "\",\n";
    s += std::string("  \"verify\": \"") + kVerifyValues[meta_.verify] + "\",\n";
    s += std::string("  \"ui\": \"") + kUiValues[meta_.uiMode] + "\",\n";
    s += std::string("  \"showWindow\": \"") + kShowValues[meta_.showWindow] + "\",\n";
    s += std::string("  \"argPassthrough\": ") + (meta_.argPassthrough ? "true" : "false") + ",\n";
    s += std::string("  \"requireAdmin\": ") + (meta_.requireAdmin ? "true" : "false") + ",\n";
    s += std::string("  \"singleInstance\": ") + (meta_.singleInstance ? "true" : "false") + ",\n";
    s += std::string("  \"allowStubFlags\": ") + (meta_.allowStubFlags ? "true" : "false") + "\n";
    s += "}\n";
    return s;
}

bool App::validate(std::string& problem) const {
    if (sources_.empty()) {
        problem = "Add a folder or some files first.";
        return false;
    }
    if (meta_.appId[0] == '\0') {
        problem = "Application ID is required: it names the cache folder.";
        return false;
    }
    if (meta_.exec[0] == '\0') {
        problem = "Choose which program runs when the package starts.";
        return false;
    }
    if (output_[0] == '\0') {
        problem = "Choose where to save the package.";
        return false;
    }

    // The entry point must be a file that is actually going into the package.
    std::string want = meta_.exec;
    for (auto& c : want)
        if (c == '\\') c = '/';
    const bool present = std::any_of(entries_.begin(), entries_.end(),
                                     [&](const SourceEntry& e) {
                                         return !e.isDir && e.included &&
                                                ::_stricmp(e.relPath.c_str(),
                                                           want.c_str()) == 0;
                                     });
    if (!present) {
        problem = "The entry point \"" + std::string(meta_.exec) +
                  "\" is not in the package. Check the path, and that it is still "
                  "ticked in the file tree.";
        return false;
    }
    return true;
}

void App::startBuild() {
    std::string problem;
    if (!validate(problem)) {
        pushLog("ERROR: " + problem);
        ::MessageBoxW(static_cast<HWND>(hwnd_), paths::widen(problem).c_str(),
                      L"HNx Packer", MB_OK | MB_ICONWARNING);
        return;
    }

    if (worker_.joinable()) worker_.join();

    {
        std::lock_guard lock(logMutex_);
        log_.clear();
    }
    cancel_.store(false);
    finished_.store(false);
    building_.store(true);
    progDone_.store(0);
    progTotal_.store(0);

    BuildOptions opt;
    opt.sources = sources_;
    for (const auto& e : entries_)
        if (!e.included) opt.excluded.push_back(e.relPath);

    opt.output = fs::path(output_);
    opt.inlineConfig = buildMetaJson();
    opt.codec = static_cast<Codec>(codec_);
    opt.level = level_;
    opt.blockSize = static_cast<u32>(blockSizeMb_) * 1024u * 1024u;
    opt.threads = static_cast<unsigned>(threads_);
    opt.noIcon = noIcon_;
    if (iconFrom_[0] != '\0') opt.iconSource = fs::path(iconFrom_);
    if (archChoice_ != 0) {
        opt.archExplicit = true;
        opt.arch = (archChoice_ == 1) ? StubArch::X64 : StubArch::X86;
    }

    BuildCallbacks cb;
    cb.log = [this](std::string_view line) { pushLog(std::string(line)); };
    cb.progress = [this](std::string_view phase, u64 done, u64 total) {
        progDone_.store(done);
        progTotal_.store(total);
        std::lock_guard lock(logMutex_);
        progPhase_.assign(phase);
    };
    // Polled from worker threads, so this has to be an atomic read and nothing
    // more; touching ImGui state here would be a data race against the UI thread.
    cb.cancelled = [this] { return cancel_.load(std::memory_order_relaxed); };

    worker_ = std::jthread([this, opt = std::move(opt), cb = std::move(cb)]() mutable {
        const int rc = buildPackage(std::move(opt), cb);
        result_.store(rc);
        building_.store(false);
        finished_.store(true);
    });
}

void App::requestCancel() {
    cancel_.store(true);
    pushLog("Cancelling...");
}

}  // namespace hnx::gui
