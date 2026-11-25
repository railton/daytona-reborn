// app/update_check: version comparison, the two fields read from GitHub's
// release answer, the daily limit and when the notice shows.
#include "app/update_check.h"

#include <cstdio>
#include <string>

#define CHECK(test) do { if (!(test)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #test); return 1; } } while (0)

int main() {
    using namespace app::update;

    // versions: a leading v, missing numbers, suffixes
    {
        const auto v = parse_version("v0.18.0");
        CHECK(v && v->major == 0 && v->minor == 18 && v->patch == 0 && !v->prerelease);
        const auto short_one = parse_version("1.2");
        CHECK(short_one && short_one->major == 1 && short_one->minor == 2 && short_one->patch == 0);
        const auto major_only = parse_version("V3");
        CHECK(major_only && major_only->major == 3 && major_only->minor == 0 && major_only->patch == 0);
        CHECK(parse_version("1.2.3-beta.1") && parse_version("1.2.3-beta.1")->prerelease);
        CHECK(parse_version("1.2.3+build.7") && !parse_version("1.2.3+build.7")->prerelease);
        for (const char *bad : {"", "v", "abc", "1.", "1..2", ".1", "1.2.3.4", "1.2.x", "1.2.3-", "1.2.3+", "1.2.3 beta",
                                "-1.0.0", "1234567890.0.0", "v 1.0", "1.2.3-be ta"})
            CHECK(!parse_version(bad));
    }
    // strictly newer only
    CHECK(is_newer("v0.19.0", "0.18.0"));
    CHECK(is_newer("v0.18.1", "0.18.0"));
    CHECK(is_newer("1.0.0", "0.99.99"));
    CHECK(is_newer("v0.10.0", "0.9.0")); // numbers, not text
    CHECK(is_newer("0.19", "0.18.9"));
    CHECK(is_newer("v1", "0.18.0"));
    CHECK(!is_newer("v0.18.0", "0.18.0"));
    CHECK(!is_newer("0.18", "0.18.0"));
    CHECK(!is_newer("v0.17.5", "0.18.0"));
    CHECK(!is_newer("v0.9.0", "0.10.0"));
    CHECK(is_newer("0.19.0", "0.19.0-rc.1")); // a release over its pre-release
    CHECK(!is_newer("0.19.0-rc.1", "0.19.0"));
    CHECK(!is_newer("0.19.0-rc.1", "0.19.0-rc.2"));
    CHECK(!is_newer("0.18.0+abc", "0.18.0"));
    CHECK(!is_newer("nightly", "0.18.0"));
    CHECK(!is_newer("v0.19.0", "unknown"));
    CHECK(same_version("v0.19.0", "0.19"));
    CHECK(!same_version("", "0.19.0"));
    CHECK(display_version("v0.19.0") == "0.19.0" && display_version("0.19.0") == "0.19.0");

    // GitHub's answer: the shape /releases/latest gives (fields in its
    // order, the author's html_url nested first, the notes with quotes,
    // escapes and the field names in them)
    {
        const std::string json = R"({"url":"https://api.github.com/repos/someone/daytona-reborn/releases/1",)"
                                 R"("assets_url":"https://api.github.com/repos/someone/daytona-reborn/releases/1/assets",)"
                                 R"("html_url":"https:\/\/github.com\/someone\/daytona-reborn\/releases\/tag\/v0.19.0",)"
                                 R"("id":457699771,"author":{"login":"someone","id":1,)"
                                 R"("html_url":"https://github.com/someone","type":"User","site_admin":false},)"
                                 R"("node_id":"RE_x","tag_name":"v0.19.0","target_commitish":"main",)"
                                 R"("name":"Daytona Reborn 0.19.0","draft":false,"prerelease":false,)"
                                 R"("created_at":"2026-11-25T05:41:37Z","assets":[{"name":"x","size":3,"list":[1,[2,{}]]}],)"
                                 R"("tarball_url":null,"body":"Notes \"tag_name\":\"v9.9.9\" {not json} [x]\r\né😀 \\",)"
                                 R"("reactions":{"total_count":0}})";
        const auto r = parse_release(json);
        CHECK(r);
        CHECK(r->tag == "v0.19.0");
        CHECK(r->url == "https://github.com/someone/daytona-reborn/releases/tag/v0.19.0");
    }
    // pretty-printed, the fields the other way round
    {
        const auto r = parse_release("  {\n  \"tag_name\" : \"0.20.1\" ,\n  \"html_url\" : \"https://github.com/a/b/releases/tag/0.20.1\"\n}\n");
        CHECK(r && r->tag == "0.20.1" && r->url == "https://github.com/a/b/releases/tag/0.20.1");
    }
    // a page that is not GitHub's is dropped (the browser opens it); the tag stays
    {
        const auto r = parse_release(R"({"tag_name":"v1.0.0","html_url":"http://github.com/a/b"})");
        CHECK(r && r->tag == "v1.0.0" && r->url.empty());
        const auto r2 = parse_release(R"({"tag_name":"v1.0.0","html_url":"https://github.com.evil.example/x"})");
        CHECK(r2 && r2->url.empty());
        const auto r3 = parse_release(R"({"tag_name":"v1.0.0"})");
        CHECK(r3 && r3->url.empty());
    }
    // no release: GitHub's errors, a tag that is no version, broken text
    CHECK(!parse_release(R"({"message":"Not Found","documentation_url":"https://docs.github.com/rest","status":"404"})"));
    CHECK(!parse_release(R"({"tag_name":"latest-build","html_url":"https://github.com/a/b"})"));
    CHECK(!parse_release(R"({"tag_name":42})"));
    CHECK(!parse_release(R"({"author":{"tag_name":"v9.0.0"}})")); // nested: not the release's
    CHECK(!parse_release(R"({"tag_name":"v1.0.0")"));
    CHECK(!parse_release(R"({"tag_name":"v1.0.0",})"));
    CHECK(!parse_release(R"({"tag_name":"v1.0.0" "x":1})"));
    CHECK(!parse_release(R"({"tag_name":"v1.0.0)"));
    CHECK(!parse_release(R"(["tag_name","v1.0.0"])"));
    CHECK(!parse_release(""));
    CHECK(!parse_release("{}"));
    CHECK(!parse_release("<html>rate limited</html>"));
    CHECK(!parse_release(R"({"body":"unterminated)"));

    // the repository, the request
    CHECK(valid_repo("railton/daytona-reborn"));
    CHECK(valid_repo("A.b_c-d/e.F"));
    for (const char *bad : {"", "/", "a/", "/b", "a/b/c", "a b/c", "a/b?x=1", "a/b#", "https://github.com/a/b", "a\\b"})
        CHECK(!valid_repo(bad));
    CHECK(api_url("railton/daytona-reborn") == "https://api.github.com/repos/railton/daytona-reborn/releases/latest");
    CHECK(releases_page("a/b") == "https://github.com/a/b/releases");

    // once a day
    const int64_t day = kInterval, t = 1764000000;
    CHECK(due(t, 0));
    CHECK(!due(t, t));
    CHECK(!due(t + day - 1, t));
    CHECK(due(t + day, t));
    CHECK(due(t - 10, t)); // the clock went back

    // the notice: on, newer, not dismissed for that version
    CHECK(show_notice(true, "v0.19.0", "0.18.0", ""));
    CHECK(!show_notice(false, "v0.19.0", "0.18.0", ""));
    CHECK(!show_notice(true, "v0.18.0", "0.18.0", ""));
    CHECK(!show_notice(true, "", "0.18.0", ""));
    CHECK(!show_notice(true, "v0.19.0", "0.18.0", "v0.19.0"));
    CHECK(!show_notice(true, "v0.19.0", "0.18.0", "0.19"));
    CHECK(show_notice(true, "v0.20.0", "0.18.0", "v0.19.0")); // dismissed an older one: the newer shows

    std::puts("update_check: ok");
    return 0;
}
