// The packer interface: state, the file tree, and everything drawn each frame.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <filesystem>

#include "dialogs.hpp"
#include "packer.hpp"

namespace hnx::gui {

// The GUI is a front end for the packer, so its vocabulary is the packer's:
// SourceItem, SourceEntry, BuildOptions, StubArch and the rest. Qualifying every
// one of them would add noise without adding clarity.
using namespace hnx::pack;

namespace fs = std::filesystem;

void applyStyle();

// Asked before the window closes. A build in flight owns worker threads and a
// half-written output file, so closing underneath it leaves both behind.
bool confirmClose(void* hwnd);

// One node of the merged package tree.
//
// The tree is built from archive paths, not from the filesystem, because that is
// what the user is actually choosing: several unrelated sources merge into one
// namespace, and what matters is where a file lands in the package.
struct TreeNode {
    std::string name;                    // this component only
    std::string path;                    // full archive path
    bool isDir = false;
    std::size_t entryIndex = SIZE_MAX;   // into App::entries_, SIZE_MAX if none
    std::size_t parent = SIZE_MAX;
    std::vector<std::size_t> children;

    // Rolled up from descendants each time the selection changes, so a folder can
    // show a partial state rather than a misleading tick.
    std::size_t fileCount = 0;
    std::size_t includedCount = 0;
    u64 includedBytes = 0;
};

// Text fields are held as fixed buffers because ImGui's InputText writes into
// one directly. Sizes are generous; paths are the only field that can approach
// them and targetDir is capped well below what the extractor accepts.
struct MetaFields {
    char appId[128]{};
    char appName[256]{};
    char appVersion[64]{};
    char targetDir[1024]{};
    char exec[1024]{};
    char args[1024]{};
    char workDir[1024]{};

    int waitMode = 0;     // none | wait | wait-and-clean
    int verify = 1;       // fast | normal | paranoid
    int uiMode = 1;       // silent | progress | console
    int showWindow = 1;   // hidden | normal | minimized | maximized

    bool argPassthrough = true;
    bool requireAdmin = false;
    bool singleInstance = false;
    bool allowStubFlags = true;

    bool firewallBlockOutbound = false;
    bool firewallBlockInbound = false;
    char firewallPrograms[1024]{};  // ;-separated, relative to the package root

    std::vector<std::pair<std::string, std::string>> env;
};

class App {
public:
    App();
    ~App();

    void setWindow(void* hwnd) { hwnd_ = hwnd; }
    void draw();
    void shutdown();

    [[nodiscard]] bool isBuilding() const { return building_.load(); }

private:
    // --- panels ---
    void drawToolbar();
    void drawSources();
    void drawTree();
    void drawTreeNode(std::size_t index);
    void drawOptions();
    void drawApplicationTab();
    void drawLaunchTab();
    void drawPackagingTab();
    void drawLog();

    // --- actions ---
    void addFolder();
    void addFiles();
    void removeSource(std::size_t index);
    void rescan();
    void rebuildTree();
    void setSubtreeIncluded(std::size_t node, bool included);
    void refreshCounts(std::size_t node);
    void autoFillFromExec();
    void startBuild();
    void requestCancel();
    void pushLog(std::string line);

    [[nodiscard]] std::string buildMetaJson() const;
    [[nodiscard]] bool validate(std::string& problem) const;

    void* hwnd_ = nullptr;

    std::vector<SourceItem> sources_;
    std::vector<SourceEntry> entries_;
    std::vector<TreeNode> nodes_;
    std::size_t root_ = SIZE_MAX;

    bool scanPending_ = false;
    std::string scanError_;

    MetaFields meta_;

    // Build options
    int codec_ = 2;        // store | lz4 | zstd
    int level_ = 19;
    int blockSizeMb_ = 8;
    int threads_ = 0;      // 0 = auto
    int archChoice_ = 0;   // auto | x64 | x86
    bool noIcon_ = false;
    char iconFrom_[1024]{};
    char output_[1024]{};

    // Build state, shared with the worker thread.
    std::jthread worker_;
    std::atomic<bool> building_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> finished_{false};
    std::atomic<int> result_{0};
    std::atomic<unsigned long long> progDone_{0};
    std::atomic<unsigned long long> progTotal_{0};

    std::mutex logMutex_;
    std::vector<std::string> log_;
    std::string progPhase_;
    bool logScrollPending_ = false;

    // Selected node in the tree, for the details strip.
    std::size_t selected_ = SIZE_MAX;
    char treeFilter_[128]{};
};

}  // namespace hnx::gui
