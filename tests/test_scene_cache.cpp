// Frame-local command identity must survive repeated ROM mesh occurrences.
#include "runtime/scene_cache.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static float bits_float(uint32_t bits) {
    static_assert(sizeof(float) == sizeof bits, "32-bit float required");
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

// Fill every GeoVertex byte, then assign the components the cache reads. This
// keeps the fixture valid when unions are enabled by M2_FAST_GEO.
static rt::GeoVertex geo_vertex(float x, float y, float z) {
    rt::GeoVertex v;
    std::memset(&v, 0, sizeof v);
    v.ox = x;
    v.oy = y;
    v.oz = z;
    v.p[0] = 0.0f;
    v.p[1] = 0.0f;
    v.p[2] = 0.0f;
    return v;
}

rt::GeoPoly triangle(uint32_t command, float tx = 0) {
    rt::GeoPoly p{};
    p.object_command = command;
    p.object_oba = 0x00800001;
    p.object_obc = 4;
    p.object_mode = 1;
    p.num_vertices = 3;
    p.v[0] = geo_vertex(1, 2, 3);
    p.v[1] = geo_vertex(4, 5, 6);
    p.v[2] = geo_vertex(7, 8, 9);
    p.object_matrix[0] = p.object_matrix[4] = p.object_matrix[8] = 1;
    p.object_matrix[9] = tx;
    return p;
}

static void fill_source(rt::GeoPoly &p) {
    p.source_num_vertices = 3;
    for (unsigned i = 0; i < 3; ++i) {
        p.source_v[i][0] = p.v[i].ox;
        p.source_v[i][1] = p.v[i].oy;
        p.source_v[i][2] = p.v[i].oz;
        p.source_v[i][3] = p.v[i].p[1];
        p.source_v[i][4] = p.v[i].p[2];
    }
}

static void set_x(rt::GeoPoly &p, float x, bool source) {
    if (source) p.source_v[0][0] = x;
    else p.v[0].ox = x;
}

static int zero_case(bool source) {
    rt::SceneCache cache;
    auto plus = triangle(101), minus = triangle(102);
    if (source) { fill_source(plus); fill_source(minus); }
    set_x(plus, bits_float(0x00000000u), source);
    set_x(minus, bits_float(0x80000000u), source);
    cache.frame({plus, minus}, 1, nullptr);
    CHECK(cache.last_instances() == 2);
    CHECK(cache.unique_meshes() == 2);
    const uint32_t plus_mesh = cache.instances()[0].mesh;
    const uint32_t minus_mesh = cache.instances()[1].mesh;
    CHECK(plus_mesh != minus_mesh);
    cache.frame({minus, plus}, 2, nullptr);
    CHECK(cache.last_instances() == 2);
    CHECK(cache.unique_meshes() == 2);
    CHECK(cache.instances()[0].mesh == minus_mesh);
    CHECK(cache.instances()[1].mesh == plus_mesh);
    return 0;
}

static int nan_case(bool source) {
    rt::SceneCache cache;
    auto a = triangle(201), b = triangle(202), c = triangle(203);
    if (source) { fill_source(a); fill_source(b); fill_source(c); }
    const float q1 = bits_float(0x7fc00001u);
    const float q2 = bits_float(0x7fc00002u);
    set_x(a, q1, source);
    set_x(b, q1, source);
    set_x(c, q2, source);
    cache.frame({a, b, c}, 1, nullptr);
    CHECK(cache.last_instances() == 3);
    CHECK(cache.unique_meshes() == 2);
    const uint32_t q1_mesh = cache.instances()[0].mesh;
    const uint32_t q2_mesh = cache.instances()[2].mesh;
    CHECK(q1_mesh != q2_mesh);
    CHECK(cache.instances()[1].mesh == q1_mesh);
    cache.frame({c, b, a}, 2, nullptr);
    CHECK(cache.last_instances() == 3);
    CHECK(cache.unique_meshes() == 2);
    CHECK(cache.instances()[0].mesh == q2_mesh);
    CHECK(cache.instances()[1].mesh == q1_mesh);
    CHECK(cache.instances()[2].mesh == q1_mesh);
    cache.frame({a, c, b}, 3, nullptr);
    CHECK(cache.last_instances() == 3);
    CHECK(cache.unique_meshes() == 2);
    CHECK(cache.instances()[0].mesh == q1_mesh);
    CHECK(cache.instances()[1].mesh == q2_mesh);
    CHECK(cache.instances()[2].mesh == q1_mesh);
    return 0;
}

// Compare occurrences against explicit per-frame expectations, never by mesh id:
// all these objects deliberately share the same ROM geometry.
int expect_instances(const rt::SceneCache &cache,
                     const std::vector<rt::GeoPoly> &expected, uint32_t mesh) {
    CHECK(cache.instances().size() == expected.size());
    CHECK(cache.last_instances() == expected.size());
    CHECK(cache.unique_meshes() == 1);
    for (size_t i = 0; i < expected.size(); ++i) {
        const auto &actual = cache.instances()[i];
        CHECK(actual.command == expected[i].object_command);
        CHECK(actual.mesh == mesh);
        for (unsigned k = 0; k < 12; ++k) {
            if (actual.matrix[k] != expected[i].object_matrix[k]) {
                std::fprintf(stderr, "occurrence %zu command %u matrix[%u]: expected %g, got %g\n",
                             i, actual.command, k, expected[i].object_matrix[k], actual.matrix[k]);
                return 1;
            }
        }
        CHECK(actual.focus_x == expected[i].object_focus_x);
        CHECK(actual.focus_y == expected[i].object_focus_y);
    }
    return 0;
}

int reordered_frame_sequence() {
    rt::SceneCache cache;
    auto a = triangle(11), b = triangle(29);
    const float pose_a[12] = {0,1,0, -1,0,0, 0,0,1, -12,3,80};
    const float pose_b[12] = {1,0,0, 0,0,-1, 0,1,0, 24,-6,120};
    std::copy(std::begin(pose_a), std::end(pose_a), a.object_matrix);
    std::copy(std::begin(pose_b), std::end(pose_b), b.object_matrix);
    a.object_focus_x = 320; a.object_focus_y = 240;
    b.object_focus_x = 400; b.object_focus_y = 300;
    cache.frame({a,b}, 100, nullptr);
    CHECK(cache.last_instances() == 2);
    const auto mesh = cache.instances()[0].mesh;
    CHECK(expect_instances(cache, {a,b}, mesh) == 0);

    // Both move and command numbers change; emission order is neither sorted
    // command order nor previous-frame index. All 12 coefficients must follow.
    a.object_command = 4; b.object_command = 90;
    a.object_matrix[9] = -8; a.object_matrix[10] = 5;
    b.object_matrix[9] = 21; b.object_matrix[11] = 116;
    a.object_focus_y = 260; b.object_focus_x = 420;
    cache.frame({b,a}, 101, nullptr);
    CHECK(expect_instances(cache, {b,a}, mesh) == 0);

    // A disappears. B reuses A's last command number: that number must not
    // resurrect A's pose or preserve a stale second occurrence.
    b.object_command = 4; b.object_matrix[9] = 18;
    cache.frame({b}, 102, nullptr);
    CHECK(expect_instances(cache, {b}, mesh) == 0);

    // A reappears first, under another number, at a new pose. B remains second.
    a.object_command = 77; b.object_command = 2;
    a.object_matrix[9] = -3; a.object_matrix[11] = 90;
    b.object_matrix[9] = 15;
    cache.frame({a,b}, 103, nullptr);
    CHECK(expect_instances(cache, {a,b}, mesh) == 0);

    // No visible occurrence does not delete the shared resident mesh.
    cache.frame({}, 104, nullptr);
    CHECK(expect_instances(cache, {}, mesh) == 0);
    b.object_command = 1; a.object_command = 2;
    b.object_matrix[10] = -2; a.object_matrix[10] = 8;
    cache.frame({b,a}, 105, nullptr);
    CHECK(expect_instances(cache, {b,a}, mesh) == 0);
    CHECK(cache.frames() == 6);
    // This proves frame-local association only, not persistent object identity.
    return 0;
}

int main() {
    rt::SceneCache cache;
    auto a = triangle(1), b = triangle(2, 10);
    cache.frame({a, b}, 1, nullptr);
    CHECK(cache.last_instances() == 2);
    CHECK(cache.unique_meshes() == 1);
    CHECK(cache.instances()[0].mesh == cache.instances()[1].mesh);
    CHECK(cache.instances()[0].matrix[9] == 0);
    CHECK(cache.instances()[1].matrix[9] == 10);
    const auto mesh = cache.instances()[0].mesh;

    // Coincident objects are still two emissions, not one matrix group.
    b.object_matrix[9] = 0;
    cache.frame({a, b}, 2, nullptr);
    CHECK(cache.last_instances() == 2);
    CHECK(cache.unique_meshes() == 1);
    CHECK(cache.instances()[0].mesh == mesh);

    // All linked polygons in each command belong to one mesh occurrence.
    auto c = a, d = b;
    c.v[0].ox = d.v[0].ox = 20;
    cache.frame({a, c, b, d}, 3, nullptr);
    CHECK(cache.last_instances() == 2);
    CHECK(cache.unique_meshes() == 2);
    CHECK(cache.instances()[0].mesh == cache.instances()[1].mesh);
    CHECK(cache.instances()[0].command == 1);
    CHECK(cache.instances()[1].command == 2);

    // Command numbers may change next frame; they must not version the mesh.
    a.object_command = 8; b.object_command = 3;
    cache.frame({b, a}, 4, nullptr);
    CHECK(cache.unique_meshes() == 2);
    CHECK(cache.instances()[0].command == 3);
    CHECK(cache.instances()[1].command == 8);
    CHECK(cache.instances()[0].mesh == mesh);

    // obc selects a link-prefix variant; the previous test wrongly asserted
    // reuse while changing both this field and UVs, with asserts off in Release.
    a.object_obc = 99;
    cache.frame({a}, 5, nullptr);
    CHECK(cache.unique_meshes() == 3);

    // Source vertices, rather than different clipped vertices, define the mesh.
    rt::SceneCache clipped;
    a = triangle(1); b = triangle(2);
    a.source_num_vertices = b.source_num_vertices = 3;
    for (unsigned i = 0; i < 3; ++i) {
        a.source_v[i][0] = b.source_v[i][0] = float(i);
        a.source_v[i][2] = b.source_v[i][2] = 5;
    }
    b.v[0].ox = 90;
    clipped.frame({a, b}, 1, nullptr);
    CHECK(clipped.last_instances() == 2);
    CHECK(clipped.unique_meshes() == 1);

    // ROM bit alone is insufficient: fast RAM wins if both address bits are set.
    auto direct = triangle(3), ram0 = triangle(4), ram1 = triangle(5);
    direct.object_direct = 1; ram0.object_oba = 1; ram1.object_oba = 0x01800001;
    cache.frame({direct, ram0, ram1}, 6, nullptr);
    CHECK(cache.last_instances() == 0);
    CHECK(cache.unique_meshes() == 3);

    // Unknown command provenance fails conservatively, never collapsing objects.
    a = triangle(0); b = triangle(0, 10);
    cache.frame({a, b}, 7, nullptr);
    CHECK(cache.last_instances() == 2);
    CHECK(cache.unique_meshes() == 3);
    cache.frame({}, 8, nullptr);
    CHECK(cache.last_instances() == 0);
    CHECK(cache.frames() == 8);

    // Flattened vertex streams can match while polygon topology differs.
    rt::SceneCache topology;
    a = triangle(1); b = triangle(1);
    b.v[0].ox = 20;
    auto joined = triangle(2);
    joined.num_vertices = 6;
    for (unsigned i = 0; i < 3; ++i) {
        joined.v[i] = a.v[i];
        joined.v[i + 3] = b.v[i];
    }
    topology.frame({a, b, joined}, 1, nullptr);
    CHECK(topology.last_instances() == 2);
    CHECK(topology.unique_meshes() == 2);
    CHECK(topology.instances()[0].mesh != topology.instances()[1].mesh);

    // The compact source signature drops upper obc bits. Equality must still
    // compare the full stored source key instead of trusting that signature.
    rt::SceneCache aliases;
    a = triangle(1); b = triangle(2);
    b.object_obc |= 0x00100000;
    aliases.frame({a, b}, 1, nullptr);
    CHECK(aliases.unique_meshes() == 2);
    const auto first = aliases.instances()[0].mesh;
    const auto second = aliases.instances()[1].mesh;
    aliases.frame({b, a}, 2, nullptr);
    CHECK(aliases.unique_meshes() == 2);
    CHECK(aliases.instances()[0].mesh == second);
    CHECK(aliases.instances()[1].mesh == first);

    // +0 and -0 differ by bit pattern and must remain distinct variants.
    // Check both fallback GeoVertex input and source_v input, then reorder.
    if (zero_case(false) || zero_case(true)) return 1;

    // Identical quiet-NaN payload bits reuse; different payload bits do not.
    // Compare mesh identities only, never the NaN values themselves.
    if (nan_case(false) || nan_case(true)) return 1;

    CHECK(reordered_frame_sequence() == 0);

    std::puts("scene cache: repeated, coincident, linked, reordered, clipped, RAM, empty, zero and NaN cases passed");
}
