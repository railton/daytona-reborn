#include "app/update_check.h"

#include <cstddef>

namespace app::update {

namespace {

bool digit(char c) { return c >= '0' && c <= '9'; }

// A JSON reader just big enough to walk one object's top level.
struct Json {
    std::string_view s;
    size_t i = 0;

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    static void utf8(std::string &out, uint32_t c) {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) out += char(0xc0 | (c >> 6)), out += char(0x80 | (c & 0x3f));
        else if (c < 0x10000) out += char(0xe0 | (c >> 12)), out += char(0x80 | ((c >> 6) & 0x3f)), out += char(0x80 | (c & 0x3f));
        else
            out += char(0xf0 | (c >> 18)), out += char(0x80 | ((c >> 12) & 0x3f)), out += char(0x80 | ((c >> 6) & 0x3f)),
                out += char(0x80 | (c & 0x3f));
    }
    bool hex4(uint32_t &v) {
        if (s.size() - i < 4) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            v <<= 4;
            if (digit(c)) v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    // A string at i (its opening quote), unescaped into out.
    bool string(std::string &out) {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        out.clear();
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) return false;
            switch (s[i++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t c1;
                if (!hex4(c1)) return false;
                if (c1 >= 0xd800 && c1 < 0xdc00 && s.substr(i, 2) == "\\u") { // a surrogate pair
                    i += 2;
                    uint32_t c2;
                    if (!hex4(c2) || c2 < 0xdc00 || c2 >= 0xe000) return false;
                    c1 = 0x10000 + ((c1 - 0xd800) << 10) + (c2 - 0xdc00);
                }
                utf8(out, c1);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    // Any value at i, skipped: strings (their brackets and quotes are text),
    // nested objects and arrays, numbers and literals.
    bool skip() {
        ws();
        if (i >= s.size()) return false;
        std::string scratch;
        if (s[i] == '"') return string(scratch);
        if (s[i] == '{' || s[i] == '[') {
            int depth = 0;
            while (i < s.size()) {
                const char c = s[i];
                if (c == '"') {
                    if (!string(scratch)) return false;
                    continue;
                }
                ++i;
                if (c == '{' || c == '[') ++depth;
                else if ((c == '}' || c == ']') && --depth == 0) return true;
            }
            return false;
        }
        const size_t from = i;
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\n' && s[i] != '\r' &&
               s[i] != '\t')
            ++i;
        return i > from;
    }
};

bool printable(std::string_view text) {
    for (const char c : text)
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f) return false;
    return true;
}

} // namespace

std::optional<Version> parse_version(std::string_view t) {
    if (!t.empty() && (t[0] == 'v' || t[0] == 'V')) t.remove_prefix(1);
    Version v;
    long *parts[] = {&v.major, &v.minor, &v.patch};
    size_t i = 0;
    for (int n = 0; n < 3; ++n) {
        if (n > 0) {
            if (i >= t.size() || t[i] != '.') break; // a missing minor or patch number: 0
            ++i;
        }
        const size_t from = i;
        long value = 0;
        while (i < t.size() && digit(t[i])) {
            if (i - from >= 9) return std::nullopt; // no number that large is a version
            value = value * 10 + (t[i++] - '0');
        }
        if (i == from) return std::nullopt;
        *parts[n] = value;
    }
    if (i < t.size()) {
        const size_t suffix = i;
        if (t[i] == '-') {
            if (i + 1 >= t.size()) return std::nullopt;
            v.prerelease = true;
            const size_t plus = t.find('+', i);
            i = plus == std::string_view::npos ? t.size() : plus;
        }
        if (i < t.size() && (t[i] != '+' || i + 1 >= t.size())) return std::nullopt;
        if (!printable(t.substr(suffix))) return std::nullopt;
    }
    return v;
}

int compare(const Version &a, const Version &b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.prerelease != b.prerelease) return a.prerelease ? -1 : 1;
    return 0;
}

bool is_newer(std::string_view latest, std::string_view running) {
    const auto a = parse_version(latest), b = parse_version(running);
    return a && b && compare(*a, *b) > 0;
}

bool same_version(std::string_view a, std::string_view b) {
    const auto x = parse_version(a), y = parse_version(b);
    return x && y && compare(*x, *y) == 0;
}

std::string display_version(std::string_view tag) {
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.remove_prefix(1);
    return std::string(tag);
}

std::optional<Release> parse_release(std::string_view json) {
    Json j{json};
    j.ws();
    if (j.i >= json.size() || json[j.i] != '{') return std::nullopt;
    ++j.i;
    Release r;
    std::string key, value;
    for (bool first = true;; first = false) {
        j.ws();
        if (j.i < json.size() && json[j.i] == '}' && first) break;
        if (!j.string(key)) return std::nullopt;
        j.ws();
        if (j.i >= json.size() || json[j.i] != ':') return std::nullopt;
        ++j.i;
        j.ws();
        if ((key == "tag_name" || key == "html_url") && j.i < json.size() && json[j.i] == '"') {
            if (!j.string(value)) return std::nullopt;
            (key == "tag_name" ? r.tag : r.url) = value;
        } else if (!j.skip()) {
            return std::nullopt;
        }
        j.ws();
        if (j.i >= json.size()) return std::nullopt;
        if (json[j.i] == '}') break;
        if (json[j.i] != ',') return std::nullopt;
        ++j.i;
    }
    if (r.tag.size() > 64 || !printable(r.tag) || !parse_version(r.tag)) return std::nullopt;
    // the page is opened in the browser: GitHub's own, nothing else
    if (r.url.rfind("https://github.com/", 0) != 0 || r.url.size() > 512 || !printable(r.url)) r.url.clear();
    return r;
}

bool valid_repo(std::string_view repo) {
    const size_t slash = repo.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= repo.size() || repo.size() > 200) return false;
    for (size_t k = 0; k < repo.size(); ++k) {
        const char c = repo[k];
        const bool ok = digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_' ||
                        (c == '/' && k == slash);
        if (!ok) return false;
    }
    return true;
}

std::string api_url(std::string_view repo) { return "https://api.github.com/repos/" + std::string(repo) + "/releases/latest"; }

std::string releases_page(std::string_view repo) { return "https://github.com/" + std::string(repo) + "/releases"; }

bool due(int64_t now, int64_t last_checked) {
    return last_checked <= 0 || now < last_checked || now - last_checked >= kInterval;
}

bool show_notice(bool enabled, std::string_view latest, std::string_view running, std::string_view dismissed) {
    return enabled && is_newer(latest, running) && !same_version(dismissed, latest);
}

} // namespace app::update
