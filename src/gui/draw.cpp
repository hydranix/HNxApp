// Everything drawn each frame.
//
// Split from app.cpp purely for size: the state machine and the layout are
// separate concerns and the file was getting long.
#include <algorithm>
#include <cstdio>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <imgui.h>
// For ImGuiItemFlags_MixedValue, which gives a folder checkbox its third state.
// It lives in the internal header rather than the public one, but it is the
// mechanism ImGui's own demo uses for tri-state, and there is no public
// equivalent: a folder that is half included has to look different from one that
// is empty.
#include <imgui_internal.h>

#include "app.hpp"
#include "dialogs.hpp"
#include "paths.hpp"

namespace hnx::gui {
namespace {
namespace fs = std::filesystem;


const char* const kWaitModes[] = {"Exit immediately", "Wait for the app to close",
                                  "Wait, then delete the unpacked files"};
const char* const kVerifyModes[] = {"Fast - trust the cache", "Normal - size and date",
                                    "Paranoid - hash everything"};
const char* const kUiModes[] = {"Silent", "Progress window if slow", "Console"};
const char* const kShowWindow[] = {"Hidden", "Normal", "Minimized", "Maximized"};
const char* const kCodecs[] = {"Store - no compression", "LZ4 - fast",
                               "Zstandard - best ratio"};
const char* const kArch[] = {"Match the entry point", "x64", "x86 (32-bit)"};

void hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.60f, 0.66f, 1.0f));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void sectionHeading(const char* text) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.75f, 1.00f, 1.0f));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();
}

// A labelled full-width input, so every field lines up without a table.
bool field(const char* label, char* buf, std::size_t cap, const char* tip = nullptr) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const std::string id = std::string("##") + label;
    const bool changed = ImGui::InputText(id.c_str(), buf, cap);
    if (tip) {
        hint(tip);
        ImGui::Spacing();
    }
    return changed;
}

bool combo(const char* label, int* value, const char* const* items, int count,
           const char* tip = nullptr) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const std::string id = std::string("##") + label;
    const bool changed = ImGui::Combo(id.c_str(), value, items, count);
    if (tip) {
        hint(tip);
        ImGui::Spacing();
    }
    return changed;
}

std::string sizeText(u64 bytes) {
    char buf[64];
    if (bytes >= 1024ull * 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.2f GB", double(bytes) / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.1f MB", double(bytes) / (1024.0 * 1024));
    else if (bytes >= 1024)
        std::snprintf(buf, sizeof(buf), "%.0f KB", double(bytes) / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
    return buf;
}

}  // namespace

void App::draw() {
    if (scanPending_) rescan();

    if (finished_.exchange(false)) {
        const int rc = result_.load();
        if (rc == 0) {
            pushLog("Done.");
            ::MessageBoxW(static_cast<HWND>(hwnd_),
                          (std::wstring(L"Packaged successfully.\n\n") +
                           paths::widen(output_))
                              .c_str(),
                          L"HNx Packer", MB_OK | MB_ICONINFORMATION);
        }
        // Failures already surfaced through the log and buildPackage's own dialog.
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    ImGui::Begin("HNxPacker", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    drawToolbar();
    ImGui::Separator();

    const float logHeight = ImGui::GetTextLineHeightWithSpacing() * 8.0f;
    const float bodyHeight = ImGui::GetContentRegionAvail().y - logHeight -
                             ImGui::GetStyle().ItemSpacing.y * 2;

    if (ImGui::BeginTable("body", 2,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                          ImVec2(0, bodyHeight))) {
        ImGui::TableSetupColumn("left", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("right", ImGuiTableColumnFlags_WidthStretch, 0.58f);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        drawSources();
        drawTree();

        ImGui::TableSetColumnIndex(1);
        drawOptions();

        ImGui::EndTable();
    }

    drawLog();
    ImGui::End();
}

void App::drawToolbar() {
    const bool busy = building_.load();

    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Add Folder...")) addFolder();
    ImGui::SameLine();
    if (ImGui::Button("Add Files...")) addFiles();
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    ImGui::BeginDisabled(busy || entries_.empty());
    if (ImGui::Button("Include All")) {
        for (auto& e : entries_) e.included = true;
        refreshCounts(root_);
    }
    ImGui::SameLine();
    if (ImGui::Button("Exclude All")) {
        for (auto& e : entries_) e.included = false;
        refreshCounts(root_);
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) scanPending_ = true;
    ImGui::EndDisabled();

    // Build and Cancel sit at the right edge, away from the destructive buttons.
    const float buttonWidth = 130.0f;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - buttonWidth * 2 - 8);

    if (busy) {
        if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0))) requestCancel();
    } else {
        ImGui::BeginDisabled(sources_.empty());
        if (ImGui::Button("Build Package", ImVec2(buttonWidth, 0))) startBuild();
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Save As...", ImVec2(buttonWidth, 0))) {
        ComScope com;
        std::wstring suggested = L"App.exe";
        if (meta_.appName[0] != '\0') suggested = paths::widen(meta_.appName) + L".exe";
        else if (!sources_.empty())
            suggested = sources_.front().hostPath.filename().wstring() + L".exe";

        auto out = pickSaveFile(hwnd_, L"Save the packaged application",
                                loadSetting(SETTING_LAST_SAVE_DIR), suggested);
        if (out) {
            std::snprintf(output_, sizeof(output_), "%s", out->string().c_str());
            saveSetting(SETTING_LAST_SAVE_DIR, out->parent_path().wstring());
        }
    }
    ImGui::EndDisabled();
}

void App::drawSources() {
    ImGui::TextUnformatted("Sources");
    hint("Folders and files that make up the package. Several unrelated sources "
         "can be combined; 'Target folder' is where each one lands inside the "
         "package.");
    ImGui::Spacing();

    const float h = ImGui::GetTextLineHeightWithSpacing() * 6.0f;
    if (ImGui::BeginChild("sources", ImVec2(0, h), ImGuiChildFlags_Borders)) {
        if (sources_.empty()) {
            ImGui::TextDisabled("Nothing added yet.");
        } else if (ImGui::BeginTable("srctable", 3,
                                     ImGuiTableFlags_SizingStretchProp |
                                         ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 0.55f);
            ImGui::TableSetupColumn("Target folder", ImGuiTableColumnFlags_WidthStretch, 0.33f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30.0f);
            ImGui::TableHeadersRow();

            std::size_t removeIndex = SIZE_MAX;
            for (std::size_t i = 0; i < sources_.size(); ++i) {
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(i));

                ImGui::TableSetColumnIndex(0);
                const std::string display = sources_[i].hostPath.string();
                ImGui::TextUnformatted(display.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", display.c_str());

                ImGui::TableSetColumnIndex(1);
                char prefix[256];
                std::snprintf(prefix, sizeof(prefix), "%s", sources_[i].archivePrefix.c_str());
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputTextWithHint("##prefix", "(package root)", prefix,
                                             sizeof(prefix))) {
                    sources_[i].archivePrefix = prefix;
                    scanPending_ = true;
                }

                ImGui::TableSetColumnIndex(2);
                if (ImGui::SmallButton("X")) removeIndex = i;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Remove from the package.\nThe files stay on disk.");

                ImGui::PopID();
            }
            ImGui::EndTable();

            if (removeIndex != SIZE_MAX) removeSource(removeIndex);
        }
    }
    ImGui::EndChild();
}

void App::drawTree() {
    ImGui::Spacing();

    if (!scanError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
        ImGui::TextWrapped("Scan failed: %s", scanError_.c_str());
        ImGui::PopStyleColor();
        return;
    }

    if (root_ != SIZE_MAX) {
        const TreeNode& r = nodes_[root_];
        ImGui::Text("Files  -  %zu of %zu included, %s", r.includedCount, r.fileCount,
                    sizeText(r.includedBytes).c_str());
    } else {
        ImGui::TextUnformatted("Files");
    }
    hint("Unticking leaves a file out of the package. Nothing is ever deleted from "
         "disk.");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", "Filter by name...", treeFilter_,
                             sizeof(treeFilter_));
    ImGui::Spacing();

    if (ImGui::BeginChild("tree", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        if (root_ == SIZE_MAX) {
            ImGui::TextDisabled("Add a folder or files to see them here.");
        } else {
            for (std::size_t c : nodes_[root_].children) drawTreeNode(c);
        }
    }
    ImGui::EndChild();
}

void App::drawTreeNode(std::size_t index) {
    if (index >= nodes_.size()) return;
    TreeNode& n = nodes_[index];

    // A filter hides non-matching files but keeps any folder that still contains
    // a match, so the path to a hit stays navigable.
    const bool filtering = treeFilter_[0] != '\0';
    if (filtering) {
        auto matches = [&](const TreeNode& node) {
            std::string hay = node.name;
            std::string needle = treeFilter_;
            for (auto& c : hay) c = static_cast<char>(::tolower(c));
            for (auto& c : needle) c = static_cast<char>(::tolower(c));
            return hay.find(needle) != std::string::npos;
        };
        if (!n.isDir && !matches(n)) return;
        if (n.isDir) {
            // Cheap containment test: recurse and let children decide. A folder
            // with nothing matching renders as an empty node, which is acceptable
            // and much simpler than precomputing match sets.
        }
    }

    ImGui::PushID(static_cast<int>(index));

    // Tri-state: a folder shows partial when only some of its contents are in.
    bool checked = n.isDir ? (n.includedCount == n.fileCount && n.fileCount > 0)
                           : (n.entryIndex != SIZE_MAX && entries_[n.entryIndex].included);
    const bool partial =
        n.isDir && n.includedCount > 0 && n.includedCount < n.fileCount;

    if (partial) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    if (ImGui::Checkbox("##inc", &checked)) {
        setSubtreeIncluded(index, checked);
        refreshCounts(root_);
    }
    if (partial) ImGui::PopItemFlag();

    ImGui::SameLine();

    if (n.isDir) {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
        if (filtering) flags |= ImGuiTreeNodeFlags_DefaultOpen;
        if (selected_ == index) flags |= ImGuiTreeNodeFlags_Selected;

        char label[512];
        std::snprintf(label, sizeof(label), "%s  (%zu/%zu)", n.name.c_str(),
                      n.includedCount, n.fileCount);

        const bool open = ImGui::TreeNodeEx(label, flags);
        if (ImGui::IsItemClicked()) selected_ = index;
        if (open) {
            for (std::size_t c : n.children) drawTreeNode(c);
            ImGui::TreePop();
        }
    } else {
        const SourceEntry* e =
            n.entryIndex != SIZE_MAX ? &entries_[n.entryIndex] : nullptr;

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf |
                                   ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                   ImGuiTreeNodeFlags_SpanAvailWidth;
        if (selected_ == index) flags |= ImGuiTreeNodeFlags_Selected;

        if (e && !e->included) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.45f, 0.5f, 1.0f));
        ImGui::TreeNodeEx(n.name.c_str(), flags);
        if (e && !e->included) ImGui::PopStyleColor();

        if (ImGui::IsItemClicked()) selected_ = index;

        if (e) {
            ImGui::SameLine();
            ImGui::TextDisabled("  %s", sizeText(e->size).c_str());

            if (ImGui::IsItemHovered() || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlapped))
                ImGui::SetTooltip("%s\nfrom %s", n.path.c_str(),
                                  paths::narrow(paths::toDisplayForm(e->absPath)).c_str());
        }

        // Right-click a file to make it the entry point, which is faster than
        // typing the path and cannot be typed wrong.
        if (ImGui::BeginPopupContextItem("filectx")) {
            if (ImGui::MenuItem("Set as entry point")) {
                std::string p = n.path;
                for (auto& c : p)
                    if (c == '/') c = '\\';
                std::snprintf(meta_.exec, sizeof(meta_.exec), "%s", p.c_str());
            }
            if (e && ImGui::MenuItem("Use icons and version from this")) {
                std::snprintf(iconFrom_, sizeof(iconFrom_), "%s",
                              paths::narrow(paths::toDisplayForm(e->absPath)).c_str());
            }
            ImGui::EndPopup();
        }
    }

    ImGui::PopID();
}

void App::drawOptions() {
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Application")) {
            if (ImGui::BeginChild("apptab")) drawApplicationTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Launch")) {
            if (ImGui::BeginChild("launchtab")) drawLaunchTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Packaging")) {
            if (ImGui::BeginChild("packtab")) drawPackagingTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void App::drawApplicationTab() {
    sectionHeading("Identity");

    field("Application ID", meta_.appId, sizeof(meta_.appId),
          "Names the cache folder and identifies the extraction lock. Lower case, "
          "no spaces or separators.");

    field("Display name", meta_.appName, sizeof(meta_.appName),
          "Shown on the progress window and in error messages.");

    field("Version", meta_.appVersion, sizeof(meta_.appVersion),
          "Part of the unpack path by default, so two versions can sit side by "
          "side.");

    sectionHeading("Entry point");

    field("Program to run", meta_.exec, sizeof(meta_.exec),
          "Relative to the package root. Right-click a file in the tree to set "
          "this without typing it.");

    field("Arguments", meta_.args, sizeof(meta_.args),
          "Passed to the program on every launch.");

    sectionHeading("Where it unpacks");

    field("Target folder", meta_.targetDir, sizeof(meta_.targetDir),
          "%LOCALAPPDATA%, %APPDIR%, %APPID%, %VERSION% and environment variables "
          "all expand. Including %VERSION% keeps versions separate; leaving it out "
          "lets an upgrade rewrite only the files that changed.");

    field("Working directory", meta_.workDir, sizeof(meta_.workDir),
          "Where the program starts. %TARGET% is the unpacked folder.");
}

void App::drawLaunchTab() {
    sectionHeading("Behaviour");

    combo("When the app starts", &meta_.waitMode, kWaitModes, 3,
          "Waiting keeps the launcher alive so it can return the app's exit code. "
          "Deleting afterwards leaves no trace but re-unpacks every time.");

    combo("Window state", &meta_.showWindow, kShowWindow, 4);

    combo("Launcher feedback", &meta_.uiMode, kUiModes, 3,
          "The progress window only appears if unpacking takes longer than about "
          "a third of a second, so a warm start shows nothing.");

    sectionHeading("Checking the unpacked files");

    combo("Verification", &meta_.verify, kVerifyModes, 3,
          "Normal compares size and date, and hashes only when they disagree. "
          "Fast trusts the cache and checks the entry point alone. Paranoid hashes "
          "everything on every launch.");

    sectionHeading("Options");

    ImGui::Checkbox("Pass command-line arguments through", &meta_.argPassthrough);
    hint("Arguments given to the package are forwarded to the application.");
    ImGui::Spacing();

    ImGui::Checkbox("Require administrator", &meta_.requireAdmin);
    hint("Launches through the shell with an elevation prompt.");
    ImGui::Spacing();

    ImGui::Checkbox("Only one instance at a time", &meta_.singleInstance);
    hint("A second launch exits instead of starting another copy.");
    ImGui::Spacing();

    ImGui::Checkbox("Allow --hnx-* options", &meta_.allowStubFlags);
    hint("Lets users pass --hnx-info, --hnx-verify and so on. Turn off if the "
         "application has its own options that would clash.");

    sectionHeading("Network");

    ImGui::Checkbox("Block outbound connections", &meta_.firewallBlockOutbound);
    ImGui::Checkbox("Block inbound connections", &meta_.firewallBlockInbound);
    hint("Temporary firewall rules, added just before launch and removed by Windows "
         "when the launcher exits. Needs administrator rights, so the package "
         "prompts for elevation and waits for the app to close.");

    if (meta_.firewallBlockOutbound || meta_.firewallBlockInbound) {
        ImGui::Spacing();
        field("Also block", meta_.firewallPrograms, sizeof(meta_.firewallPrograms),
              "Other programs in the package to cover, such as an updater, "
              "separated by ';'. The program to run is always included.");
    }

    sectionHeading("Environment variables");
    hint("Set for the application only. %TARGET% and the other tokens expand here "
         "too.");
    ImGui::Spacing();

    int removeAt = -1;
    for (std::size_t i = 0; i < meta_.env.size(); ++i) {
        ImGui::PushID(static_cast<int>(i) + 5000);

        char name[128];
        char value[512];
        std::snprintf(name, sizeof(name), "%s", meta_.env[i].first.c_str());
        std::snprintf(value, sizeof(value), "%s", meta_.env[i].second.c_str());

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.32f);
        if (ImGui::InputTextWithHint("##n", "NAME", name, sizeof(name)))
            meta_.env[i].first = name;

        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 34.0f);
        if (ImGui::InputTextWithHint("##v", "value", value, sizeof(value)))
            meta_.env[i].second = value;

        ImGui::SameLine();
        if (ImGui::SmallButton("X")) removeAt = static_cast<int>(i);

        ImGui::PopID();
    }
    if (removeAt >= 0) meta_.env.erase(meta_.env.begin() + removeAt);

    if (ImGui::Button("Add variable")) meta_.env.emplace_back("", "");
}

void App::drawPackagingTab() {
    sectionHeading("Output");

    ImGui::TextUnformatted("Save package as");
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::InputText("##out", output_, sizeof(output_));
    ImGui::SameLine();
    if (ImGui::Button("Browse", ImVec2(-FLT_MIN, 0))) {
        ComScope com;
        std::wstring suggested =
            meta_.appName[0] ? paths::widen(meta_.appName) + L".exe" : L"App.exe";
        auto out = pickSaveFile(hwnd_, L"Save the packaged application",
                                loadSetting(SETTING_LAST_SAVE_DIR), suggested);
        if (out) {
            std::snprintf(output_, sizeof(output_), "%s", out->string().c_str());
            saveSetting(SETTING_LAST_SAVE_DIR, out->parent_path().wstring());
        }
    }
    ImGui::Spacing();

    sectionHeading("Launcher");

    combo("Architecture", &archChoice_, kArch, 3,
          "A 32-bit application gets a 32-bit launcher so the package runs "
          "anywhere the application would. A 32-bit launcher cannot carry a "
          "payload near 2 GB, so choose x64 for very large packages.");

    ImGui::Checkbox("Do not copy icons or version info", &noIcon_);
    hint("By default the package takes its icon and its Properties details from "
         "the entry point, so it looks like the application it contains.");
    ImGui::Spacing();

    ImGui::TextUnformatted("Take icons from (optional)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##iconfrom", "defaults to the entry point", iconFrom_,
                             sizeof(iconFrom_));
    ImGui::Spacing();

    sectionHeading("Compression");

    combo("Method", &codec_, kCodecs, 3,
          "Blocks that do not compress are stored automatically, so already "
          "compressed content costs nothing either way.");

    const int maxLevel = (codec_ == 2) ? 22 : (codec_ == 1 ? 12 : 0);
    ImGui::BeginDisabled(codec_ == 0);
    ImGui::TextUnformatted("Level");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##level", &level_, 1, maxLevel > 0 ? maxLevel : 1);
    ImGui::EndDisabled();
    hint("Higher is smaller and slower to build. Unpacking speed barely changes, "
         "so a package built once and launched often is worth compressing hard.");
    ImGui::Spacing();

    ImGui::TextUnformatted("Block size (MB)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##block", &blockSizeMb_, 1, 64);
    hint("Files are grouped into independently compressed blocks. Smaller blocks "
         "let an update rewrite less; larger blocks compress better.");
    ImGui::Spacing();

    ImGui::TextUnformatted("Threads (0 = all cores)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##threads", &threads_, 0, 32);
}

void App::drawLog() {
    const bool busy = building_.load();
    const u64 total = progTotal_.load();
    const u64 done = progDone_.load();

    if (busy) {
        std::string phase;
        {
            std::lock_guard lock(logMutex_);
            phase = progPhase_;
        }
        const float frac = total > 0 ? float(double(done) / double(total)) : 0.0f;
        char overlay[128];
        if (total > 0)
            std::snprintf(overlay, sizeof(overlay), "%s  %llu / %llu", phase.c_str(),
                          (unsigned long long)done, (unsigned long long)total);
        else
            std::snprintf(overlay, sizeof(overlay), "Working...");

        ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0), overlay);
    }

    if (ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        std::lock_guard lock(logMutex_);
        for (const auto& line : log_) {
            const bool isError = line.rfind("ERROR", 0) == 0;
            if (isError) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
            ImGui::TextUnformatted(line.c_str());
            if (isError) ImGui::PopStyleColor();
        }
        if (logScrollPending_) {
            ImGui::SetScrollHereY(1.0f);
            logScrollPending_ = false;
        }
    }
    ImGui::EndChild();
}

}  // namespace hnx::gui
