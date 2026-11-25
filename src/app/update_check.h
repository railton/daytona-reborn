// "A new version is out": the PC home asks the project's GitHub releases,
// at most once a day, whether a release newer than this build exists, and
// says so with what to do: update the copy of the source (git pull, or a
// new download of it) and build the game again. Players never get built
// games (rules.md, 6 and 22), so there is nothing to download but source.
//
// The request is the system's curl (Linux, macOS, and Windows 10/11 ship
// it) in a background thread: no TLS library in the game. It sends nothing
// about the player: a generic User-Agent and the public API's URL. Without
// curl, or offline, it stays silent (a log line).
//
// This header is the pure part (versions, the answer's two fields, the
// daily limit), tested in tests/test_update_check.cpp; UpdateFetch
// (app/update_fetch.h) runs curl.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace app::update {

// MAJOR.MINOR.PATCH, as VERSION and the release tags ("v0.18.0"). A
// leading v, missing minor or patch numbers (0), and a pre-release
// ("-beta.1", lower than the release) or build ("+abc", ignored) suffix are
// read; anything else is not a version.
struct Version {
    long major = 0, minor = 0, patch = 0;
    bool prerelease = false;
};
std::optional<Version> parse_version(std::string_view text);
// <0, 0, >0 as a is older than, the same as, or newer than b.
int compare(const Version &a, const Version &b);
// Whether `latest` is a version strictly newer than `running`; false when
// either is not a version.
bool is_newer(std::string_view latest, std::string_view running);
// Whether two texts are the same version ("v0.19.0" and "0.19"); false when
// either is not a version.
bool same_version(std::string_view a, std::string_view b);
// The tag as shown: without its leading v ("v0.19.0" -> "0.19.0").
std::string display_version(std::string_view tag);

// What the release answer says: its tag and its page.
struct Release {
    std::string tag; // "v0.19.0"
    std::string url; // "https://github.com/<owner>/<repo>/releases/tag/v0.19.0"; "" when missing or not GitHub's
};
// tag_name and html_url of the answer's top-level object (GitHub's
// /releases/latest JSON). Nested objects (the author's own html_url) and
// text inside other strings are skipped. Empty when the text is not such
// an object or its tag is not a version.
std::optional<Release> parse_release(std::string_view json);

// "owner/repo": letters, digits, '.', '-', '_', one '/'.
bool valid_repo(std::string_view repo);
// https://api.github.com/repos/<repo>/releases/latest
std::string api_url(std::string_view repo);
// The releases page, when the answer gave no usable page.
std::string releases_page(std::string_view repo);

// Whether a check is due: never checked, a day (86400 s) since the last,
// or the clock went back. Times are Unix seconds.
constexpr int64_t kInterval = 24 * 60 * 60;
bool due(int64_t now, int64_t last_checked);

// The notice to show, or nothing: the check is on, the latest version seen
// is newer than the running one, and the player has not dismissed it.
bool show_notice(bool enabled, std::string_view latest, std::string_view running, std::string_view dismissed);

} // namespace app::update
