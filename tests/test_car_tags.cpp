// runtime/car_tags and app::game_picture: where an online race's name tags
// go. Synthetic polygons only (no ROM data).
#include "app/online_overlay.h"
#include "runtime/car_tags.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

#define CHECK(test) do { if (!(test)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #test); return 1; } } while (0)

namespace {
bool near(float a, float b, float tolerance = 0.01f) { return std::abs(a - b) <= tolerance; }

// A board whose car records hold the given car-number bytes (+0x94).
struct FakeBoard {
    std::array<uint8_t, rt::MobileIdentity::slot_count> numbers{};
    uint8_t read_byte(uint32_t address) const {
        const uint32_t i = address - rt::online::kCarRecords;
        if (i % rt::online::kCarRecordSize != rt::online::kCarNumberOffset) return 0xee;
        return numbers.at(i / rt::online::kCarRecordSize);
    }
};

uint32_t record(unsigned slot) { return rt::online::kCarRecords + slot * rt::online::kCarRecordSize; }

// A square on the screen: centre (x, y) in screen pixels with origin 0,
// half side h, every corner at camera depth z; sort key zsort.
rt::GeoPoly square(float x, float y, float h, float z, uint16_t zsort, uint32_t part = 0, uint32_t slot = 0) {
    rt::GeoPoly poly;
    poly.center[0] = 248;
    poly.center[1] = 192;
    poly.z = zsort;
    poly.window = 1;
    poly.texheader[0] = 2 << 13; // textured, opaque
    poly.mobile_key = {part, slot, 0};
    poly.num_vertices = 4;
    const float corners[4][2] = {{-h, -h}, {h, -h}, {h, h}, {-h, h}};
    for (int k = 0; k < 4; ++k) {
        // x = 248 + vx / z, y = 384 - 192 - vy / z (GpuRenderer::build)
        poly.v[k].x = (x + corners[k][0] - 248.f) * z;
        poly.v[k].y = (192.f - (y + corners[k][1])) * z;
        poly.v[k].p[0] = z;
    }
    return poly;
}
} // namespace

int main() {
    using namespace rt::online;
    FakeBoard board;
    board.numbers.fill(8);   // computer cars
    board.numbers[1] = 2;    // slot 1: CAR NUMBER 3
    board.numbers[4] = 0;    // slot 4: CAR NUMBER 1
    const auto car_of = [&](unsigned slot) { return linked_car(board, slot); };

    // linked_car: the byte + 1, computer cars and records out of range 0
    CHECK(linked_car(board, 1) == 3);
    CHECK(linked_car(board, 4) == 1);
    CHECK(linked_car(board, 0) == 0);
    CHECK(linked_car(board, rt::MobileIdentity::slot_count) == 0);

    // one linked car, its body and a wheel; a computer car; scenery
    std::vector<rt::GeoPoly> polys;
    polys.push_back(square(200, 150, 20, 40, 500, 0x8808, record(1)));
    polys.push_back(square(200, 175, 5, 40, 500, 0x882c, record(1)));
    polys.push_back(square(100, 100, 10, 60, 600, 0x8c00, record(2)));
    polys.push_back(square(300, 300, 50, 10, 100));
    auto tags = car_tags(polys, 0, 0, car_of);
    CHECK(tags.size() == 1);
    CHECK(tags[0].car == 3);
    CHECK(near(tags[0].x, 200) && near(tags[0].top, 130) && near(tags[0].bottom, 180) && near(tags[0].width, 40));
    CHECK(near(tags[0].depth, 40));
    CHECK(tags[0].visible == 1.f);

    // the origin (crtc and widescreen margin) moves it
    tags = car_tags(polys, 93, 2, car_of);
    CHECK(tags.size() == 1 && near(tags[0].x, 293) && near(tags[0].top, 132));

    // a vertex behind the eye: that polygon is left out; none left, no tag
    polys[0].v[2].p[0] = -1.f;
    tags = car_tags(polys, 0, 0, car_of);
    CHECK(tags.size() == 1 && near(tags[0].top, 170)); // the wheel only
    polys[1].v[0].p[0] = 0.f;
    CHECK(car_tags(polys, 0, 0, car_of).empty());
    polys[0] = square(200, 150, 20, 40, 500, 0x8808, record(1));
    polys[1] = square(200, 175, 5, 40, 500, 0x882c, record(1));

    // cover: an opaque polygon drawn earlier (nearer: a lower sort key) over the car
    polys.push_back(square(200, 160, 40, 20, 300));
    tags = car_tags(polys, 0, 0, car_of);
    CHECK(tags.size() == 1 && tags[0].visible == 0.f);
    // as near and added later is drawn first too; added before is not
    polys.back().z = 500;
    CHECK(car_tags(polys, 0, 0, car_of)[0].visible == 0.f);
    polys ={square(200, 160, 40, 20, 500), square(200, 150, 20, 40, 500, 0x8808, record(1))};
    CHECK(car_tags(polys, 0, 0, car_of)[0].visible == 1.f);
    // farther (drawn later): no cover
    polys = {square(200, 150, 20, 40, 500, 0x8808, record(1)), square(200, 160, 40, 80, 700)};
    CHECK(car_tags(polys, 0, 0, car_of)[0].visible == 1.f);
    // nearer but translucent (renderer 1 draws nothing, 3 is a cut-out), in another window,
    // a HUD object, or the car's own part: no cover
    auto cover = square(200, 160, 40, 20, 300);
    for (int variant = 0; variant < 5; ++variant) {
        auto c = cover;
        if (variant == 0) c.texheader[0] = 1 << 13;
        if (variant == 1) c.texheader[0] = 3 << 13;
        if (variant == 2) c.window = 0;
        if (variant == 3) c.object_oba = 0x8d5da4;
        if (variant == 4) c.mobile_key = {0x882c, record(1), 0};
        polys = {square(200, 150, 20, 40, 500, 0x8808, record(1)), c};
        CHECK(car_tags(polys, 0, 0, car_of)[0].visible == 1.f);
    }
    // a solid polygon (renderer 0) does cover; another car does too
    cover.texheader[0] = 0;
    polys = {square(200, 150, 20, 40, 500, 0x8808, record(1)), cover};
    CHECK(car_tags(polys, 0, 0, car_of)[0].visible == 0.f);
    cover.mobile_key = {0x8808, record(4), 0};
    polys = {square(200, 150, 20, 40, 500, 0x8808, record(1)), cover};
    tags = car_tags(polys, 0, 0, car_of);
    CHECK(tags.size() == 2);
    for (const auto &tag : tags) CHECK(tag.car == 1 ? tag.visible == 1.f : tag.visible == 0.f);
    // half covered: of two sampled faces, one is seen
    polys = {square(150, 150, 10, 40, 500, 0x8808, record(1)), square(250, 150, 10, 40, 500, 0x8808, record(1)),
             square(150, 150, 30, 20, 300)};
    CHECK(near(car_tags(polys, 0, 0, car_of)[0].visible, 0.5f));
    // the samples are the largest faces
    polys = {square(150, 150, 2, 40, 500, 0x8808, record(1)), square(250, 150, 10, 40, 500, 0x8808, record(1)),
             square(150, 150, 30, 20, 300)};
    CHECK(car_tags(polys, 0, 0, car_of, 1)[0].visible == 1.f);

    // the picture in the window, as main.cpp blits it
    auto p = app::game_picture(1920, 1080, 496, 384, false); // bars at the sides
    CHECK(near(p.h, 1080) && near(p.w, 496 * 1080.f / 384) && near(p.x, (1920 - p.w) / 2) && near(p.y, 0));
    p = app::game_picture(1000, 1000, 496, 384, false); // bars above and below
    CHECK(near(p.w, 1000) && near(p.h, 384 * 1000.f / 496) && near(p.x, 0) && near(p.y, (1000 - p.h) / 2));
    p = app::game_picture(1920, 1080, 682, 384, false); // widescreen 16:9: the whole window
    CHECK(near(p.w, 682 * 1080.f / 384, 0.5f) && near(p.h, 1080));
    p = app::game_picture(1920, 1080, 682, 384, true); // "4:3": stretched to 4:3
    CHECK(near(p.w, 1440) && near(p.h, 1080) && near(p.x, 240) && near(p.y, 0));
    p = app::game_picture(800, 1000, 496, 384, true);
    CHECK(near(p.w, 800) && near(p.h, 600) && near(p.y, 200));

    // fading and shrinking with distance
    CHECK(app::CarTagOverlay::distance_alpha(1) == 1.f && app::CarTagOverlay::distance_alpha(60) == 1.f);
    CHECK(near(app::CarTagOverlay::distance_alpha(110), 0.5f) && app::CarTagOverlay::distance_alpha(160) == 0.f);
    CHECK(app::CarTagOverlay::distance_alpha(1000) == 0.f);
    CHECK(app::CarTagOverlay::distance_scale(15) == 1.f && near(app::CarTagOverlay::distance_scale(90), 0.7f));
    CHECK(near(app::CarTagOverlay::distance_scale(1000), 0.7f));

    std::puts("car_tags: ok");
    return 0;
}
