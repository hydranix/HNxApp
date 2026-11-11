// The launcher stubs, carried inside hnxpack as resources.
//
// This exists because the packer used to default to a hnxstub.exe sitting beside
// it. That works in a build tree and nowhere else: copy hnxpack somewhere on its
// own, or run it from a shortcut, and packaging fails at the very last step with
// a path the user never chose. Carrying the stubs internally means hnxpack is a
// single self-sufficient file.
#include "packer.hpp"

#include <fstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "paths.hpp"

namespace hnx::pack {
namespace {

[[nodiscard]] const wchar_t* resourceNameFor(StubArch arch) {
    switch (arch) {
        case StubArch::X64: return L"HNXSTUB_X64";
        case StubArch::X86: return L"HNXSTUB_X86";
    }
    return nullptr;
}

// Returns the raw resource bytes, or an empty span when that architecture was not
// built into this packer.
[[nodiscard]] std::span<const std::byte> findStub(StubArch arch) {
    const wchar_t* name = resourceNameFor(arch);
    if (!name) return {};

    HMODULE self = ::GetModuleHandleW(nullptr);

    HRSRC res = ::FindResourceW(self, name, RT_RCDATA);
    if (!res) return {};

    const DWORD size = ::SizeofResource(self, res);
    if (size == 0) return {};

    HGLOBAL h = ::LoadResource(self, res);
    if (!h) return {};

    const void* p = ::LockResource(h);
    if (!p) return {};

    // Resource memory belongs to the module image and is never freed, so this
    // view stays valid for the life of the process.
    return {static_cast<const std::byte*>(p), size};
}

}  // namespace

const char* stubArchName(StubArch arch) {
    switch (arch) {
        case StubArch::X64: return "x64";
        case StubArch::X86: return "x86";
    }
    return "?";
}

std::optional<StubArch> detectTargetArch(const std::filesystem::path& exe) {
    std::ifstream in(exe, std::ios::binary);
    if (!in) return std::nullopt;

    // Only the DOS header, the PE signature and the machine field are needed, so
    // this reads a few dozen bytes rather than mapping the file.
    IMAGE_DOS_HEADER dos{};
    in.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!in || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
        return std::nullopt;

    in.seekg(dos.e_lfanew, std::ios::beg);
    if (!in) return std::nullopt;

    DWORD signature = 0;
    in.read(reinterpret_cast<char*>(&signature), sizeof(signature));
    if (!in || signature != IMAGE_NT_SIGNATURE) return std::nullopt;

    IMAGE_FILE_HEADER fh{};
    in.read(reinterpret_cast<char*>(&fh), sizeof(fh));
    if (!in) return std::nullopt;

    switch (fh.Machine) {
        case IMAGE_FILE_MACHINE_I386:
            return StubArch::X86;

        case IMAGE_FILE_MACHINE_AMD64:
            return StubArch::X64;

        // ARM64 and ARM64EC have no stub of their own yet. Reporting nothing
        // rather than guessing lets the caller fall back to x64 with a warning:
        // an x64 stub does run on ARM64 Windows under emulation and can launch
        // the native application, so the package works, just less efficiently
        // than it would with a native stub.
        default:
            return std::nullopt;
    }
}

bool hasEmbeddedStub(StubArch arch) { return !findStub(arch).empty(); }

std::expected<void, Error> writeEmbeddedStub(StubArch arch,
                                             const std::filesystem::path& output) {
    const auto bytes = findStub(arch);
    if (bytes.empty())
        return std::unexpected(Error{
            Ec::StubMissing,
            std::string("this build of hnxpack has no embedded ") + stubArchName(arch) +
                " stub; pass --stub to supply one"});

    // A stub that does not at least look like a PE would produce a packaged file
    // that fails mysteriously at launch. Checking here turns a broken build of
    // hnxpack into an immediate, obvious error.
    if (bytes.size() < 2 || static_cast<unsigned char>(bytes[0]) != 'M' ||
        static_cast<unsigned char>(bytes[1]) != 'Z')
        return std::unexpected(
            Error{Ec::StubMissing, std::string("embedded ") + stubArchName(arch) +
                                       " stub is not a PE image"});

    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    if (!out)
        return std::unexpected(Error{Ec::OutputWriteFailed, paths::narrow(output.wstring())});

    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out)
        return std::unexpected(Error{Ec::OutputWriteFailed, paths::narrow(output.wstring())});

    out.close();
    if (!out)
        return std::unexpected(Error{Ec::OutputWriteFailed, paths::narrow(output.wstring())});

    return {};
}

}  // namespace hnx::pack
