// Vector maths for the free camera, without Windows headers, so tests/cammath_test.cpp
// can check it natively. Y is up; yaw 0 looks along +Z; right is +X there (DirectX's
// left-handed axes, as in the game's camera matrix).
#pragma once
#include <math.h>

namespace pm {

struct Vec3 { float x, y, z; };
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float len(Vec3 a) { return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline Vec3 norm(Vec3 a) {
    float l = len(a);
    return l > 1e-6f ? a * (1 / l) : Vec3{0, 0, 1};
}

inline Vec3 look_dir(float yaw, float pitch) { return {sinf(yaw) * cosf(pitch), sinf(pitch), cosf(yaw) * cosf(pitch)}; }
// The camera's right with roll 0. It comes from yaw alone, so straight up or down is fine.
inline Vec3 yaw_right(float yaw) { return {cosf(yaw), 0, -sinf(yaw)}; }

// Turns two axes in their plane: a toward b by angle t.
inline void turn(Vec3& a, Vec3& b, float t) {
    float ct = cosf(t), st = sinf(t);
    Vec3 a2 = a * ct + b * st, b2 = b * ct + a * (-st);
    a = a2;
    b = b2;
}

// The camera's axes from yaw (about world up), then pitch, then roll (about the view;
// positive lifts the right side).
struct Basis { Vec3 r, u, f; };
inline Basis make_basis(float yaw, float pitch, float roll) {
    Basis b{yaw_right(yaw), {}, look_dir(yaw, pitch)};
    b.u = cross(b.f, b.r);
    if (roll != 0) turn(b.r, b.u, roll);
    return b;
}

// Back from axes to yaw, pitch and roll. Every orientation has them, with pitch within
// +-90 degrees. Looking straight up or down, yaw keeps its value and roll takes the rest.
inline void basis_angles(const Basis& b, float& yaw, float& pitch, float& roll) {
    Vec3 f = norm(b.f);
    pitch = asinf(f.y < -1 ? -1 : f.y > 1 ? 1 : f.y);
    if (f.x * f.x + f.z * f.z > 1e-10f) yaw = atan2f(f.x, f.z);
    Vec3 r0 = yaw_right(yaw), u0 = cross(f, r0);
    roll = atan2f(dot(b.r, u0), dot(b.r, r0));
}

// ---- Seamoth (Subnautica's little sub): momentum and water drag.
// The velocity eases toward the wanted one (stick direction x top speed) with time
// constant SEAMOTH_TAU, solved exactly over each frame, so the path is the same at any
// frame rate. Let go and it glides about top speed x tau before the drag stops it; once
// it's below SEAMOTH_STOP of top speed with no input, it stops dead.
const float SEAMOTH_TAU = 0.5f;        // seconds: reaches 86% of top speed in this time
const float SEAMOTH_STOP = 0.02f;      // fraction of top speed
const float SEAMOTH_LOOK_TAU = 0.08f;  // seconds: the slight lag on the look

inline void seamoth_step(Vec3& pos, Vec3& vel, Vec3 want, float top_speed, float dt) {
    float e = expf(-dt / SEAMOTH_TAU);
    Vec3 d = vel - want;
    pos = pos + want * dt + d * (SEAMOTH_TAU * (1 - e));
    vel = want + d * e;
    bool idle = want.x == 0 && want.y == 0 && want.z == 0;
    if (idle && len(vel) < SEAMOTH_STOP * top_speed) vel = {0, 0, 0};
}

// Eases a value toward a target with time constant tau (frame-rate independent).
inline float ease_to(float cur, float target, float tau, float dt) { return target + (cur - target) * expf(-dt / tau); }

// Flight mode (the Misc panel's row): left/right steps through them and wraps round.
enum Flight { FLIGHT_DEFAULT, FLIGHT_6DOF, FLIGHT_SEAMOTH, FLIGHT_COUNT };
inline int flight_cycle(int f, int dir) { return ((f + dir) % FLIGHT_COUNT + FLIGHT_COUNT) % FLIGHT_COUNT; }
inline const char* flight_name(int f) {
    static const char* const names[FLIGHT_COUNT] = {"Default", "6DOF", "Seamoth"};
    return f >= 0 && f < FLIGHT_COUNT ? names[f] : "?";
}

}  // namespace pm
