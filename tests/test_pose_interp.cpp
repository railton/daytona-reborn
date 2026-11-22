// Pose interpolation. No ROM and no window.

#include "runtime/pose_interp.h"

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
constexpr double kTol = 1e-9;

void check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "pose interp: %s\n", message);
        ++failures;
    }
}

double dot(const double* a, const double* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
void cross(const double* a, const double* b, double* o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
double len(const double* a) { return std::sqrt(dot(a, a)); }

bool near_pose(const rt::Pose& a, const rt::Pose& b, double tol) {
    for (int i = 0; i < 12; ++i)
        if (!(std::fabs(a[i] - b[i]) <= tol)) return false;
    return true;
}
bool finite_pose(const rt::Pose& a) {
    for (double v : a)
        if (!std::isfinite(v)) return false;
    return true;
}

rt::Pose identity() { return {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}; }

rt::Pose translation(double x, double y, double z) {
    rt::Pose p = identity();
    p[9] = x;
    p[10] = y;
    p[11] = z;
    return p;
}

rt::Pose rotation_about(const double axis[3], double angle, const double center[3]) {
    const double n = len(axis);
    const double u[3] = {axis[0] / n, axis[1] / n, axis[2] / n};
    const double c = std::cos(angle), s = std::sin(angle);
    const double k = 1 - c;
    rt::Pose p{};
    p[0] = c + u[0] * u[0] * k;
    p[1] = u[1] * u[0] * k + u[2] * s;
    p[2] = u[2] * u[0] * k - u[1] * s;
    p[3] = u[0] * u[1] * k - u[2] * s;
    p[4] = c + u[1] * u[1] * k;
    p[5] = u[2] * u[1] * k + u[0] * s;
    p[6] = u[0] * u[2] * k + u[1] * s;
    p[7] = u[1] * u[2] * k - u[0] * s;
    p[8] = c + u[2] * u[2] * k;
    double rc[3] = {
        p[0] * center[0] + p[3] * center[1] + p[6] * center[2],
        p[1] * center[0] + p[4] * center[1] + p[7] * center[2],
        p[2] * center[0] + p[5] * center[1] + p[8] * center[2],
    };
    p[9] = center[0] - rc[0];
    p[10] = center[1] - rc[1];
    p[11] = center[2] - rc[2];
    return p;
}

void apply(const rt::Pose& m, const double* p, double* o) {
    o[0] = m[0] * p[0] + m[3] * p[1] + m[6] * p[2] + m[9];
    o[1] = m[1] * p[0] + m[4] * p[1] + m[7] * p[2] + m[10];
    o[2] = m[2] * p[0] + m[5] * p[1] + m[8] * p[2] + m[11];
}

double axis_distance(const double* point, const double* center, const double* axis) {
    double v[3] = {point[0] - center[0], point[1] - center[1], point[2] - center[2]};
    const double along = dot(v, axis);
    double radial[3] = {v[0] - axis[0] * along, v[1] - axis[1] * along, v[2] - axis[2] * along};
    return len(radial);
}
}

int main() {
    const rt::Pose eye = identity();
    const double axis[3] = {0, 0.6, 0.8};
    const double unit_axis[3] = {0, 0.6, 0.8};
    const double center[3] = {2, -1, 0.5};
    const double ten = 10 * 3.14159265358979323846 / 180;
    const rt::Pose spin = rotation_about(axis, ten, center);
    const rt::Pose shift = translation(4, -2, 1);
    const rt::Pose placed = translation(0.5, 1.5, -0.25);

    check(near_pose(rt::rigid_power(spin, 0), eye, kTol), "rigid_power 0");
    check(near_pose(rt::rigid_power(spin, 1), spin, kTol), "rigid_power 1");
    check(near_pose(rt::compose(rt::inverse(spin), spin), eye, kTol), "inverse then pose");
    check(near_pose(rt::compose(spin, rt::inverse(spin)), eye, kTol), "pose then inverse");
    check(near_pose(rt::rigid_power(spin, -1), rt::inverse(spin), kTol), "power -1 is inverse");

    check(near_pose(rt::compose(rt::rigid_power(spin, 0.3), rt::rigid_power(spin, 0.4)),
                    rt::rigid_power(spin, 0.7), kTol),
          "additivity 0.3+0.4");
    check(near_pose(rt::compose(rt::rigid_power(spin, -0.5), rt::rigid_power(spin, 0.25)),
                    rt::rigid_power(spin, -0.25), kTol),
          "additivity negative");
    check(near_pose(rt::rigid_power(shift, 0.25), translation(1, -0.5, 0.25), kTol),
          "pure translation power");

    const rt::PoseFactors parts = rt::decompose(spin);
    check(near_pose(rt::recompose(parts), spin, kTol), "recompose rotation");
    rt::Pose stretched = spin;
    for (int i = 0; i < 3; ++i) stretched[i] *= 2;
    for (int i = 3; i < 6; ++i) stretched[i] *= 0.5;
    for (int i = 6; i < 9; ++i) stretched[i] *= 1.5;
    check(near_pose(rt::recompose(rt::decompose(stretched)), stretched, kTol), "recompose scale");

    check(near_pose(rt::scenery_pose(spin, placed, 1), placed, kTol), "scenery alpha 1");
    check(near_pose(rt::scenery_pose(spin, placed, 0), rt::compose(rt::inverse(spin), placed), kTol),
          "scenery alpha 0");
    check(near_pose(rt::mobile_pose(spin, placed, shift, 0), placed, kTol), "mobile alpha 0");
    check(near_pose(rt::mobile_pose(spin, placed, shift, 1), shift, kTol), "mobile alpha 1");

    const rt::Pose fixed_next = rt::compose(spin, placed);
    const double alphas[] = {0, 0.25, 0.5, 0.75, 1};
    for (double alpha : alphas)
        check(near_pose(rt::mobile_pose(spin, placed, fixed_next, alpha),
                        rt::scenery_pose(spin, fixed_next, alpha), kTol),
              "fixed object matches scenery");

    rt::Pose straight[5];
    for (int i = 0; i < 5; ++i) straight[i] = rt::scenery_pose(shift, eye, alphas[i]);
    double step[3] = {straight[1][9] - straight[0][9], straight[1][10] - straight[0][10],
                      straight[1][11] - straight[0][11]};
    for (int i = 1; i < 4; ++i) {
        double got[3] = {straight[i + 1][9] - straight[i][9], straight[i + 1][10] - straight[i][10],
                         straight[i + 1][11] - straight[i][11]};
        check(std::fabs(got[0] - step[0]) <= kTol && std::fabs(got[1] - step[1]) <= kTol &&
                  std::fabs(got[2] - step[2]) <= kTol,
              "scenery translation equidistant");
    }
    const rt::Pose mobile_start = translation(1, 2, 3);
    const rt::Pose mobile_end = translation(5, -2, 7);
    rt::Pose line[5];
    for (int i = 0; i < 5; ++i)
        line[i] = rt::mobile_pose(eye, mobile_start, mobile_end, alphas[i]);
    double mstep[3] = {line[1][9] - line[0][9], line[1][10] - line[0][10], line[1][11] - line[0][11]};
    for (int i = 0; i < 5; ++i) {
        double expect[3] = {mobile_start[9] + alphas[i] * (mobile_end[9] - mobile_start[9]),
                            mobile_start[10] + alphas[i] * (mobile_end[10] - mobile_start[10]),
                            mobile_start[11] + alphas[i] * (mobile_end[11] - mobile_start[11])};
        check(std::fabs(line[i][9] - expect[0]) <= kTol && std::fabs(line[i][10] - expect[1]) <= kTol &&
                  std::fabs(line[i][11] - expect[2]) <= kTol,
              "mobile translation aligned");
        (void)mstep;
    }

    const double point[3] = {2, 1, 0.5};
    double origin_d = 0;
    {
        double at[3];
        apply(eye, point, at);
        origin_d = axis_distance(point, center, unit_axis);
        (void)at;
    }
    const double powers[] = {-0.5, 0, 0.25, 0.5, 1, 1.5};
    for (double s : powers) {
        double at[3];
        apply(rt::rigid_power(spin, s), point, at);
        check(std::fabs(axis_distance(at, center, unit_axis) - origin_d) <= kTol, "arc distance");
        check(finite_pose(rt::rigid_power(spin, s)), "finite power");
    }
    for (double alpha : alphas) {
        double at[3];
        apply(rt::scenery_pose(spin, eye, alpha), point, at);
        check(std::fabs(axis_distance(at, center, unit_axis) - origin_d) <= kTol, "scenery arc");
    }

    const double almost = 1e-9;
    const rt::Pose tiny = rotation_about(axis, almost, center);
    const double deg179 = 179 * 3.14159265358979323846 / 180;
    const rt::Pose wide = rotation_about(axis, deg179, center);
    check(finite_pose(rt::rigid_power(tiny, 0.5)) && finite_pose(rt::interp(eye, tiny, 0.5)),
          "near-zero rotation finite");
    check(finite_pose(rt::rigid_power(wide, 0.5)) && finite_pose(rt::interp(eye, wide, 0.5)) &&
              finite_pose(rt::scenery_pose(wide, placed, 0.5)),
          "179 degree rotation finite");
    check(near_pose(rt::compose(rt::rigid_power(wide, 0.4), rt::rigid_power(wide, 0.4)),
                    rt::rigid_power(wide, 0.8), kTol),
          "179 degree additivity");

    const rt::Pose from = rotation_about(unit_axis, 170 * 3.14159265358979323846 / 180, center);
    const rt::Pose to = rotation_about(unit_axis, -170 * 3.14159265358979323846 / 180, center);
    const rt::Pose mid = rt::interp(from, to, 0.5);
    const double trace = mid[0] + mid[4] + mid[8];
    check(trace < -0.5, "shortest path passes near 180 degrees");

    if (failures) return 1;
    std::puts("pose interp: endpoints, screw additivity, straight line and arc passed");
    return 0;
}
