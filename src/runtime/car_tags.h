// Online play: where each linked player's car is on the screen, for the
// name tags over the race (app/online_overlay.h).
//
// Revision A keeps one 0x300-byte record per car the race draws as an
// opponent, from 0x505480 (the MobileIdentity slots, mobile_identity.h;
// the cabinet's own car is the record just before, 0x505180). In a linked
// race the byte at +0x94 of a record is the CAR NUMBER - 1 of the cabinet
// driving that car, 8 for a computer car. Measured 23/11 on 3 and 4 linked
// cabinets (m2run --relay): every record's byte held for the whole race,
// each cabinet's own record held its own number, and each linked car's
// colour on the screen matched its number's (app::online_car_colour).
//
// The geometrizer keys every polygon of those cars with its record
// (GeoPoly::mobile_key.slot), so a car's tag goes where its polygons are
// drawn, projected as the renderer projects them (gpu_renderer.cpp):
// widescreen, the four camera views and the interpolated poses included,
// and a car behind the camera or beyond the draw distance has no polygons.
// Whether it is hidden is read from the drawing order: the hardware draws
// front to back, the earlier polygon winning a pixel (gpu_renderer.cpp, its
// depth test), so a point of the car is covered when an earlier opaque
// polygon of the same window contains it.
#pragma once

#include "runtime/geo.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace rt::online {

constexpr uint32_t kCarRecords = 0x505480, kOwnCarRecord = 0x505180, kCarRecordSize = 0x300;
constexpr uint32_t kCarNumberOffset = 0x94;

// The linked CAR NUMBER (1..8) driving a record, 0 for a computer car.
template <class Board> int linked_car(Board &board, unsigned slot) {
    if (slot >= MobileIdentity::slot_count) return 0;
    const int value = board.read_byte(kCarRecords + slot * kCarRecordSize + kCarNumberOffset);
    return value < 8 ? value + 1 : 0;
}

struct CarTag {
    int car = 0;                         // CAR NUMBER, 1..8
    float x = 0, top = 0, bottom = 0;    // the outline's centre, top and bottom, in screen pixels
    float width = 0;                     // the outline's width
    float depth = 0;                     // the nearest vertex, camera units
    float visible = 0;                   // share of the sampled points not covered, 0..1
};

namespace car_tags_detail {
struct Point {
    float x, y;
};

inline Point project(const GeoPoly &poly, const GeoVertex &v, int origin_x, int origin_y) {
    // model2_3d_project, as GpuRenderer::build
    const float z = v.p[0];
    return {float(origin_x + poly.center[0]) + v.x / z, float(384 - poly.center[1] + origin_y) - v.y / z};
}

// Convex polygon (either winding) contains p.
inline bool contains(const Point *v, int n, Point p) {
    int sign = 0;
    for (int i = 0; i < n; ++i) {
        const Point a = v[i], b = v[(i + 1) % n];
        const float cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (cross == 0) continue;
        const int s = cross > 0 ? 1 : -1;
        if (sign && s != sign) return false;
        sign = s;
    }
    return true;
}

inline bool opaque(const GeoPoly &poly) {
    const int renderer = (poly.texheader[0] >> 13) & 3; // 1: draws nothing; 3: cut-out texture
    return renderer == 0 || renderer == 2;
}
} // namespace car_tags_detail

// The linked cars drawn in a frame's polygons. car_of(slot) gives a record's
// CAR NUMBER (linked_car), 0 to leave it out. origin: the renderer's
// crtc_x + widescreen margin, crtc_y. Up to `samples` polygons of each car
// (its largest) are tested for cover.
template <class CarOf>
std::vector<CarTag> car_tags(const std::vector<GeoPoly> &polys, int origin_x, int origin_y, CarOf car_of,
                             int samples = 8) {
    using namespace car_tags_detail;
    struct Car {
        int car = 0;
        float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, depth = 1e30f;
        std::vector<std::pair<float, size_t>> faces; // area, polygon
    };
    std::array<Car, MobileIdentity::slot_count> cars;
    std::array<bool, MobileIdentity::slot_count> asked{};
    for (size_t i = 0; i < polys.size(); ++i) {
        const auto &poly = polys[i];
        if (!poly.mobile_key.part || poly.num_vertices < 3 || Geo::hud_overlay_address(poly.object_oba)) continue;
        const auto slot = MobileIdentity::slot_index(poly.mobile_key.slot);
        if (!slot) continue;
        auto &car = cars[*slot];
        if (!asked[*slot]) asked[*slot] = true, car.car = car_of(*slot);
        if (car.car < 1 || car.car > 8) continue;
        Point v[8];
        bool front = true;
        for (int k = 0; k < poly.num_vertices; ++k) front = front && poly.v[k].p[0] > 0;
        if (!front) continue;
        float area = 0;
        for (int k = 0; k < poly.num_vertices; ++k) {
            v[k] = project(poly, poly.v[k], origin_x, origin_y);
            car.x0 = std::min(car.x0, v[k].x), car.x1 = std::max(car.x1, v[k].x);
            car.y0 = std::min(car.y0, v[k].y), car.y1 = std::max(car.y1, v[k].y);
            car.depth = std::min(car.depth, poly.v[k].p[0]);
        }
        for (int k = 0; k < poly.num_vertices; ++k) {
            const Point a = v[k], b = v[(k + 1) % poly.num_vertices];
            area += a.x * b.y - b.x * a.y;
        }
        car.faces.push_back({std::abs(area) * 0.5f, i});
    }

    std::vector<CarTag> tags;
    // every polygon's outline on the screen, once (x0, x1, y0, y1; empty if
    // it reaches behind the eye)
    std::vector<std::array<float, 4>> boxes;
    for (auto &car : cars) {
        if (car.faces.empty()) continue;
        if (boxes.empty()) {
            boxes.resize(polys.size(), {1, 0, 1, 0});
            for (size_t j = 0; j < polys.size(); ++j) {
                const auto &other = polys[j];
                std::array<float, 4> box{1e30f, -1e30f, 1e30f, -1e30f};
                bool front = other.num_vertices >= 3;
                for (int k = 0; k < other.num_vertices && front; ++k) {
                    front = other.v[k].p[0] > 0;
                    const Point q = front ? project(other, other.v[k], origin_x, origin_y) : Point{0, 0};
                    box = {std::min(box[0], q.x), std::max(box[1], q.x), std::min(box[2], q.y), std::max(box[3], q.y)};
                }
                if (front) boxes[j] = box;
            }
        }
        std::sort(car.faces.begin(), car.faces.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        if (int(car.faces.size()) > samples) car.faces.resize(size_t(samples));
        int seen = 0;
        for (const auto &[area, index] : car.faces) {
            const auto &face = polys[index];
            Point p{0, 0};
            for (int k = 0; k < face.num_vertices; ++k) {
                const Point q = project(face, face.v[k], origin_x, origin_y);
                p.x += q.x / face.num_vertices, p.y += q.y / face.num_vertices;
            }
            bool covered = false;
            for (size_t j = 0; j < polys.size() && !covered; ++j) {
                const auto &box = boxes[j];
                if (p.x < box[0] || p.x > box[1] || p.y < box[2] || p.y > box[3]) continue;
                const auto &other = polys[j];
                // drawn earlier: the same window, nearer, or as near and added later
                if (other.window != face.window) continue;
                if (!(other.z < face.z || (other.z == face.z && j > index))) continue;
                if (other.mobile_key.part && other.mobile_key.slot == face.mobile_key.slot) continue;
                if (!opaque(other) || Geo::hud_overlay_address(other.object_oba)) continue;
                Point v[8];
                for (int k = 0; k < other.num_vertices; ++k) v[k] = project(other, other.v[k], origin_x, origin_y);
                covered = contains(v, other.num_vertices, p);
            }
            seen += !covered;
        }
        CarTag tag;
        tag.car = car.car;
        tag.x = (car.x0 + car.x1) * 0.5f;
        tag.top = car.y0;
        tag.bottom = car.y1;
        tag.width = car.x1 - car.x0;
        tag.depth = car.depth;
        tag.visible = float(seen) / float(car.faces.size());
        tags.push_back(tag);
    }
    return tags;
}

} // namespace rt::online
