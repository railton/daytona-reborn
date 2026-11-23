// Rigid poses stored like the object-trace matrix columns: three basis
// columns (0..8) then translation x, y, z (9..11). Column vectors.
#pragma once

#include <array>
#include <cmath>

namespace rt {

using Pose = std::array<double, 12>;

// rotation is a unit quaternion, x y z w. scale is per basis column.
struct PoseFactors {
    std::array<double, 4> rotation{};
    std::array<double, 3> scale{};
    std::array<double, 3> translation{};
};

namespace pose_detail {
inline double dot3(const double* a, const double* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline void cross3(const double* a, const double* b, double* o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
inline double len3(const double* a) { return std::sqrt(dot3(a, a)); }

inline void mul_rot(const double* a, const double* v, double* o) {
    o[0] = a[0] * v[0] + a[3] * v[1] + a[6] * v[2];
    o[1] = a[1] * v[0] + a[4] * v[1] + a[7] * v[2];
    o[2] = a[2] * v[0] + a[5] * v[1] + a[8] * v[2];
}

inline void mat_mul(const double* a, const double* b, double* o) {
    for (int col = 0; col < 3; ++col) {
        const double v[3] = {b[col * 3], b[col * 3 + 1], b[col * 3 + 2]};
        double r[3];
        mul_rot(a, v, r);
        o[col * 3] = r[0];
        o[col * 3 + 1] = r[1];
        o[col * 3 + 2] = r[2];
    }
}

inline Pose identity() {
    return {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
}

inline void quat_to_rot(const double* q, double* m) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double xx = x * x, yy = y * y, zz = z * z;
    const double xy = x * y, xz = x * z, yz = y * z;
    const double wx = w * x, wy = w * y, wz = w * z;
    m[0] = 1 - 2 * (yy + zz);
    m[1] = 2 * (xy + wz);
    m[2] = 2 * (xz - wy);
    m[3] = 2 * (xy - wz);
    m[4] = 1 - 2 * (xx + zz);
    m[5] = 2 * (yz + wx);
    m[6] = 2 * (xz + wy);
    m[7] = 2 * (yz - wx);
    m[8] = 1 - 2 * (xx + yy);
}

inline void rot_to_quat(const double* m, double* q) {
    const double tr = m[0] + m[4] + m[8];
    if (tr > 0) {
        const double s = std::sqrt(tr + 1) * 2;
        q[3] = 0.25 * s;
        q[0] = (m[5] - m[7]) / s;
        q[1] = (m[6] - m[2]) / s;
        q[2] = (m[1] - m[3]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const double s = std::sqrt(1 + m[0] - m[4] - m[8]) * 2;
        q[3] = (m[5] - m[7]) / s;
        q[0] = 0.25 * s;
        q[1] = (m[1] + m[3]) / s;
        q[2] = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        const double s = std::sqrt(1 + m[4] - m[0] - m[8]) * 2;
        q[3] = (m[6] - m[2]) / s;
        q[0] = (m[1] + m[3]) / s;
        q[1] = 0.25 * s;
        q[2] = (m[5] + m[7]) / s;
    } else {
        const double s = std::sqrt(1 + m[8] - m[0] - m[4]) * 2;
        q[3] = (m[1] - m[3]) / s;
        q[0] = (m[2] + m[6]) / s;
        q[1] = (m[5] + m[7]) / s;
        q[2] = 0.25 * s;
    }
    const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 0) {
        q[0] /= n;
        q[1] /= n;
        q[2] /= n;
        q[3] /= n;
    } else {
        q[0] = q[1] = q[2] = 0;
        q[3] = 1;
    }
    if (q[3] < 0) {
        q[0] = -q[0];
        q[1] = -q[1];
        q[2] = -q[2];
        q[3] = -q[3];
    }
}

inline void so3_exp(const double* r, double* m) {
    const double th2 = dot3(r, r);
    if (th2 < 1e-16) {
        const double x = r[0], y = r[1], z = r[2];
        m[0] = 1;
        m[1] = z;
        m[2] = -y;
        m[3] = -z;
        m[4] = 1;
        m[5] = x;
        m[6] = y;
        m[7] = -x;
        m[8] = 1;
        for (int col = 0; col < 3; ++col) {
            double e[3] = {};
            e[col] = 1;
            double c[3], c2[3];
            cross3(r, e, c);
            cross3(r, c, c2);
            m[col * 3] += 0.5 * c2[0];
            m[col * 3 + 1] += 0.5 * c2[1];
            m[col * 3 + 2] += 0.5 * c2[2];
        }
        return;
    }
    const double th = std::sqrt(th2);
    const double s = std::sin(th) / th;
    const double c = (1 - std::cos(th)) / th2;
    for (int col = 0; col < 3; ++col) {
        double e[3] = {};
        e[col] = 1;
        double kv[3], k2[3];
        cross3(r, e, kv);
        cross3(r, kv, k2);
        m[col * 3] = e[0] + s * kv[0] + c * k2[0];
        m[col * 3 + 1] = e[1] + s * kv[1] + c * k2[1];
        m[col * 3 + 2] = e[2] + s * kv[2] + c * k2[2];
    }
}

inline void so3_log(const double* m, double* r) {
    const double cosine = (m[0] + m[4] + m[8] - 1) * 0.5;
    const double clamped = cosine < -1 ? -1 : (cosine > 1 ? 1 : cosine);
    const double x = m[5] - m[7];
    const double y = m[6] - m[2];
    const double z = m[1] - m[3];
    const double sine = 0.5 * std::sqrt(x * x + y * y + z * z);
    const double theta = std::atan2(sine, clamped);
    if (sine < 1e-12) {
        r[0] = 0.5 * x;
        r[1] = 0.5 * y;
        r[2] = 0.5 * z;
        return;
    }
    const double scale = theta / (2 * sine);
    r[0] = scale * x;
    r[1] = scale * y;
    r[2] = scale * z;
}

inline void se3_log(const double* rot, const double* t, double* r, double* v) {
    so3_log(rot, r);
    const double th2 = dot3(r, r);
    double c[3], c2[3];
    cross3(r, t, c);
    cross3(r, c, c2);
    double beta = 1.0 / 12.0;
    if (th2 >= 1e-16) {
        const double th = std::sqrt(th2);
        beta = (1 - (th * std::sin(th)) / (2 * (1 - std::cos(th)))) / th2;
    }
    v[0] = t[0] - 0.5 * c[0] + beta * c2[0];
    v[1] = t[1] - 0.5 * c[1] + beta * c2[1];
    v[2] = t[2] - 0.5 * c[2] + beta * c2[2];
}

inline void se3_exp(const double* r, const double* v, double* rot, double* t) {
    so3_exp(r, rot);
    const double th2 = dot3(r, r);
    double c[3], c2[3];
    cross3(r, v, c);
    cross3(r, c, c2);
    double a = 0.5, b = 1.0 / 6.0;
    if (th2 >= 1e-16) {
        const double th = std::sqrt(th2);
        a = (1 - std::cos(th)) / th2;
        b = (th - std::sin(th)) / (th2 * th);
    }
    t[0] = v[0] + a * c[0] + b * c2[0];
    t[1] = v[1] + a * c[1] + b * c2[1];
    t[2] = v[2] + a * c[2] + b * c2[2];
}

inline void slerp(const double* a, const double* b_in, double alpha, double* o) {
    double b[4] = {b_in[0], b_in[1], b_in[2], b_in[3]};
    double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0) {
        b[0] = -b[0];
        b[1] = -b[1];
        b[2] = -b[2];
        b[3] = -b[3];
        d = -d;
    }
    double w0, w1;
    if (d > 0.9995) {
        w0 = 1 - alpha;
        w1 = alpha;
    } else {
        const double theta = std::acos(d < 1 ? d : 1);
        const double s = std::sin(theta);
        w0 = std::sin((1 - alpha) * theta) / s;
        w1 = std::sin(alpha * theta) / s;
    }
    o[0] = w0 * a[0] + w1 * b[0];
    o[1] = w0 * a[1] + w1 * b[1];
    o[2] = w0 * a[2] + w1 * b[2];
    o[3] = w0 * a[3] + w1 * b[3];
    const double n = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2] + o[3] * o[3]);
    if (n > 0) {
        o[0] /= n;
        o[1] /= n;
        o[2] /= n;
        o[3] /= n;
    }
}
} // namespace pose_detail

// a after b. Column vectors: apply b, then a.
inline Pose compose(const Pose& a, const Pose& b) {
    Pose o;
    pose_detail::mat_mul(a.data(), b.data(), o.data());
    double tb[3] = {b[9], b[10], b[11]};
    double rotated[3];
    pose_detail::mul_rot(a.data(), tb, rotated);
    o[9] = rotated[0] + a[9];
    o[10] = rotated[1] + a[10];
    o[11] = rotated[2] + a[11];
    return o;
}

// Inverse of a rigid pose with one uniform scale (positive or negative).
inline Pose inverse(const Pose& a) {
    const double det = a[0] * (a[4] * a[8] - a[7] * a[5]) - a[3] * (a[1] * a[8] - a[7] * a[2]) +
                       a[6] * (a[1] * a[5] - a[4] * a[2]);
    const double s = std::cbrt(det);
    const double inv_s2 = 1 / (s * s);
    Pose o;
    o[0] = a[0] * inv_s2;
    o[1] = a[3] * inv_s2;
    o[2] = a[6] * inv_s2;
    o[3] = a[1] * inv_s2;
    o[4] = a[4] * inv_s2;
    o[5] = a[7] * inv_s2;
    o[6] = a[2] * inv_s2;
    o[7] = a[5] * inv_s2;
    o[8] = a[8] * inv_s2;
    double t[3] = {a[9], a[10], a[11]};
    double rt[3];
    pose_detail::mul_rot(o.data(), t, rt);
    o[9] = -rt[0];
    o[10] = -rt[1];
    o[11] = -rt[2];
    return o;
}

inline PoseFactors decompose(const Pose& m) {
    PoseFactors f;
    double c0[3] = {m[0], m[1], m[2]};
    double c1[3] = {m[3], m[4], m[5]};
    double c2[3] = {m[6], m[7], m[8]};
    f.scale = {pose_detail::len3(c0), pose_detail::len3(c1), pose_detail::len3(c2)};
    if (f.scale[0] > 0) {
        c0[0] /= f.scale[0];
        c0[1] /= f.scale[0];
        c0[2] /= f.scale[0];
    }
    if (f.scale[1] > 0) {
        c1[0] /= f.scale[1];
        c1[1] /= f.scale[1];
        c1[2] /= f.scale[1];
    }
    if (f.scale[2] > 0) {
        c2[0] /= f.scale[2];
        c2[1] /= f.scale[2];
        c2[2] /= f.scale[2];
    }
    double n[3];
    pose_detail::cross3(c0, c1, n);
    if (pose_detail::dot3(n, c2) < 0) {
        f.scale[2] = -f.scale[2];
        c2[0] = -c2[0];
        c2[1] = -c2[1];
        c2[2] = -c2[2];
    }
    double rot[9] = {c0[0], c0[1], c0[2], c1[0], c1[1], c1[2], c2[0], c2[1], c2[2]};
    pose_detail::rot_to_quat(rot, f.rotation.data());
    f.translation = {m[9], m[10], m[11]};
    return f;
}

inline Pose recompose(const PoseFactors& f) {
    double rot[9];
    pose_detail::quat_to_rot(f.rotation.data(), rot);
    Pose o{};
    for (int col = 0; col < 3; ++col) {
        o[col * 3] = rot[col * 3] * f.scale[col];
        o[col * 3 + 1] = rot[col * 3 + 1] * f.scale[col];
        o[col * 3 + 2] = rot[col * 3 + 2] * f.scale[col];
    }
    o[9] = f.translation[0];
    o[10] = f.translation[1];
    o[11] = f.translation[2];
    return o;
}

// Fractional power of a rigid displacement (scale 1). Screw motion:
// the axis stays, the angle and the twist translation scale by s.
inline Pose rigid_power(const Pose& d, double s) {
    if (s == 0) return pose_detail::identity();
    if (s == 1) return d;
    double r[3], v[3], rot[9], t[3];
    const double trans[3] = {d[9], d[10], d[11]};
    pose_detail::se3_log(d.data(), trans, r, v);
    r[0] *= s;
    r[1] *= s;
    r[2] *= s;
    v[0] *= s;
    v[1] *= s;
    v[2] *= s;
    pose_detail::se3_exp(r, v, rot, t);
    Pose o;
    for (int i = 0; i < 9; ++i) o[i] = rot[i];
    o[9] = t[0];
    o[10] = t[1];
    o[11] = t[2];
    return o;
}

// Shortest-path slerp on rotation. Linear scale and translation.
inline Pose interp(const Pose& a, const Pose& b, double alpha) {
    if (alpha == 0) return a;
    if (alpha == 1) return b;
    const PoseFactors fa = decompose(a);
    const PoseFactors fb = decompose(b);
    PoseFactors f;
    pose_detail::slerp(fa.rotation.data(), fb.rotation.data(), alpha, f.rotation.data());
    for (int i = 0; i < 3; ++i) {
        f.scale[i] = (1 - alpha) * fa.scale[i] + alpha * fb.scale[i];
        f.translation[i] = (1 - alpha) * fa.translation[i] + alpha * fb.translation[i];
    }
    return recompose(f);
}

// M(alpha) = D^(alpha-1) after m_next.
inline Pose scenery_pose(const Pose& d, const Pose& m_next, double alpha) {
    return compose(rigid_power(d, alpha - 1), m_next);
}

// Own motion of a keyed object, then the same scenery retreat.
inline Pose mobile_pose(const Pose& d, const Pose& m_prev, const Pose& m_next, double alpha) {
    return compose(rigid_power(d, alpha - 1), interp(compose(d, m_prev), m_next, alpha));
}

// The same, with the translation on a cubic Hermite curve between the two
// poses (in m_next's frame): ta and tb are its velocities, per interval, at
// the start and the end. With ta = tb = the step it is mobile_pose exactly.
// Given the neighbouring snapshots, a pair's velocity at the shared tick
// matches the next pair's, so a car no longer changes speed at each tick.
inline Pose mobile_pose_hermite(const Pose& d, const Pose& m_prev, const Pose& m_next, double alpha,
                                const std::array<double, 3>& ta, const std::array<double, 3>& tb) {
    const Pose start = compose(d, m_prev);
    Pose own = interp(start, m_next, alpha);
    const double t = alpha, t2 = t * t, t3 = t2 * t;
    const double h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t, h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
    for (int i = 0; i < 3; ++i)
        own[9 + i] = alpha > 1 ? m_next[9 + i] + (alpha - 1) * tb[i] // past the end: along the last velocity
                               : h00 * start[9 + i] + h10 * ta[i] + h01 * m_next[9 + i] + h11 * tb[i];
    return compose(rigid_power(d, alpha - 1), own);
}

} // namespace rt
