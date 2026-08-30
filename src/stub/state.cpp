#include "state.hpp"

#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "paths.hpp"

namespace hnx {
namespace {

constexpr std::wstring_view kMetaDirName = L".hnx";
constexpr std::wstring_view kStateName = L"state.bin";
constexpr std::wstring_view kStateTmpName = L"state.tmp";

[[nodiscard]] std::wstring join(std::wstring_view dir, std::wstring_view leaf) {
    std::wstring out(dir);
    if (!out.empty() && out.back() != L'\\') out.push_back(L'\\');
    out.append(leaf);
    return out;
}

[[nodiscard]] bool readExact(HANDLE h, void* dst, DWORD bytes) {
    auto* p = static_cast<std::byte*>(dst);
    DWORD done = 0;
    while (done < bytes) {
        DWORD got = 0;
        if (!::ReadFile(h, p + done, bytes - done, &got, nullptr)) return false;
        if (got == 0) return false;  // premature EOF
        done += got;
    }
    return true;
}

[[nodiscard]] bool writeExact(HANDLE h, const void* src, DWORD bytes) {
    const auto* p = static_cast<const std::byte*>(src);
    DWORD done = 0;
    while (done < bytes) {
        DWORD put = 0;
        if (!::WriteFile(h, p + done, bytes - done, &put, nullptr)) return false;
        if (put == 0) return false;
        done += put;
    }
    return true;
}

}  // namespace

std::wstring State::metaDir(std::wstring_view targetDir) {
    return join(paths::toExtendedForm(targetDir), kMetaDirName);
}

State State::load(std::wstring_view targetDir) {
    State st;

    const std::wstring path = join(metaDir(targetDir), kStateName);

    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return st;  // first run: not an error

    StateHeader hdr{};
    if (!readExact(h, &hdr, sizeof(hdr))) {
        ::CloseHandle(h);
        return st;
    }

    if (std::memcmp(hdr.magic, STATE_MAGIC, sizeof(STATE_MAGIC)) != 0 ||
        hdr.version != STATE_VERSION || hdr.entryCount > MAX_ENTRY_COUNT) {
        ::CloseHandle(h);
        return st;
    }

    // Cross-check the declared count against the real file size before
    // allocating: this file lives in a user-writable directory, so it is not
    // more trustworthy than the archive.
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size)) {
        ::CloseHandle(h);
        return st;
    }
    const unsigned long long expected =
        sizeof(StateHeader) +
        static_cast<unsigned long long>(hdr.entryCount) * sizeof(StateRecord);
    if (static_cast<unsigned long long>(size.QuadPart) != expected) {
        ::CloseHandle(h);
        return st;
    }

    st.records_.resize(hdr.entryCount);
    if (hdr.entryCount > 0) {
        const DWORD bytes = static_cast<DWORD>(hdr.entryCount * sizeof(StateRecord));
        if (!readExact(h, st.records_.data(), bytes)) {
            ::CloseHandle(h);
            st.records_.clear();
            return st;
        }
    }
    ::CloseHandle(h);

    st.buildId_ = hdr.buildId;
    st.valid_ = true;
    return st;
}

bool State::matches(u64 buildId, std::size_t entryCount) const noexcept {
    return valid_ && buildId_ == buildId && records_.size() == entryCount;
}

std::expected<void, Error> State::store(std::wstring_view targetDir, u64 buildId,
                                        std::span<const StateRecord> records) {
    const std::wstring dir = metaDir(targetDir);

    if (!::CreateDirectoryW(dir.c_str(), nullptr)) {
        const DWORD e = ::GetLastError();
        if (e != ERROR_ALREADY_EXISTS)
            return std::unexpected(Error{Ec::CreateDirFailed, paths::narrow(dir), e});
    }

    // Hide the bookkeeping directory so it does not clutter a target the user may
    // browse. Failure is cosmetic, so it is not checked.
    ::SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_HIDDEN);

    const std::wstring tmp = join(dir, kStateTmpName);
    const std::wstring final = join(dir, kStateName);

    HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return std::unexpected(Error{Ec::CreateFileFailed, paths::narrow(tmp), ::GetLastError()});

    StateHeader hdr{};
    std::memcpy(hdr.magic, STATE_MAGIC, sizeof(STATE_MAGIC));
    hdr.version = STATE_VERSION;
    hdr.buildId = buildId;
    hdr.entryCount = static_cast<u32>(records.size());

    bool ok = writeExact(h, &hdr, sizeof(hdr));
    if (ok && !records.empty()) {
        ok = writeExact(h, records.data(),
                        static_cast<DWORD>(records.size() * sizeof(StateRecord)));
    }

    // Flush before the rename. Without this the rename can be durable while the
    // contents are not, which on an unclean shutdown yields a state file that
    // claims an extraction finished when its own records never reached the disk.
    if (ok) ok = ::FlushFileBuffers(h) != FALSE;

    const DWORD writeErr = ok ? 0 : ::GetLastError();
    ::CloseHandle(h);

    if (!ok) {
        ::DeleteFileW(tmp.c_str());
        return std::unexpected(Error{Ec::WriteFailed, paths::narrow(tmp), writeErr});
    }

    if (!::MoveFileExW(tmp.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD e = ::GetLastError();
        ::DeleteFileW(tmp.c_str());
        return std::unexpected(Error{Ec::WriteFailed, paths::narrow(final), e});
    }

    return {};
}

void State::invalidate(std::wstring_view targetDir) noexcept {
    const std::wstring path = join(metaDir(targetDir), kStateName);
    ::DeleteFileW(path.c_str());
}

}  // namespace hnx
