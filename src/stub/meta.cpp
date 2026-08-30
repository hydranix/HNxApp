#include "meta.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <variant>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "paths.hpp"

namespace hnx {
namespace {

// ---------------------------------------------------------------------------
// Minimal recursive-descent JSON reader.
//
// Deliberately supports only what the metadata schema uses: objects, strings,
// integers, booleans, and a flat string->string object for env. No arrays of
// objects, no floats, no nesting beyond one level. Anything else is a parse
// error rather than silently accepted, which keeps the surface small enough to
// reason about and the code small enough to justify not linking a real library.
// ---------------------------------------------------------------------------

class JsonReader {
public:
    explicit JsonReader(std::string_view s) : s_(s) {}

    struct Value {
        enum class Kind { String, Int, Bool, Object, Null } kind = Kind::Null;
        std::string str;
        long long num = 0;
        bool boolean = false;
        std::vector<std::pair<std::string, std::string>> members;  // flat objects only
    };

    [[nodiscard]] bool parseObject(std::vector<std::pair<std::string, Value>>& out) {
        skipWs();
        if (!consume('{')) return false;
        skipWs();
        if (consume('}')) return true;

        for (;;) {
            skipWs();
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (!consume(':')) return false;

            Value v;
            if (!parseValue(v)) return false;
            out.emplace_back(std::move(key), std::move(v));

            skipWs();
            if (consume(',')) continue;
            if (consume('}')) return true;
            return false;
        }
    }

private:
    std::string_view s_;
    std::size_t i_ = 0;

    void skipWs() {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i_;
            } else {
                break;
            }
        }
    }

    bool consume(char c) {
        if (i_ < s_.size() && s_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }

    bool literal(std::string_view lit) {
        if (s_.compare(i_, lit.size(), lit) != 0) return false;
        i_ += lit.size();
        return true;
    }

    bool parseString(std::string& out) {
        if (!consume('"')) return false;
        out.clear();
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (i_ >= s_.size()) return false;
            const char e = s_[i_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    // \uXXXX, including surrogate pairs, re-encoded as UTF-8 so the
                    // rest of the pipeline only ever sees UTF-8.
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                            i_ += 2;
                            unsigned lo = 0;
                            if (!hex4(lo)) return false;
                            if (lo >= 0xDC00 && lo <= 0xDFFF)
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            else
                                return false;
                        } else {
                            return false;  // unpaired high surrogate
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return false;  // unpaired low surrogate
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    return false;
            }
        }
        return false;
    }

    bool hex4(unsigned& out) {
        if (i_ + 4 > s_.size()) return false;
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parseValue(Value& v) {
        skipWs();
        if (i_ >= s_.size()) return false;

        const char c = s_[i_];
        if (c == '"') {
            v.kind = Value::Kind::String;
            return parseString(v.str);
        }
        if (c == 't') {
            if (!literal("true")) return false;
            v.kind = Value::Kind::Bool;
            v.boolean = true;
            return true;
        }
        if (c == 'f') {
            if (!literal("false")) return false;
            v.kind = Value::Kind::Bool;
            v.boolean = false;
            return true;
        }
        if (c == 'n') {
            if (!literal("null")) return false;
            v.kind = Value::Kind::Null;
            return true;
        }
        if (c == '{') {
            v.kind = Value::Kind::Object;
            return parseFlatObject(v.members);
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            v.kind = Value::Kind::Int;
            return parseInt(v.num);
        }
        // Arrays and floats are not part of the schema. Rejecting rather than
        // skipping means a typo surfaces as an error instead of a silently
        // ignored setting.
        return false;
    }

    bool parseFlatObject(std::vector<std::pair<std::string, std::string>>& out) {
        if (!consume('{')) return false;
        skipWs();
        if (consume('}')) return true;
        for (;;) {
            skipWs();
            std::string k;
            if (!parseString(k)) return false;
            skipWs();
            if (!consume(':')) return false;
            skipWs();
            std::string val;
            if (!parseString(val)) return false;  // env values must be strings
            out.emplace_back(std::move(k), std::move(val));
            skipWs();
            if (consume(',')) continue;
            if (consume('}')) return true;
            return false;
        }
    }

    bool parseInt(long long& out) {
        const std::size_t start = i_;
        if (i_ < s_.size() && s_[i_] == '-') ++i_;
        const std::size_t digitsStart = i_;
        while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        if (i_ == digitsStart) return false;
        if (i_ < s_.size() && (s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E'))
            return false;  // no floats in the schema

        long long v = 0;
        const bool neg = s_[start] == '-';
        for (std::size_t k = digitsStart; k < i_; ++k) {
            const int d = s_[k] - '0';
            if (v > (INT64_MAX - d) / 10) return false;  // overflow
            v = v * 10 + d;
        }
        out = neg ? -v : v;
        return true;
    }
};

// ---------------------------------------------------------------------------

using JV = JsonReader::Value;

[[nodiscard]] std::optional<WaitMode> toWaitMode(std::string_view s) {
    if (s == "none") return WaitMode::None;
    if (s == "wait") return WaitMode::Wait;
    if (s == "wait-and-clean") return WaitMode::WaitAndClean;
    return std::nullopt;
}

[[nodiscard]] std::optional<VerifyMode> toVerifyMode(std::string_view s) {
    if (s == "fast") return VerifyMode::Fast;
    if (s == "normal") return VerifyMode::Normal;
    if (s == "paranoid") return VerifyMode::Paranoid;
    return std::nullopt;
}

[[nodiscard]] std::optional<UiMode> toUiMode(std::string_view s) {
    if (s == "silent") return UiMode::Silent;
    if (s == "progress") return UiMode::Progress;
    if (s == "console") return UiMode::Console;
    return std::nullopt;
}

[[nodiscard]] std::optional<int> toShowWindow(std::string_view s) {
    if (s == "hidden") return SW_HIDE;
    if (s == "normal") return SW_SHOWNORMAL;
    if (s == "minimized") return SW_SHOWMINNOACTIVE;
    if (s == "maximized") return SW_SHOWMAXIMIZED;
    return std::nullopt;
}

}  // namespace

std::expected<Metadata, Error> Metadata::parse(std::string_view json) {
    if (!paths::isValidUtf8(json))
        return std::unexpected(Error{Ec::MetaParseFailed, "resource is not valid UTF-8"});

    // A UTF-8 BOM is easy to introduce by editing the resource by hand, and it
    // would otherwise fail as an unexpected character at offset 0.
    if (json.starts_with("\xEF\xBB\xBF")) json.remove_prefix(3);

    std::vector<std::pair<std::string, JV>> root;
    JsonReader reader(json);
    if (!reader.parseObject(root))
        return std::unexpected(Error{Ec::MetaParseFailed, "malformed JSON"});

    Metadata m;
    bool sawExec = false;
    bool sawAppId = false;

    for (auto& [key, v] : root) {
        auto wantString = [&](std::wstring& dst) -> std::expected<void, Error> {
            if (v.kind != JV::Kind::String)
                return std::unexpected(Error{Ec::MetaFieldInvalid, key + " must be a string"});
            dst = paths::widen(v.str);
            return {};
        };
        auto wantBool = [&](bool& dst) -> std::expected<void, Error> {
            if (v.kind != JV::Kind::Bool)
                return std::unexpected(Error{Ec::MetaFieldInvalid, key + " must be a boolean"});
            dst = v.boolean;
            return {};
        };

        std::expected<void, Error> r{};

        if (key == "schemaVersion") {
            if (v.kind != JV::Kind::Int)
                return std::unexpected(Error{Ec::MetaFieldInvalid, "schemaVersion must be an integer"});
            m.schemaVersion = static_cast<int>(v.num);
        } else if (key == "appId") {
            r = wantString(m.appId);
            sawAppId = true;
        } else if (key == "appName") {
            r = wantString(m.appName);
        } else if (key == "appVersion") {
            r = wantString(m.appVersion);
        } else if (key == "targetDir") {
            r = wantString(m.targetDir);
        } else if (key == "exec") {
            r = wantString(m.exec);
            sawExec = true;
        } else if (key == "args") {
            r = wantString(m.args);
        } else if (key == "workDir") {
            r = wantString(m.workDir);
        } else if (key == "argPassthrough") {
            r = wantBool(m.argPassthrough);
        } else if (key == "requireAdmin") {
            r = wantBool(m.requireAdmin);
        } else if (key == "singleInstance") {
            r = wantBool(m.singleInstance);
        } else if (key == "allowStubFlags") {
            r = wantBool(m.allowStubFlags);
        } else if (key == "env") {
            if (v.kind != JV::Kind::Object)
                return std::unexpected(Error{Ec::MetaFieldInvalid, "env must be an object"});
            for (auto& [ek, ev] : v.members)
                m.env.push_back({paths::widen(ek), paths::widen(ev)});
        } else if (key == "waitMode") {
            if (v.kind != JV::Kind::String) return std::unexpected(Error{Ec::MetaFieldInvalid, "waitMode"});
            auto w = toWaitMode(v.str);
            if (!w) return std::unexpected(Error{Ec::MetaFieldInvalid, "waitMode: " + v.str});
            m.waitMode = *w;
        } else if (key == "verify") {
            if (v.kind != JV::Kind::String) return std::unexpected(Error{Ec::MetaFieldInvalid, "verify"});
            auto w = toVerifyMode(v.str);
            if (!w) return std::unexpected(Error{Ec::MetaFieldInvalid, "verify: " + v.str});
            m.verify = *w;
        } else if (key == "ui") {
            if (v.kind != JV::Kind::String) return std::unexpected(Error{Ec::MetaFieldInvalid, "ui"});
            auto w = toUiMode(v.str);
            if (!w) return std::unexpected(Error{Ec::MetaFieldInvalid, "ui: " + v.str});
            m.ui = *w;
        } else if (key == "showWindow") {
            if (v.kind != JV::Kind::String) return std::unexpected(Error{Ec::MetaFieldInvalid, "showWindow"});
            auto w = toShowWindow(v.str);
            if (!w) return std::unexpected(Error{Ec::MetaFieldInvalid, "showWindow: " + v.str});
            m.showWindow = *w;
        }
        // Unknown keys are ignored on purpose: a newer packer must be able to add
        // fields without breaking stubs already in the wild.

        if (!r) return std::unexpected(r.error());
    }

    if (!sawAppId || m.appId.empty())
        return std::unexpected(Error{Ec::MetaFieldMissing, "appId"});
    if (!sawExec || m.exec.empty())
        return std::unexpected(Error{Ec::MetaFieldMissing, "exec"});

    // appId becomes a directory name and part of the lock identity, so it must be
    // a safe single path component.
    if (auto v = paths::validateArchivePath(paths::narrow(m.appId)); !v)
        return std::unexpected(Error{Ec::MetaFieldInvalid, "appId is not a safe name"});
    if (m.appId.find(L'/') != std::wstring::npos)
        return std::unexpected(Error{Ec::MetaFieldInvalid, "appId must not contain a separator"});

    // exec is resolved inside the target directory, so it gets the same treatment
    // as any archive path: a metadata resource is as editable as the archive.
    {
        std::string execUtf8 = paths::narrow(m.exec);
        for (auto& ch : execUtf8)
            if (ch == '\\') ch = '/';
        if (auto v = paths::validateArchivePath(execUtf8); !v)
            return std::unexpected(Error{Ec::MetaFieldInvalid, "exec: " + v.error().context});
    }

    if (m.targetDir.empty())
        m.targetDir = LR"(%LOCALAPPDATA%\HNx\%APPID%\%VERSION%)";
    if (m.appVersion.empty()) m.appVersion = L"0";
    if (m.workDir.empty()) m.workDir = L"%TARGET%";

    return m;
}

std::expected<Metadata, Error> Metadata::loadFromResource() {
    HMODULE self = ::GetModuleHandleW(nullptr);

    HRSRC res = ::FindResourceW(self, META_RESOURCE_NAME, RT_RCDATA);
    if (!res)
        return std::unexpected(Error{Ec::MetaResourceMissing, "FindResourceW", ::GetLastError()});

    const DWORD size = ::SizeofResource(self, res);
    if (size == 0)
        return std::unexpected(Error{Ec::MetaResourceMissing, "empty resource"});

    HGLOBAL h = ::LoadResource(self, res);
    if (!h)
        return std::unexpected(Error{Ec::MetaResourceMissing, "LoadResource", ::GetLastError()});

    const void* p = ::LockResource(h);
    if (!p)
        return std::unexpected(Error{Ec::MetaResourceMissing, "LockResource"});

    // Resource memory belongs to the module image and is never freed, so this
    // view stays valid for the life of the process.
    return parse(std::string_view{static_cast<const char*>(p), size});
}

}  // namespace hnx
